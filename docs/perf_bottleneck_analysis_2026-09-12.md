# Performance bottleneck analysis — 2026-09-12

Machine: RX 7900 XT (gfx1100), Windows, big2cater/ZLUDA @ 1d47bf4 (initial pass) through 5ac9102+ (fp8-inline build; later sections).
Workload: DLSS-NR (nvngx_dlssnr 310.8.0, CG2R backbone `crazy-cuckoo`), 640×360,
temporal path, WGP mode (fork default). All numbers from `build/framebench`
(start + N frames, per-frame `last_ms`) and `DLSSNR_PHASE_TIMING=1`.

## Where a frame's milliseconds go

| measurement | result |
|---|---|
| steady-state frame (640×360) | 83.5 ms (pre-v5; 78.4 after the fp8-inline) |
| ... at 320×180 (¼ pixels) | 79.8 ms |
| ... at 160×90 (1/16 pixels) | 74.9 ms |
| phase split (steady frame) | upload 0.3 ms, evaluate 82 ms, readback 0.4 ms |
| CPU usage during evaluate | ~101 % of one core |
| GPU Compute engine during evaluate | ~98 % |
| D3D12 upload+readback cost | < 1 ms total — irrelevant |

The D3D12 staging pipeline (upload, barriers, fence waits, readback Map) costs
under a millisecond per frame; optimizing it was a dead end. Everything happens
inside `EvaluateFeature`.

## What evaluate is NOT bound by

- **Launch-path CPU cost.** zluda_trace counts ~156 `cuLaunchKernel` per frame.
  A microbenchmark (`build/launchbench`, tiny PTX module) measures ZLUDA's
  launch at **0.6 µs** and an idle `cuCtxSynchronize` at 0.1 µs → ~0.1 ms/frame
  in launches. Not the bottleneck.
- **Weight upload.** The 153-tensor, 140.9 MB weight heap is uploaded **once**
  at init (trace: all HtoD copies sit before the first context sync). Not
  per-frame.
- **Cold cache / precompile.** Fixed by the warm-start stamp (see
  `precompile.cpp`); hot start is ~0.6 s.
- **CU vs WGP mode.** A/B measured on this machine: WGP (default) 84.4 ms
  steady, CU 97.8 ms — WGP is already the right choice, keep it.
- **VOPD dual-issue / WMMA share.** `tools/analyze_vopd.py` on the compiled
  cache: 6.9 % dual-issued (86 % of duals are `v_dual_mov_b32`), WMMA is 0.5 %
  of vector ops. Micro-architectural, but it cannot explain the numbers below
  *by itself* — see the evening revision for why the WMMA path is nevertheless
  the main remaining lever.

## What it IS bound by

*(superseded 2026-09-13: the "latency-bound weight streaming" hypothesis below
was the leading candidate from the first pass — the evening revision identifies
MMA pad/split scaffolding as the dominant term. Read on before acting on it.)*

The GPU Compute engine is 98 % busy while CPU spins in `cuCtxSynchronize` —
evaluate is GPU-bound. But the kernel work is astonishingly small:

- 156 launches per frame, total **~770 K threads = ~24 K wave32s** (640×360).
  The 7900 XT has 84 CUs; average grid is 12–24 waves — the shader units are
  mostly idle while the engine timeline is occupied.
- Kernel grids barely change with resolution (the most common grids are
  (6,2)·32 threads at 640×360 vs (4,2)·32 at 160×90), and total time does not
  either: **~0.5 ms per kernel, independent of grid size and pixel count.**

So the cost is a fixed per-kernel GPU latency (~850 K cycles per wave at
boost clock) × 156 kernels. On NVIDIA the same network runs this resolution
in single-digit ms, so the per-kernel latency is specific to how the fork's
compiled code executes on RDNA3 — most plausibly latency-bound weight streaming
(≈0.9 MB of weights per kernel read serially by a single wave) and/or
dependency chains in the compiled conv code, not any per-launch software
overhead (that is 0.6 µs) and not the `s_dcache.inv` prelude (one instruction).

## Recommended next steps

1. ~~**Per-kernel GPU timing from inside the fork**~~ — **implemented and closed.**
   Implemented via `ZLUDA_LAUNCH_TIMING=1` in `zluda/src/impl/function.rs` (event pair per launch). Measured kernel GPU time at 80.6 ms/frame and identified the top serialized attention kernels.
2. ~~**Chase the single-wave latency**~~ — superseded the same way as the
   section above: the evening revision reframes the per-kernel latency cause
   as MMA pad/split scaffolding. The ISA checks listed here are still worth
   one pass when Route A lands; `tools/analyze_vopd.py` is the toolkit.
3. ~~**More waves per kernel / grid sizing**~~ — **verified and closed.**
   Inspection of the snippet host code shows it queries only 4 basic device attributes (`CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK`, `WARP_SIZE`, `MAX_SHARED_MEMORY_PER_BLOCK`, `COMPUTE_CAPABILITY_MAJOR/MINOR`). The (6,2) grid reflects Swin transformer's own window partitioning (patch/window spatial layout), not ZLUDA under-reporting attributes.

