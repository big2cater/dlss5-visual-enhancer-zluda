# Performance bottleneck analysis — 2026-09-12

Machine: RX 7900 XT (gfx1100), Windows, big2cater/ZLUDA @ 1d47bf4.
Workload: DLSS-NR (nvngx_dlssnr 310.8.0, CG2R backbone `crazy-cuckoo`), 640×360,
temporal path, WGP mode (fork default). All numbers from `build/framebench`
(start + N frames, per-frame `last_ms`) and `DLSSNR_PHASE_TIMING=1`.

## Where a frame's milliseconds go

| measurement | result |
|---|---|
| steady-state frame (640×360) | 83.5 ms |
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
  of vector ops. Micro-architectural, but it cannot explain the numbers below.

## What it IS bound by

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

1. **Per-kernel GPU timing from inside the fork.** An env-gated event pair
   around each `hipModuleLaunchKernel` (or a rocprofv2 trace) would rank the
   156 kernels by GPU duration and confirm the 0.5 ms uniformity. This is the
   single most valuable measurement; RGP would show the same directly.
2. **Chase the single-wave latency** for the top kernels: check whether the
   compiled ISA does scalar (s_load) weight fetches with long `s_waitcnt`
   chains, whether `glc`/coherent bits are forced on image accesses, and
   whether LDS staging of weights could turn per-kernel latency into
   throughput. `tools/analyze_vopd.py` is the starting toolkit.
3. **More waves per kernel** — if grids are chosen by the snippet's host code
   from device attributes, verify every attribute ZLUDA answers (SM count,
   occupancy) matches what NVIDIA would return; an under-answer shrinks grids
   and turns every kernel latency-bound.

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
  `v_cvt_f16_f32` repack (~20 % of the code). *(Note: while initially suspected that WMMA was absent, later detailed cache disassembly confirmed 24,319 WMMAs are present, but severely diluted by ~327 auxiliary ops per WMMA due to pass scheduling failure; see evening revision below)*.
- 512 `s_swappc_b64` call sites — the "chained" network layers are real
  subroutine calls, not inlined.
- The PTX carries **no tuning directives at all** (no `.maxnreg`,
  `.minnctapersm`, `.reqntid`), so neither the fork's `amdgpu-num-vgpr`
  mapping nor occupancy directives cause the cap; it is LLVM's own occupancy
  heuristic (`MaxNumVGPRs = min(TotalVGPRs / WavesPerEU, 256)`, gfx11
  wave32 addressable max = 256).

Levers, in order of expected value:

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

## The real smoking gun: fp8 mma.sync is fully emulated (evening revision)

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
  Call-based lowering was replaced with direct inline emission in `ptx/src/pass/llvm/emit.rs` for all four f16x2/f32 ↔ e4m3x2/e5m2x2 directions, with direct bit-level RNE rounding from f32. Verified: 588/588 compiler tests, numeric sweeps, and measured 83.5 → 78.4 ms/frame (~6 % gain).
- **The 0.5% WMMA metric decoded**:
  `tools/analyze_vopd.py` on the compiled cache (`zluda2.db`) reveals **24,319 instances of `v_wmma_f32_16x16x16_f16`** physically present across modules 9–15 (e.g., 3,473 in module 14, 9,921 in module 15).
  However, total Vector ALU instructions number **4,849,571** (plus 1.78M scalar ALU and 1.33M memory/LDS ops).
  WMMA is not missing or dropped; rather, **each single WMMA instruction is diluted by ~327 auxiliary instructions** (DPP shuffles, `fp8_widen_pair` expansion, stack/alloca frames, and uncombined `dMatrixSplit` LDS bpermute trees).

## Project proposal: lower fp8 mma.sync to WMMA & unblock fusion (2026-09-13)

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
2. **Subfunctions contain only 1 MMA each**:
   Inside `zluda_ptx_impl`, each helper function encapsulates a single MMA instance. `CombineMMA.cpp:179` requires `FirstA == SecondA` within the same basic block to fuse two 16×8 MMAs into one 16×16 WMMA. Across separate helper functions before inlining, **pairing is structurally impossible**.
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
  2. The ~327 auxiliary DPP, unpack, and `bpermuteLane` instructions per MMA keeping live values across unrolled loop iterations.

### Hardware WMMA comparison

| Metric | Unpaired 16×8 (Current) | Fused 2× `v_wmma_f32_16x16x16_f16` (16×16 tile) |
|---|---|---|
| Math throughput per wave | ~4–8 MACs/cycle (dependent chains + unpack) | 8192 MACs in ~32–64 cycles ≈ **128–256 MACs/cycle** |
| Accumulator VGPRs per thread | **4** (F32) or **2** (packed F16) | **8** (F32) or **4** (packed F16) |
| Padding / Truncation overhead | High (padded B + `dMatrixSplit` LDS bpermute tree) | **Zero** (natural 16×16 coverage, direct writeback) |
| Auxiliary instructions per MMA | ~327 (LDS, DPP, pack/unpack, split) | **~12–16** (DPP quad-gather + bitwise widen) |

