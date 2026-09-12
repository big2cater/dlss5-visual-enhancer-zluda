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
- fp8 is emulated: no fp8 ALU, no WMMA at all — every fp8 op goes through
  `v_perm_b32`/`v_bfi_b32` unpack + `v_pk_mul_f16`/`v_pk_add_f16` +
  `v_cvt_f16_f32` repack (~20 % of the code).
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

Counting instructions in NVIDIA's own PTX for the module carrying
`cc_split_swin_16h_qkv_512_chained_fp8` (dumped by zluda_trace):

- **5056 `mma.sync`** module-wide — shapes `m16n8k16` (f16, 2880) and
  `m16n8k32` (**e4m3 fp8 tensor core**, 2176). The qkv kernel alone contains
  **256 mma.sync** plus 196 `cvt.rn.satfinite.e4m3x2.f16x2` (f16→fp8 packing
  for the next layer). An earlier statement that NVIDIA "does not use tensor
  cores for this layer" is wrong — it is a tensor-core network with fp8
  storage.

ZLUDA's lowering of these on gfx11:

- fp8 **conversions are lowered to real function calls**:
  `replace_instructions_with_functions.rs` maps `cvt ... e4m3x2` to calls
  into ptx_impl helpers (`cvt_rn_satfinite_e4m3x2_f16x2` etc.) — every call
  is an `s_swappc_b64` with a scratch stack frame and a scheduling fence.
  This is where the 512 calls in the compiled kernel come from: the calls
  and the fp8 emulation tax are the **same root cause**.
- fp8 **mma.sync is emulated scalar-wise**: the compiled kernel has no WMMA
  and no fp8 ops — each m16n8k32 (4096 MACs) becomes hundreds of
  perm/bfi/pk_f16 instructions with dependent chains (measured: ~182 cycles
  per MAC on the qkv kernel). e4m3 values are exactly representable in f16,
  so the mma could instead lower to unpack-e4m3→f16 + `v_wmma_f32_16x16x16_f16`
  (which the fork's CombineMMA already supports for f16) — one WMMA per 4096
  MACs instead of hundreds of scalar ops.

This makes the two concrete levers, both in `ptx/src/pass`:

1. ~~Inline the fp8 conversion helpers~~ — **done the same day**: the
   call-based lowering was replaced with inline emission for all four
   f16x2/f32 ↔ e4m3x2/e5m2x2 directions (IEEE RNE, denormals, satfinite,
   .relu), and the f32 sources round **directly from the f32 bits** — the
   f32→f16→e4m3 two-step double-rounded (1.1875 − 2^-16 lands on 1.25
   through f16, 1.125 directly). Verified: 588/588 compiler tests, the
   real-GPU `_amdgpu` numeric tests for both f32 cvt forms, and a
   315 762-value exhaustive sweep of the direct-RNE algorithm against a
   spec reference. Measured: 83.5 → 78.4 ms/frame (~6 %); the remaining
   `s_swappc_b64` calls in the hot kernels are the m16n8k16/m16n8k32
   mma.sync software emulation.
2. **Lower fp8 m16n8k32 mma.sync via f16 WMMA after unpacking** — the next
   lever, unchanged.

Grid sizing question closed: the trace shows the snippet queried only four
device attributes (compute capability major/minor) at init — the (6,2)-style
grids are the snippet's own Swin-window decomposition, not something ZLUDA
under-answered.

## Artifacts

- `build/framebench.cpp` (+ `build/framebench_build.bat`) — multi-frame
  per-frame timing harness (processor_smoke with N frames and a summary).
- `build/launchbench.cpp` (+ bat) — ZLUDA launch/sync microbenchmark.
- `DLSSNR_PHASE_TIMING=1` in `core/image_processor.cpp` — permanent per-phase
  stderr timing, default off.
- zluda_trace usage that worked here:
  `ZLUDA_LOG_DIR=<dir> zluda.exe --zluda-trace -- framebench.exe ...`
  (driving `zluda_trace.dll` directly as `nvcuda.dll` does **not** work — the
  NGX snippet's export resolution fails; go through the inject host).