## Per-kernel timing results (2026-09-12, later the same day)

Implemented `ZLUDA_LAUNCH_TIMING=1` in the fork (`zluda/src/impl/function.rs`):
an event pair per launch, kernel GPU duration on stderr, plus the module
symbol name (`Function` now carries it). `ZLUDA_CUMODE` A/B meanwhile: WGP
84.4 ms vs CU 97.8 ms steady at 640×360 — keep WGP.

Findings:

- Launch GPU time totals **80.6 ms/frame** — exactly the evaluate cost, so
  the serialized kernels are the whole story.
- Only **~10 distinct kernels** run per frame, each launched 2–16 times.
  All are the network's ViT/Swin attention layers in **fp8** variants
  (`cc_split_swin_16h_qkv_512_chained_fp8`, `cc_vit_1d_ffn_contract/expand`,
  `cc_tinlayout_fused_swin_8h_256_8`, ...). Top kernel (16 launches/frame,
  grid 3×2×32 = 192 threads = 6 waves): 0.6 ms each → 9.5 ms/frame by itself.
- GPU is shader-idle inside every launch: 6–32 waves on 84 CUs, duration
  independent of grid size. This is single-wave latency, not throughput.

ISA of the top kernel (`cc_split_swin_16h_qkv_512_chained_fp8`, module 14,
28 220 instructions):

- `vgpr_count: 192` (at the allocator's occupancy-driven cap) with
  **`vgpr_spill_count: 118–184`** across the module's kernels — every thread
  spills 118–184 register slots into scratch (global) memory, and the static
  code has ~400 `scratch_load` + ~400 `scratch_store` sites.
- **13.8 % of all instructions are `s_delay_alu`** — the scheduler papering
  over long dependency chains it cannot fill.
- fp8 is emulated: no native fp8 ALU on gfx11 — every fp8 op goes through
  `v_perm_b32`/`v_bfi_b32` unpack + `v_pk_mul_f16`/`v_pk_add_f16` +
  `v_cvt_f16_f32` repack (~20 % of the code). *(Note: an earlier draft assumed WMMA was absent. Later cache disassembly confirmed 24,319 `v_wmma_f32_16x16x16_f16` are present — they are **unpaired and burdened by pad/split scaffolding**, not missing. See the evening revision; the "~327 ops per WMMA" figure sometimes quoted is a whole-cache dilution ratio, not MMA-local cost)*.
- 512 `s_swappc_b64` call sites — a mix of the "chained" network layer calls
  and the non-inlined mma helpers; see Step 0, which shows the mma helper
  calls are blocked by a surviving `noinline` attribute.
- The PTX carries **no tuning directives at all** (no `.maxnreg`,
  `.minnctapersm`, `.reqntid`), so neither the fork's `amdgpu-num-vgpr`
  mapping nor occupancy directives cause the cap; it is LLVM's own occupancy
  heuristic (`MaxNumVGPRs = min(TotalVGPRs / WavesPerEU, 256)`, gfx11
  wave32 addressable max = 256).

Levers, in order of expected value:

*(superseded 2026-09-13: item 1 is closed by the A/B below; items 2–3 remain
separate live levers — the project proposal further down adds the mma-pairing
lever on top of them, it does not absorb them. Kept for the measurement
record.)*

1. ~~**Raise the VGPR budget**~~ — **tested and closed.** `ZLUDA_NUM_VGPR=256`
   (implemented with cache-key isolation) recompiled every module: gfx11
   wave32's addressable VGPR file is 192 per thread, LLVM was already at the
   ceiling, and the spills are structural. Measured: launch total 82.5 ms vs
   80.6 baseline, steady 98.2 vs 94.7 with the same event overhead — no gain,
   the spilled 2149-slot kernels got no faster. The ~0.5 ms per kernel
   survives register-allocation changes, so spills are not the bottleneck.
2. **Reduce fp8 emulation cost** in the fork's PTX lowering (batch the
   unpack/repack, or widen to f16 early) — attacks the ~20 % perm/bfi/cvt
   tax and the register pressure behind it.
3. **Inline the chained layer calls** if LLVM kept them as calls for
   register-pressure reasons; forcing inlining raises pressure but removes
   per-call stack traffic — needs measurement, direction unknown.

Hard constraints worth stating plainly (corrected per RDNA3 ISA specs): each
SIMD32 unit holds 1536 VGPRs; the per-thread architectural limit is 256, with
12-register allocation granularity making 252 the usable maximum. gfx11 has
no fp8 ALU and no fp8 tensor path, and the snippet's grids put only 6–32
waves on 84 CUs. The ~30× per-kernel gap to NVIDIA is therefore mostly
architectural; the fork can shave the emulation and scheduling margins but
not erase it. RDNA4 (gfx12) with its fp8 hardware is the platform where this
network stops being latency-bound.