### Numerics: equivalent or better

- NVIDIA PTX in DLSS-NR uses `.f16` accumulators (`mma.sync...f16.e4m3.e4m3.f16` and `mma.sync...f16.f16.f16.f16`).
- In ZLUDA's lowering (`zluda_ptx_impl.cpp:1424-1438`), the `.f16` input is widened to `float4`, accumulated in F32 inside `v_wmma_f32_16x16x16_f16`, and converted back via `fptrunc` on return.
- This preserves the exact mixed-precision behavior of NVIDIA Ada Lovelace Tensor Cores (F32 accumulation internally, rounded to F16 output). No precision is traded away.

### Implementation levers: Two viable architectural paths

1. **Route A (LLVM Pass timing — the definitive 2-line experiment)**:
   In `ext/llvm-project/llvm/lib/Target/AMDGPU/AMDGPUTargetMachine.cpp`, move `CombineMMAPass` and `LowerMatrixConversionsPass` from `registerOptimizerEarlyEPCallback` (line 897) to `registerOptimizerLastEPCallback` (line 954), after the Inliner has flattened subfunctions into kernel basic blocks.
   - **Verification test**: Recompile a single module (e.g. module 14) with `-mllvm -print-after=zluda-combine-mma`, run `llvm-objdump -d`, and verify that `v_wmma` count halves, `bpermuteLane` / LDS overhead drops, and `ZLUDA_LAUNCH_TIMING=1` shows immediate per-launch latency reduction.
2. **Route B (PTX IR AST Peephole Fusion in Rust)**:
   In ZLUDA's `ptx/src/pass/`, implement a peephole fusion pass on the PTX AST prior to LLVM emission:
   - *Why AST pass is required*: Line-by-line translation in `emit.rs` cannot observe adjacent instruction context or detect operand sharing across multiple `mma.sync` statements.
   - In the PTX AST pass, inspect basic blocks to identify adjacent `mma.sync` pairs sharing identical matrix $A$ operands (e.g. `module_0001_01.ptx:16870-16880`: `mma.sync {%r623, %r635}, {%r595..598}, {%r593..594}` and `mma.sync {%r642, %r649}, {%r595..598}, {%r599..600}`).
   - Fuse the two 16×8 operations into a single 16×16 node with combined $B=[B_0, B_1]$.
   - Directly emit hardware WMMA intrinsics without relying on LLVM C++ pass pattern matching.
3. **Unified target (FP16 + FP8)**:
   Both the 35,072 FP16 MMAs and the 19,728 FP8 MMAs lower into `v_wmma_f32_16x16x16_f16`. Unblocking fusion at either layer optimizes both precisions simultaneously.

### Projection (with uncertainties)

| Platform | Path | Projected 1080p frame |
|---|---|---|
| RX 7900 XT (gfx1100) | m16n8k32 e4m3 → DPP widen → fused f16 WMMA | **80–120 ms** (band: 40–150 ms depending on memory bandwidth ceiling) |
| RX 9070/9080 (gfx1200) | native fp8 WMMA (`v_wmma_f32_16x16x16_fp8_fp8`) | **30–50 ms** — confirmed: vendored LLVM carries `Intrinsic::amdgcn_wmma_f32_16x16x16_fp8_fp8` and gfx12 builtins |
| RX 6000 (gfx10) | no WMMA hardware | excluded — keeps scalar path; fp8-inline still applies |

640×360 case improves proportionally from 78.4 ms down toward ~20–30 ms.

### Phased plan

1. **Pass timing experiment (Route A)**: Move `CombineMMAPass` to `registerOptimizerLastEPCallback` in `AMDGPUTargetMachine.cpp`; test module 14 with `ZLUDA_LAUNCH_TIMING=1` and `-mllvm -print-after=zluda-combine-mma`.
2. **Peephole fusion / direct lowering (Route B)**: If LLVM pass migration has unintended phase interactions, implement PTX AST peephole pairing in Rust (`ptx/src/pass`).
3. **Numeric verification**: Run full test suite (`cargo test --test spirv_run`), verifying that F32 accumulation produces identical output to NVIDIA references across subnormals, NaNs (`0x7F`), and saturated values (`0x7E`).
4. **RDNA4 branch**: Hook `__oclc_ISA_version >= 12000` directly to `amdgcn_wmma_f32_16x16x16_fp8_fp8`.

### Artifacts

- `build/framebench.cpp` (+ `build/framebench_build.bat`) — multi-frame per-frame timing harness (processor_smoke with N frames and summary).
- `build/launchbench.cpp` (+ bat) — ZLUDA launch/sync microbenchmark.
- `tools/analyze_vopd.py` — disassembly profiler for VOPD and WMMA counts in `zluda2.db`.
- `DLSSNR_PHASE_TIMING=1` in `core/image_processor.cpp` — permanent per-phase stderr timing, default off.