Register-allocation A/B detail (three generations of the same kernel, from the
cache metadata):

| generation | vgpr allocated | max spill | qkv per-launch |
|---|---|---|---|
| WGP default | 128–192 | 1110 | 0.595 ms |
| CU mode | 96 | 1898 | — (frame +16 %) |
| WGP + `amdgpu-num-vgpr=256` | 190–192 | 886 | 0.636 ms |

The num-vgpr attribute alone did not raise the allocation above 192: the
GCN scheduler's register budget is derived from its occupancy *target*
(default 8 waves/SIMD → 1536/8 = 192), and the attribute only raises the
ceiling. Follow-up run emitted `amdgpu-waves-per-eu=1` (a floor constraint)
alongside `num-vgpr=252` so the budget could grow to the architectural
limit — **and the allocator still stayed at 190–192 VGPRs** (spills 0–886,
43/46 kernels spilling). Even where spills dropped across generations,
per-kernel latency did not move: qkv 0.595 (default) → 0.636 (vgpr256) →
0.600 ms (vgpr252+waves1); launch totals 80.6 → 82.5 → **78.8 ms/frame**
(~2 %, noise-adjacent).

Register axis therefore closes with two findings: LLVM's GCN scheduler will
not spend the 192–252 headroom on this PTX through the exposed attributes,
and — decisively — the spills were never the bottleneck. The ~0.5 ms per
kernel is the instruction-chain volume of emulated fp8 (28 K instructions,
~20 % perm/bfi/cvt packing tax, 13.8 % `s_delay_alu` stalls, 512 real calls)
run by a single wave. The levers that remain live are compiler-quality work
(fp8 lowering, scheduling, call inlining) in the fork, bounded by the same
~16 % scale the CU-mode A/B measured for halved registers.

## The real smoking gun: fp8 mma.sync lands on unpaired, padded WMMA (evening revision)

Counting instructions in NVIDIA's own PTX for the model across all modules (dumped by zluda_trace in `build/trace/framebench.exe/*.ptx`):

- **54,800 total `mma.sync` instructions** model-wide:
  - **35,072** `mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16` (64.0 %)
  - **19,728** `mma.sync.aligned.m16n8k32.row.col.f16.e4m3.e4m3.f16` (36.0 %)
  - Exactly **0** `.f32` accumulator variants (all model MMAs interface via `.f16` accumulators/outputs).
- The top kernel (`cc_split_swin_16h_qkv_512_chained_fp8`) alone contains **256 mma.sync** plus 196 `cvt.rn.satfinite.e4m3x2.f16x2` (f16→fp8 packing for the next layer). An earlier statement that NVIDIA "does not use tensor cores for this layer" is wrong — it is a tensor-core network with fp8 storage.

ZLUDA's lowering of these on gfx11:

- fp8 **conversions were lowered to real function calls**:
  `replace_instructions_with_functions.rs` mapped `cvt ... e4m3x2` to calls into ptx_impl helpers (`cvt_rn_satfinite_e4m3x2_f16x2` etc.) — every call was an `s_swappc_b64` with a scratch stack frame and a scheduling fence.
- fp8 **conversions inlined (done 2026-09-12)**:
  Call-based lowering was replaced with direct inline emission in `ptx/src/pass/llvm/emit.rs` for all four f16x2/f32 ↔ e4m3x2/e5m2x2 directions, with direct bit-level RNE rounding from f32. Verified: 588/588 compiler tests, numeric sweeps, and measured 83.5 → 78.4 ms/frame (~6 % gain; two separate runs — the register A/B's 78.8 ms landing is the same magnitude, the 0.4 ms spread is run-to-run noise).
- **The 0.5% WMMA metric decoded**:
  `tools/analyze_vopd.py` on the compiled cache (`zluda2.db`) reveals **24,319 instances of `v_wmma_f32_16x16x16_f16`** physically present across modules 9–15 (e.g., 3,473 in module 14, 9,921 in module 15).
  *(Note on module coverage: the measured cache holds 7 of the 15 modules —
  46.7 % by count, but only ~32.6 % of the expected 74,528 unpaired WMMAs
  (35,072 FP16 + 19,728 × 2 FP8-split). Two hypotheses, both alive: (a) MMA
  density is strongly non-uniform — module 15 alone accounts for 9,921
  (~41 % of all measured); (b) a fraction of expected WMMAs never
  materialised (scalarised or merged downstream). Route A's before/after
  count on module 14 separates the two at near-zero cost.)*
  Total Vector ALU instructions in the cache number **4,849,571** (plus 1.78M scalar ALU and 1.33M memory/LDS ops).
  Dividing total instructions by 24,319 yields **~327 total instructions per WMMA as a whole-cache macro dilution ratio** (or ~200 vector ALU/WMMA). This macro ratio spans the entire network's non-MMA computation (LayerNorm, softmax, GELU, residual additions, and layout transforms). At the kernel level (e.g. the qkv kernel with 28,220 total instructions and ~256–512 WMMAs), total instructions per WMMA is ~55–110 (including non-MMA kernel logic). Crucially: WMMA is not dropped by downstream compiler passes, but each uncombined WMMA is burdened by local scaffolding (operand widening, B-matrix zero padding, and LDS `bpermuteLane` split trees).

## Project proposal: pair the unpaired WMMAs & delete the pad/split scaffolding (2026-09-13)

### Why existing WMMA is drowned: the Pass scheduling bottleneck

In `ext/llvm-project/llvm/lib/Target/AMDGPU/AMDGPUTargetMachine.cpp:897-908`:
```cpp
PB.registerOptimizerEarlyEPCallback(
  [](ModulePassManager &PM, OptimizationLevel Level, ThinOrFullLTOPhase Phase) {
    // TODO: maybe disable combining MMAs and just lower them individually
    // if at O0
    FunctionPassManager ZludaFPM;
    ZludaFPM.addPass(CombineMMAPass());
    if (Level != OptimizationLevel::O0) {
      ZludaFPM.addPass(EarlyCSEPass());
    }
    ZludaFPM.addPass(LowerMatrixConversionsPass());
    PM.addPass(createModuleToFunctionPassAdaptor(std::move(ZludaFPM)));
  });
```

1. **`OptimizerEarlyEPCallback` runs before the Inliner**:
   When `CombineMMAPass` executes, the kernel contains only external function calls (`call @__zluda_ptx_impl_...`). The intrinsic `llvm.zluda.mma` is not yet visible in the kernel's basic blocks.
2. **Subfunctions cannot pair internally**:
   Inside `zluda_ptx_impl`, each helper encapsulates the MMA(s) of a single `mma.sync` — one for `m16n8k16`, two for `m16n8k32` ($k \in [0..15]$ and $k \in [16..31]$) — and none of them can pair internally because their $A$ operands differ (`FirstA != SecondA`). Across separate helper functions before inlining, **pairing is structurally impossible**.
3. **The author's TODO context (line 901-902)**:
   The source comments `// TODO: maybe disable combining MMAs and just lower them individually / if at O0` demonstrate the author was weighing combining vs. individual lowering, but overlooked the pipeline scheduling order.
4. **Pipeline anchors in `AMDGPUTargetMachine.cpp`**:
   - Line 954 already provides `registerOptimizerLastEPCallback` (currently only hosting `AMDGPUAttributor`) — an existing hook running *after* function inlining.
   - Line 894 has `AMDGPUAlwaysInlinePass()`, but it is enclosed inside line 849 (`registerFullLinkTimeOptimizationEarlyEPCallback`), which only executes during LTO pre-link. Since ZLUDA compiles module-by-module without LTO pre-link callbacks, inlining never ran prior to `registerOptimizerEarlyEPCallback`.
5. **Forced fallback to `lowerMMA`**:
   Every MMA falls into `lowerMMA` (unpaired 16×8 padded with zero B columns to 16×16). Immediately following, `LowerMatrixConversionsPass` emits `dMatrixSplit`—inserting heavy trees of `bpermuteLane` (LDS roundtrips), `select`, and shift operations to carve the 16×8 result back out of the 16×16 hardware fragment.
6. **Post-inlining blindness**:
   When the Inliner finally flattens the helper functions into the kernel basic blocks, `CombineMMAPass` is never registered or called again anywhere else in the pipeline. Adjacent MMAs that naturally share `A` (e.g. `module_0001_01.ptx:16870-16880`) remain permanently frozen as separate padded WMMAs wrapped in bpermute/LDS scaffolding.
7. **Direct verification flag**:
   Compiling with `-mllvm -print-after=zluda-combine-mma` (pass registered in `PassRegistry.def:574`) allows directly observing whether paired intrinsics are formed and how many `amdgcn_wmma` instructions are emitted.

### Code proof: Why cross-instruction pairing is mathematically inevitable once inlined

Inside `zluda_ptx_impl.cpp:1400-1417`, `fp8_mma_half` emits a single intrinsic:
```cpp
return __llvm_zluda_mma_m16n8k16_f32_f16_f16_f32_optnone(a, bb, acc);
```
In `zluda_ptx_impl.cpp:1426-1428`, one `m16n8k32` is lowered to two consecutive `fp8_mma_half` calls ($k \in [0..15]$ with `a_reg[0..1]`, and $k \in [16..31]$ with `a_reg[2..3]`).
Internally within a single `m16n8k32`, `FirstA != SecondA` (`a_reg[0..1]` vs `a_reg[2..3]`), so intra-instruction pairing cannot occur.

However, adjacent PTX instructions naturally share operand $A$:
In real PTX dumps (e.g. `module_0001_01.ptx:16870-16880`), two adjacent `m16n8k32` instructions share `{%r595..%r598}` (matrix $A$). When inlined into the kernel, this expands into four intrinsics:

| Intrinsic | $A$ Source | $B$ Source | Target 16×16 Tile Quadrant |
|---|---|---|---|
| `mma1_k0_15`  | `a_reg[0], a_reg[1]` | `b1[0]` | $k \in [0..15]$, Left half ($N \in [0..7]$) |
| `mma1_k16_31` | `a_reg[2], a_reg[3]` | `b1[1]` | $k \in [16..31]$, Left half ($N \in [0..7]$) |
| `mma2_k0_15`  | `a_reg[0], a_reg[1]` | `b2[0]` | $k \in [0..15]$, Right half ($N \in [8..15]$) |
| `mma2_k16_31` | `a_reg[2], a_reg[3]` | `b2[1]` | $k \in [16..31]$, Right half ($N \in [8..15]$) |

- `mma1_k0_15` and `mma2_k0_15` share the **exact same SSA values** for $A$ (originating from the same `fp8_widen_pair`). Once inlined, `EarlyCSEPass` exposes identical operands, causing `FirstA == SecondA` in `CombineMMA.cpp:179` to evaluate to `true`. They fuse into a single hardware 16×16 WMMA ($k \in [0..15]$) with combined $B = [b_1[0], b_2[0]]$.
- `mma1_k16_31` and `mma2_k16_31` similarly fuse into a single hardware 16×16 WMMA ($k \in [16..31]$) with combined $B = [b_1[1], b_2[1]]$.
- **Result: 4 intrinsics collapse into exactly 2 hardware WMMAs (50% reduction)**. The verification gate `v_wmma count halves` is not an empirical hope, but a mathematical necessity derived from the SSA graph — **conditional on Step 0 making the helpers inlineable and Route A moving the pass after the inliner** (without those, the intrinsics never share a function with each other or with the kernel at pairing time).

### Why register spills happen — correcting the accumulator math

- In `m16n8k32`, matrix $C$ has $16 \times 8 = \mathbf{128}$ total elements for the **entire 32-thread warp**.
- Each individual thread owns $\frac{128}{32} = \mathbf{4}$ output elements:
  - In FP32 accumulation: **4 VGPRs per thread**.
  - In packed FP16 accumulation: **2 VGPRs per thread**.
- For a fused $16 \times 16$ tile, matrix $C$ has $16 \times 16 = \mathbf{256}$ total elements for the warp:
  - In FP32 accumulation: $\frac{256}{32} = \mathbf{8}$ VGPRs per thread.
  - In packed FP16 accumulation: $\frac{256}{32} = \mathbf{4}$ VGPRs per thread.
- The number 4096 is warp MAC operations ($16 \times 8 \times 32$), corresponding to 128 MAC operations per thread, **not 128 accumulator slots**.
- The measured `vgpr_spill_count: 118–184` is therefore **not** caused by accumulator count. It is caused by:
  1. The scalar fallback / conversion scaffolding allocating multiple 8-element temporary row/col arrays (`upper_row[8]`, `lower_row[8]`, `left_column[8]`, `right_column[8]`).
  2. The local scaffolding instructions (DPP/widen, B-padding, and heavy `bpermuteLane` LDS split trees) keeping live values across unrolled loop iterations.

### Hardware WMMA comparison

*(corrected 2026-09-13: the left column is **not** scalar emulation — the compiled code already emits `v_wmma_f32_16x16x16_f16` (24,319 instances). The fusion win is therefore "halve the WMMA count and delete the pad/split scaffolding", **not** "replace hundreds of scalar ops with one WMMA".)*

| Metric | Unpaired 16×8 WMMA (Current) | Fused 16×16 WMMA (Target) |
|---|---|---|
| WMMA per 2× `m16n8k32` | 4 (half of each 16×16 wasted) | **2** (both halves useful) |
| Useful MACs per WMMA | 2048 of 4096 (50 %) | **4096 of 4096 (100 %)** |
| B-matrix handling | Zero-padded to 16 columns | Natural `[B₀, B₁]` concat |
| Result extraction | `dMatrixSplit` + `bpermuteLane` LDS round-trips | Direct register fragment |
| Local scaffolding per tile | ~40–80 ops | **~12–16 ops** |
| Accumulator VGPRs per thread | 4 (F32) / 2 (packed F16) | 8 (F32) / 4 (packed F16) |

  Per element the cost is identical (4/128 = 8/256 = 1 VGPR per 32 outputs).
  The fused tile holds more accumulators because it covers twice the output,
  not because fusion is more register-hungry; the register win comes from
  deleting the operand-staging arrays.

**Sizing the win honestly.** Using cache-consistent figures (24,319 WMMAs against ~7.96 M total instructions, both from the same 7 cached modules): removing ~30–65 scaffolding ops per tile removes **0.73–1.58 M instructions, i.e. 9–20 % of the total** — not the ~2–3× that comparing against scalar emulation would imply. These kernels are latency-bound, however, and serialized `bpermuteLane` LDS round-trips cost far more in cycle latency than in issue slots, so measured latency reduction may significantly outperform this static estimate. **That gap is precisely why Route A is a measurement, not an argument.**

### Numerics: equivalent or better

- NVIDIA PTX in DLSS-NR uses `.f16` accumulators (`mma.sync...f16.e4m3.e4m3.f16` and `mma.sync...f16.f16.f16.f16`).
- In ZLUDA's lowering (`zluda_ptx_impl.cpp:1424-1438`), the `.f16` input is widened to `float4`, accumulated in F32 inside `v_wmma_f32_16x16x16_f16`, and converted back via `fptrunc` on return.
- This preserves the exact mixed-precision behavior of NVIDIA Ada Lovelace Tensor Cores (F32 accumulation internally, rounded to F16 output). No precision is traded away.

### Implementation levers: Two viable architectural paths

0. **Step 0 (precondition for Route A — strip `noinline` from the ptx_impl
   bitcode)**: disassembling the shipped `zluda_ptx_impl.bc` shows the mma
   helpers carry `noinline` (attribute group #16:
   `{ convergent mustprogress noinline nounwind ... }`) — a leftover of the
   O0-compile `optnone` pairing that the bitcode build pipeline's sed strips
   only half of (`optnone` is stripped, `noinline` survives; and LLVM's
   inliner refuses noinline callees, `InlineCost.cpp:3246`). Evidence: the
   compiled kernels contain live `s_swappc_b64` calls to the `_ZL49`
   helpers, and the `_ZL49` call sites in the bitcode carry no
   `alwaysinline` (attribute #23 = `{ convergent nounwind }`) — the
   `[[clang::always_inline]]` hints in the C++ source did not survive the
   O0 + sed rebuild. Consequence: **the intrinsics never reach the kernel's
   basic blocks at any pipeline stage, so pairing is structurally
   impossible until this is fixed.** Fix: add `sed 's/noinline//g'` to both
   bitcode build pipelines (the two intentional `noinline` sites —
   `__assert_fail`, `vprintf` — become inlineable, a harmless size change
   worth watching in the vgpr/spill notes). `ZLUDA_PTX_IMPL_DIGEST` bumps,
   invalidating the cache once by design.
   - **Expected outcome of Step 0 alone**: the **mma-helper** `s_swappc`
     calls disappear (the O3 inliner flattens the now-inlineable helpers)
     but the WMMA count does **not** halve — `CombineMMAPass` still runs at
     the early extension point, before the inliner, so it still sees calls
     instead of intrinsics. Other `s_swappc` calls (the chained layer
     calls, `__assert_fail`, `vprintf`) are unaffected — possibly they even
     become inlined now that their `noinline` is gone, so the verification
     gate is **"the mma-helper call count drops to zero"**, not "the
     `s_swappc` total drops to zero". That asymmetry is what cleanly
     separates the two variables (inlining failure vs pass timing).
   - Empirical per-module state of the current cache (s_swappc calls vs
     `v_wmma` instructions, static disassembly):

     | module | KB | s_swappc | v_wmma |
     |---|---|---|---|
     | 15 | 12,950 | 9,776 | 9,921 |
     | 13 | 9,618 | 7,216 | 3,001 |
     | 14 | 9,434 | 6,960 | 3,473 |
     | 12 | 8,302 | 6,608 | 2,705 |
     | 11 | 7,509 | 4,288 | 3,713 |
     | 10 | 4,739 | 4,352 | 1,441 |
     | 9 | 396 | 256 | 65 |

     Calls and WMMAs coexist in every module, but the ratio varies widely
     (0.99:1 in module 15, up to 3.94:1 in module 9; 1.62:1 overall). That
     ratio does **not** measure inlining: `s_swappc_b64` counts *all* calls
     (mma helpers, the chained layer calls, `__assert_fail`, `vprintf`), so
     >1 is expected regardless. The inlining state is settled by the
     attribute evidence instead — the `_ZL49` call sites carry no
     `alwaysinline` and the callee carries `noinline`, so **no** mma helper
     call is inlined on either the f16 or the fp8 path.
1. **Route A (LLVM Pass timing — the definitive experiment)**:
   After Step 0, in `ext/llvm-project/llvm/lib/Target/AMDGPU/AMDGPUTargetMachine.cpp`, move `CombineMMAPass` and `LowerMatrixConversionsPass` from `registerOptimizerEarlyEPCallback` (line 897) to `registerOptimizerLastEPCallback` (line 954), after the Inliner has flattened subfunctions into kernel basic blocks.
   - **Verification test**: Recompile a single module (e.g. module 14) with
     `-mllvm -print-changed=diff -mllvm -filter-print-funcs=cc_split_swin_16h_qkv_512_chained_fp8`,
     run `llvm-objdump -d`, and verify that `v_wmma` count halves, `bpermuteLane` / LDS overhead drops, and `ZLUDA_LAUNCH_TIMING=1` shows immediate per-launch latency reduction.
2. **Route B (PTX IR AST Peephole Fusion in Rust)**:
   In ZLUDA's `ptx/src/pass/`, implement a peephole fusion pass on the PTX AST prior to LLVM emission:
   - *Why AST pass is required*: Line-by-line translation in `emit.rs` cannot observe adjacent instruction context or detect operand sharing across multiple `mma.sync` statements.
   - In the PTX AST pass, inspect basic blocks to identify adjacent `mma.sync` pairs sharing identical matrix $A$ operands (e.g. `module_0001_01.ptx:16870-16880`: `mma.sync {%r623, %r635}, {%r595..598}, {%r593..594}` and `mma.sync {%r642, %r649}, {%r595..598}, {%r599..600}`).
   - Fuse the two 16×8 operations into a single 16×16 node with combined $B=[B_0, B_1]$.
   - Directly emit hardware WMMA intrinsics without relying on LLVM C++ pass pattern matching.
3. **Unified target (FP16 + FP8)**:
   Both the 35,072 FP16 MMAs and the 19,728 FP8 MMAs lower into `v_wmma_f32_16x16x16_f16`. Unblocking fusion at either layer optimizes both precisions simultaneously.

### Projection (with uncertainties)

*(Two figures, two premises: **232–264 ms** is derived from cache-consistent static instruction scaling — the 9–20 % removal, measured on the 640×360 cache, applied to the **290 ms measured GPU average** from the solid-color runs; the cross-resolution extrapolation is justified by the per-kernel latency being resolution-independent, see above — and is the defensible baseline; **80–120 ms** is the legacy figure from the earlier scalar-emulation premise and requires LDS-latency removal to compound. Route A replaces both with a measurement).*

| Platform | Path | Projected 1080p frame |
|---|---|---|
| RX 7900 XT (gfx1100) | m16n8k32 e4m3 → DPP widen → fused f16 WMMA | **~232–264 ms** (static instruction scaling, cache-consistent); **80–120 ms** target only if LDS-latency removal compounds; the 40 ms lower end of some bands is a theoretical floor, not an expectation |
| RX 9060 XT (gfx1200) / RX 9070, 9070 XT (gfx1201) | native fp8 WMMA (`v_wmma_f32_16x16x16_fp8_fp8`) | **30–50 ms** — confirmed: vendored LLVM carries `Intrinsic::amdgcn_wmma_f32_16x16x16_fp8_fp8` and gfx12 builtins |
| RX 6000 (gfx10) | no WMMA hardware | excluded — keeps scalar path; fp8-inline still applies |

640×360 case: pending empirical measurement with Route A (static instruction scaling suggests ~63–71 ms, down from 78.4 ms; lower latencies depend on whether single-wave wait latency collapses with the removal of LDS bpermute).

### RDNA4 (gfx12) field status — 2026-09-13: no successful run recorded

All numbers above come from the RX 7900 XT (gfx1100). **No RDNA4 measurement
exists**; the 30–50 ms figure is extrapolated from the intrinsic being present,
not from a run.

A field report on an RX 9070 XT (gfx1201) fails before any frame is produced:

- `1 of 15 modules could not be translated` during precompile, and at runtime
  `[CCNRDGpuInfo::CreateFeature:363] error: cuModuleLoadFile Function failed`
  → `Init_Kernels failed: get kernel
  "cc_tinlayout_fused_pre_block_swin_3h_32_3_ds_fp8" failed`.
- The failing module is the one carrying the **fp8** Swin kernel.
- Confounding factor on that machine: `HSA_OVERRIDE_GFX_VERSION` was set to
  **11.0.0**, which suppresses the RDNA4 auto-config (`gpu_detection.h:109-119`
  only injects 12.0.1 when the variable is unset) and makes ZLUDA compile gfx11
  ELF for a gfx1201 device. **Not yet confirmed whether removing it fixes the
  run** — that is the open question.

Consequences:

1. **Phase 4 may be a correctness prerequisite, not a performance nicety.**
   Today gfx12 falls into the `>= 11000 && < 13000` f16-widen branch
   (`zluda_ptx_impl.cpp:1424`), i.e. RDNA4 is handed the RDNA3 WMMA form.
   Whether that is merely slow or genuinely uncompilable on gfx12 is exactly
   what this report cannot yet distinguish — and resolving it should outrank the
   gfx11 frame-rate work, because gfx11 users are "slow" while RDNA4 users may
   be "broken".
2. ~~**`gpu_detection.h` must not defer to an obviously mismatched override.**~~ —
   **implemented (2026-09-13)**: the RDNA4 branch now corrects a non-gfx12
   override to 12.0.1 with a loud auto-config message, and the batch GUI
   surfaces the same warning in its log window at startup
   (`BatchWindow::hint_override_mismatch`). Note the original design intent
   visible at `zluda_ptx_impl.cpp:1201` — the optnone wrapper exists to stop
   ZLUDA-specific passes from optimizing away the intrinsic — which is why
   the fix strips `noinline`/`optnone` in the bitcode build rather than
   removing the wrapper blindly.

### External corroboration: a native HIP reimplementation hits the projection

`danielblnc/DLSS-NR-on-AMD` (the execution backend of
`eikkapine/DLSS5-AMD-Video`) is a **ground-up HIP/ROCm reimplementation** of
the DLSS-NR runtime — no CUDA translation, no ZLUDA; weights are extracted
from the user's `nvngx_dlssnr.dll` into a custom `.bin`, inference is
hand-written HIP. Its measured **~33 FPS at 1080p on an RX 9070 XT
(~30 ms/frame)** independently corroborates this document's 30–50 ms RDNA4
projection: the number is achievable on gfx12 when the fp8 math runs natively
instead of through any form of translation.

Two caveats keep it honest: (a) their figure is a game-integration claim at
unknown internal settings, not a like-for-like video benchmark; (b) the
eikkapine video pipeline built on top of it measured 3.41 s/frame at 1080p —
10× slower than this project's ZLUDA pipeline (~0.3 s/frame at 1080p) — so
the orchestration overhead around the runtime, not the GPU, dominates that
particular pipeline. The strategic reading: the reimplementation route has
the higher ceiling on RDNA4, while this project's ZLUDA route currently wins
on video throughput and keeps NVIDIA's actual code paths (fidelity).
3. **Detection coverage note (verified)**: the name match covers RX 9060/9070
   and the generic "rx 9"/"radeon 9" forms (`gpu_detection.h:141-147`), so the
   gfx1200 cards ARE auto-configured. One stale piece: the device-id fallback
   range (0x7480–0x74DF) predates RDNA4 — adapters reporting without a
   standard 9000-series name fall through it. Worth widening or dropping when
   Phase 4 ships.

### Phased plan

0. **Strip `noinline` from the ptx_impl bitcode** (both sed pipelines), rebuild
   `zluda_ptx_impl.bc`, and measure alone: the **mma-helper** `s_swappc` calls
   should vanish (gate: mma-helper call count → 0, *not* total `s_swappc` → 0
   — the chained layer calls, `__assert_fail` and `vprintf` stay) while the
   WMMA count stays flat. This isolates the inlining variable from
   the pass-timing variable and is an independent win on its own (helpers
   become inlineable everywhere, not just for the mma pairing).
1. **Pass timing experiment (Route A)**: Move `CombineMMAPass` to `registerOptimizerLastEPCallback` in `AMDGPUTargetMachine.cpp`; test module 14 with `ZLUDA_LAUNCH_TIMING=1` and
   `-mllvm -print-changed=diff -mllvm -filter-print-funcs=cc_split_swin_16h_qkv_512_chained_fp8`.
2. **Peephole fusion / direct lowering (Route B)**: If LLVM pass migration has unintended phase interactions, implement PTX AST peephole pairing in Rust (`ptx/src/pass`).
3. **Numeric verification**: run the full suite with `cargo test -p ptx`
   (debug build — in release mode the IR fixture comparison hits a
   pre-existing value-naming difference unrelated to the change), verifying
   that F32 accumulation produces identical output to NVIDIA references
   across subnormals, NaNs (`0x7F`), and saturated values (`0x7E`).
4. **RDNA4 branch**: Add a `>= 12000` branch ahead of the existing `11000..13000` branch in `zluda_ptx_impl.cpp:1424`, so RDNA4 selects native `amdgcn_wmma_f32_16x16x16_fp8_fp8` instead of falling into the f16-widen path. Note the shape difference: the gfx12 fp8 WMMA is a single k32 instruction (no k-split pairing needed), but its N dimension still needs the 8→16 handling — the RDNA3 pairing logic does not transfer one-to-one.

### Artifacts

- `tools/framebench.cpp` — multi-frame per-frame timing harness (start once,
  N frames, per-frame and steady-state summary). Same link line as
  `build_smoke.bat`: `cl /std:c++17 /EHsc /O2 /utf-8 /I core /I dlss_layer
  tools\framebench.cpp core\image_processor.cpp core\precompile.cpp
  dlss_layer\dlss_cuda.cpp dlss_layer\frame_blit.cpp wintrust.lib d3d12.lib
  dxgi.lib d3dcompiler.lib windowscodecs.lib ole32.lib shell32.lib user32.lib`.
- `tools/launchbench.cpp` — ZLUDA launch/sync microbenchmark:
  `cl /std:c++17 /EHsc /O2 /utf-8 tools\launchbench.cpp`, run as
  `launchbench <nvcuda.dll> <module.ptx>`.
- `tools/analyze_vopd.py` — disassembly profiler for VOPD and WMMA counts in `zluda2.db`.
- `DLSSNR_PHASE_TIMING=1` in `core/image_processor.cpp` — permanent per-phase stderr timing, default off.
- `zluda_trace` operational invocation:
  `ZLUDA_LOG_DIR=<dir> zluda.exe --zluda-trace -- framebench.exe ...`
  *(Note: Driving `zluda_trace.dll` directly as `nvcuda.dll` does **not** work because the NGX snippet's export resolution fails; execution must proceed through the inject host `zluda.exe`. The trace dump files referenced by line numbers above were one-off local artifacts and have been cleaned; the invocation line regenerates them.)*
