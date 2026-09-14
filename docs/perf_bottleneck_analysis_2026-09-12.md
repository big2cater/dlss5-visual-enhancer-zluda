# Performance bottleneck analysis — 2026-09-12

Machine: RX 7900 XT (gfx1100), Windows, big2cater/ZLUDA @ 1d47bf4 (initial pass) through 5ac9102+ (fp8-inline build; later sections).
Workload: DLSS-NR (nvngx_dlssnr 310.8.0, CG2R backbone `crazy-cuckoo`), 640×360,
temporal path, WGP mode (fork default). All numbers from `build/framebench`
(start + N frames, per-frame `last_ms`) and `DLSSNR_PHASE_TIMING=1`.

## Current conclusions (authoritative — read this and nothing else)

Review pass 2026-09-13. Everything below this section is the measurement record,
kept append-only, including the hypotheses it retracts. Where the two disagree,
this section wins.

**The one live lever.** The network's `mma.sync` lands on *unpaired, zero-padded*
`v_wmma` because `CombineMMAPass` is registered at
`registerOptimizerEarlyEPCallback` (`AMDGPUTargetMachine.cpp:897`) — before the
inliner — while the mma helpers carry `noinline` and are therefore still calls at
that point. Pairing is structurally impossible there. Once the helpers are
inlineable **and** the pass runs after inlining, adjacent MMAs that share operand
$A$ fuse: **4 intrinsics → 2 WMMAs, MAC utilisation 50 % → 100 %**. That
derivation is SSA-exact. The *size* of the win is not, and Route A is the
measurement that decides it — 9–20 % of instructions by whole-cache static
scaling is the defensible floor, but the scaffolding is concentrated in the hot
chained kernels, which sit well above that ratio (see the sizing note below).
**Superseded 2026-09-14: the pairing is reachable without paying that 81 %. 87 →
67 ms, output frame-for-frame identical. Read the paragraph below; the ⚠️ note on
Step 0 / Route A further down is the earlier, superseded measurement.**

**2026-09-14 — reaching the pairing and paying only for it.** Same machine, same
clip, same command line, same snippet, only the driver DLL swapped:

| driver | 640×360 frame | avg GPU | throughput |
|---|---|---|---|
| plain `5ac9102` | **87 ms** | 126.6 ms | 7.37 fps |
| `5ac9102` + this change | **67–68 ms** | 105.2–105.9 ms | 8.52–8.61 fps |

All 30 output frames came out **byte-identical** (`--dump-frames`, PNG SHA-256).
That is what pairing two MMAs over one shared A should give: the products and
their accumulation order are unchanged, only the padding goes away.

What produced it is *not* the helper that was built to produce it (that one never
fires — see below). It is the pair of attribute decisions around the inliner:

- The mma wrappers no longer carry `[[clang::optnone]]`, so the
  `[[clang::always_inline]]` on their call sites is honoured and the intrinsics
  land in the `FUNC(...)` helper bodies and then in the kernel, where
  `CombineMMAPass` can see and pair them. Its own report, with
  `ZLUDA_MMA_STATS=1`, on one module: `saw 512 intrinsic(s), combined 256
  pair(s), lowered alone 0, refused 0` for each `swin_2h_64_2` kernel. In the
  baseline those intrinsics sit behind a `noinline` wrapper call, so the pass
  sees nothing to pair.
- The **fp8** mma helper stays a call — it is marked `noinline` in the source.
  The earlier attempt stripped `noinline` from the whole bitcode instead, which
  inlines the fp8 pad/split scaffolding into every call site, and that is the
  81 %. Holding that one helper out of line is what keeps the win.

Not every kernel pairs: on another module the same pass refuses all of them
(`saw 528, combined 0, refused 444 (an operand of the second MMA touches
memory)`) and pays the inlining for nothing. Across the clip the net is still
−23 %.

**The AST-level pair helper is in the tree and does not fire.** It was built to
buy pairing with no inlining at all:
`replace_instructions_with_functions.rs` emits one call to
`mma_sync_aligned_m16n8k32_row_col_f16_e4m3_e4m3_f16_pair` for two adjacent
`mma.sync` sharing an A operand, and that helper's body holds the four intrinsics
as two same-A pairs inside one basic block — verified in the shipped `.bc`. On
the real network it never triggers: one module lowers **3 608** `mma.sync` to the
single e4m3 helper and **0** to the pair helper. The gate is narrow on purpose
and for a correctness reason, not a missed match: fusing two MMAs into one call
**moves the second one's computation earlier**, so it is legal only if the second
instruction's operands are already live and nothing between them has memory
effects. In these kernels the second MMA's `B` is loaded in between — which is
exactly what the LLVM pass reports (`an operand of the second MMA touches
memory`) when it refuses the same pair. Widening the gate would change what the
kernels compute.

So the fusion is reachable only through the inliner: the AMDGPU pass is the only
place with enough information to reorder operands legally. The lever that makes it
pay is *which* helpers are allowed to inline, not avoiding inlining altogether.

**Ordered plan (supersedes the "Phased plan" ordering further down).**

Everything this project can measure is gfx11; everything gfx12 needs hardware it
does not have. The order follows that line rather than the severity of the
symptoms.

**On the machine that exists (RX 7900 XT, gfx1100):**

- **Step 0 — strip `noinline` from the ptx_impl bitcode.** No longer inferred:
  `llvm-dis` of the committed `.bc` puts noinline on the wrapper definition
  (`#16`), on the outer helpers (`#14`) and on the call sites (`#24`), with
  `alwaysinline` nowhere in the module. Restoring the inline hint matters too —
  removing noinline alone can leave the gate inconclusive.
- **Route A — pass timing** (move `CombineMMAPass` after the inliner), with
  CSE/GVN ahead of the relocated pass.
- **Route B — PTX AST peephole fusion**, if Route A's phase interactions bite.

  ⚠️ **Measured 2026-09-13/14: Step 0 + Route A are a 1.6× regression *as a
  combination*, and not for the reason this section first gave. Do not ship them
  together — but the fusion inside them is worth keeping.** Three builds separate
  the two variables, all on the same bitcode and pass placement, measured on the
  same clip with the order alternated so drift cannot line up with one binary:

  | build | configuration | 640×360 frame | vs baseline |
  |---|---|---|---|
  | A | `5ac9102`, helpers out of line (fusion not reachable) | 86–87 ms | — |
  | C | helpers inlined, fusion **disabled** (384 WMMA) | 156 ms | **+81 %** |
  | B | helpers inlined, fusion **enabled** (192 WMMA) | 137–138 ms | +59 % |

  Inlining: 86 → 156 ms. Fusion: 156 → 138 ms. **The whole regression is the
  inlining**; the fusion is a genuine **−12 %**, which is exactly the pairing this
  section argued for, working as designed.

  **What this section got wrong, and why it is written down rather than deleted.**
  An earlier revision read the static `v_wmma` counts — 65 → 192 on one module —
  as "more matrix work", and concluded that pairing was not happening. That is a
  static/runtime confusion: a static count counts *code instances*, not
  executions. In build A those 65 WMMA instructions sit inside shared helper
  bodies that the kernel calls 256 times; in build B the 192 are the fused
  results, inlined and therefore executed once each. The pass' own report settles
  it: **384 intrinsics seen, 192 pairs combined, 0 refused, 0 lowered alone** —
  everything that could pair did pair, and what had been 384 unfused executions
  became 192 fused ones. The `v_wmma` column once called "the tell" was in fact
  the fusion's own output.

  Withdrawn with it: the claim that `combineMMA`'s `FirstA == SecondA` gate
  (`CombineMMA.cpp:179`) or `tryToReorderOperands`' memory-visibility bail-out
  (`:43-47`) stops the inlined fp8 pairs. Both are real gates, but nothing was
  ever measured about which pairs they reject here, and the fusion rate says they
  did not. What the numbers do say is that inlining the ptx_impl helpers into the
  kernels is what costs — static `scratch_load/store` up 20–87 %, `ds_bpermute`
  66–109 %, module size 34–61 % — and that on this hardware that outweighs the
  WMMAs the fusion removes.

  **The corrected target is therefore not "make pairing work" (it works) but "get
  the fusion without the inlining."** The fusion alone is worth 12 %, so emitting
  a *pair helper* at translation time — one call that does both MMAs with a shared
  A and keeps the widening out of line — has a measured ceiling of about
  **76 ms**, below the 78.4 ms this section started from. Route B is its natural
  home.

  Instrumentation this measurement needed, worth keeping: `CombineMMAPass`
  reports how many MMAs it combined and why the rest were not, when
  `ZLUDA_MMA_STATS` is set in the environment; off otherwise, and free. Reaching
  it took a detour worth recording: adding an option to ZLUDA's LLVM argument
  list (`llvm_zluda/src/compile.rs`) makes `LLVMZludaParseCommandLineOptions`
  fail, after which every `cuModuleLoadData` errors out with no diagnostic at all
  — which is why `-mllvm -print-after=…` does not work here, and why the switch
  had to live in the pass.

  **Isolated on one revision.** Two more builds pin the cause down. Plain
  `5ac9102` (the fp8-inline commit alone, no experiment) was built and measured
  against both the DLL that predates that commit and the experiment build:

  | driver | 640×360 frame | avg GPU | throughput |
  |---|---|---|---|
  | pre-`5ac9102` | 88–89 ms | 128.7–130.9 ms | 7.37–7.40 fps |
  | plain `5ac9102` | 89 ms | 128.9 ms | 7.41–7.43 fps |
  | `5ac9102` + this experiment | 142–144 ms | 189.1–189.3 ms | 4.95 fps |

  The first two rows are the same number, and the compiled modules say why. Of
  the modules that now exist in all three generations, the pre-`5ac9102` and
  plain-`5ac9102` cached images are **byte-identical** (equal SHA-256), and for
  eight of them the experiment build is byte-identical too — those are the ones
  with no MMA in them. So the fp8-inline commit is not a factor here, and the
  whole 89 → 144 ms belongs to the experiment: **+62 % on one revision with the
  commit held constant.**

  One module (`105d6e59a254…`, MMA-bearing), all three generations:

  | metric | pre-`5ac9102` | plain `5ac9102` | experiment |
  |---|---|---|---|
  | bytes | 7 689 968 | 7 689 968 | 10 914 200 (+41.9 %) |
  | `v_wmma` | 3 713 | 3 713 | 8 000 (+115.5 %) |
  | `s_swappc` | 4 288 | 4 288 | 0 |
  | `ds_bpermute` | 131 980 | 131 980 | 219 412 (+66.2 %) |
  | `scratch_store` | 25 382 | 25 382 | 47 398 (+86.7 %) |

  State left behind: deployment is the pre-change DLL; the three builds are kept
  as `nvcuda-before-mma.dll` (A), `nvcuda-mine-mma.dll` (B) and
  `nvcuda-no-fuse.dll` (C) under `%TEMP%`; and the cache holds 15 modules × 4
  generations, which is what makes the tables above possible.
  `tools/zluda_module_ab.py` extracts the module inputs out of the snippet,
  compiles them one at a time with each driver, and prints the per-generation
  table straight from the cache, so a codegen question is answered in seconds
  instead of another twenty-minute prewarm. The fork changes stay uncommitted in
  the working tree.

  A warning for whoever measures this next: `video_filter` re-runs the entire
  prewarm when it starts if its warm stamp — which covers the cache database size
  — is stale, and building the comparison above keeps changing that size. A
  benchmark loop over several drivers therefore pays fifteen minutes per driver
  unless `DLSSNR_PRECOMPILE_SKIP=1` is set; with it, a 30-frame run takes about
  five seconds and there is no prewarm in the log at all.

The two tests this section proposed are both gfx11 and both cheap, but only one of
them survives being used: the mma-helper `s_swappc` count going to zero does say
whether the helpers were inlined, which — as measured above — is the thing that
actually costs. A `v_wmma` count on its own says nothing about work done, because
a fused pair and an unfused single each come out as one AMD 16x16 WMMA; it has to
be read next to the pass' own combined/lowered numbers (`ZLUDA_MMA_STATS`). The
proposed `-mllvm -print-after=zluda-combine-mma` does not work at all here: an
extra option in ZLUDA's LLVM argument list fails the parse, and every module load
then fails without a word.

**Blocked, not deprioritised.** These were ranked first in an earlier draft of
this section, on the reasoning that RDNA4 users may be broken while gfx11 users
are only slow. The reasoning holds; the ranking does not, because this project
has no gfx12 card — the development machine is gfx1100 — so it cannot run either
of them.

- **Phase -1 — RDNA4 retest: needs whoever owns the RX 9070 XT.** Until that
  machine re-runs with `HSA_OVERRIDE_GFX_VERSION` corrected, "gfx12 is broken"
  stays a field report rather than a reproduction, and the fp8-Swin module
  failure has no confirmed cause. The run, and the four things worth sending
  back, are spelled out in *For whoever has the RX 9070 XT* below — written as
  one run so the answer does not cost another round trip.
- **Phase 4 — native gfx12 fp8 path: do not attempt without a gfx12 card.** The
  fork's own comment says why — an unverified fragment mapping "would compile and
  silently produce wrong pixels" — and a gfx12 machine is the only instrument
  that can verify one. This is the sharpest case of the block being a hardware
  limit rather than a scheduling choice.
- Every gfx12 number in this document (the 30–50 ms band, the HIP
  corroboration) is **external**. None of it was measured here, and none of it
  can be repeated here.

**Two gates to add (both cheap, neither currently recorded).**

- `vgpr_spill_count` plus `scratch_load`/`scratch_store` site counts **before and
  after**. Fusion raises the f32 accumulator per thread from 4 to 8 VGPRs, and
  the "spills are not the bottleneck" result came from A/B runs that *lowered*
  register pressure — the opposite direction is untested.
- A back-to-back double launch of one kernel with independent data: if the second
  does not add its full time, the kernels can overlap (latency-bound); if it
  doubles, they are issue-bound. Cleaner than inferring from resolution scaling.

**Shipping cost, and the trap under it.** Two separate things have to move for
any of this to reach a user, and each has its own failure mode — see the
"ordering traps" in *What was changed* below. Briefly: the committed `.bc` files
are build inputs and must be regenerated (or the `.cpp` edit changes nothing and
`ZLUDA_PTX_IMPL_DIGEST` does not even move, because the digest covers the `.bc`),
and the LLVM-side change needs the module cache marker in
`zluda/src/impl/module.rs` bumped (or a warm cache keeps serving modules the
pre-change backend compiled). Either miss is silent. Expect one ~20–40 min
re-prewarm per user once both are right.

**Do not quote these until resolved.**

| number | status |
|---|---|
| `~770 K threads = ~24 K wave32s` | **Contradicts** "average grid is 12–24 waves" by ~7–10×. Reconciliation: 770 K / 156 launches = 4 936 threads = 154 waves per launch; and 24 K waves / 84 CU = 286 waves/CU, which would kill the "shader units mostly idle" premise that the latency argument rests on. Reconcile both readings before quoting either. |
| `80–120 ms` 1080p band | Legacy scalar-emulation premise; retracted (24 319 `v_wmma` are present). History only. |
| `~327 total instructions per WMMA` | Whole-cache macro dilution ratio, **not** an MMA-local cost. The hot kernel is far denser. |
| `vgpr_spill_count 118–184` vs `1110 / 886 / 1898` | Different kernels and different register-attribute generations; label the scope whenever cited. |

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

  *(⚠️ 2026-09-13: these two readings are inconsistent by ~7–10×. 770 K threads
  over 156 launches is 4 936 threads = **154 waves per launch**, and 24 K waves on
  84 CUs is **286 waves/CU** — in which case the shader units are *not* idle and
  the latency argument loses the floor it stands on. One of the two numbers is
  wrong. Resolve before quoting either.)*
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
  *(scope 2026-09-13: this is a per-kernel spread across one module. The
  1110 / 886 / 1898 figures further down are the top kernel across three
  register-attribute generations — a different measurement. Label which one is
  meant whenever citing either.)*
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

- `mma1_k0_15` and `mma2_k0_15` share the **exact same SSA value** for $A$ — both derive from the same `fp8_widen_pair` argument — so `FirstA == SecondA` in `CombineMMA.cpp:179` evaluates to `true` and they fuse into a single hardware 16×16 WMMA ($k \in [0..15]$) with combined $B = [b_1[0], b_2[0]]$.
  *(corrected 2026-09-13: the earlier draft credited `EarlyCSEPass` with exposing the identical operands, which it cannot do — in the quoted FPM `EarlyCSEPass` is registered **after** `CombineMMAPass` (`AMDGPUTargetMachine.cpp:903-906`), so it never runs ahead of it within one invocation. The real precondition is one of: $A$ is already a single SSA value as above, or the O3 pipeline has CSE/GVN'd the duplicated widening by the time the pass runs at `OptimizerLastEP`. Neither is guaranteed across pipeline changes, so the relocated FPM should run `EarlyCSE` (ideally `GVN`) **before** `CombineMMAPass`. Without that, a failed pairing is indistinguishable from a pass-ordering problem — two different bugs, one symptom.)*
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
| Result extraction | `dMatrixSplit` + `bpermuteLane` LDS round-trips | Direct register fragment — **unverified, see the note below** |
| Local scaffolding per tile | ~40–80 ops | **~12–16 ops** |
| Accumulator VGPRs per thread | 4 (F32) / 2 (packed F16) | 8 (F32) / 4 (packed F16) |

  Per element the cost is identical (4/128 = 8/256 = 1 VGPR per 32 outputs).
  The fused tile holds more accumulators because it covers twice the output,
  not because fusion is more register-hungry; the register win comes from
  deleting the operand-staging arrays.

> **Open question on result extraction (2026-09-13).** The fused WMMA returns a
> native 16×16 fragment, but the consuming PTX expects *two separate* m16n8
> results — they accumulate into different registers (`{%r623,%r635}` vs
> `{%r642,%r649}` in the quoted PTX). If the consumers do not accept the native
> layout, the split cost **moves** rather than disappears: one split per fused
> tile instead of two per unpaired pair. Still a win, but not zero. Route A's
> `bpermuteLane`/LDS gate is what decides it; until then "direct register
> fragment" is a hypothesis, not a property.

**Sizing the win honestly.** Using cache-consistent figures (24,319 WMMAs against ~7.96 M total instructions, both from the same 7 cached modules): removing ~30–65 scaffolding ops per tile removes **0.73–1.58 M instructions, i.e. 9–20 % of the total** — not the ~2–3× that comparing against scalar emulation would imply. These kernels are latency-bound, however, and serialized `bpermuteLane` LDS round-trips cost far more in cycle latency than in issue slots, so measured latency reduction may significantly outperform this static estimate. **That gap is precisely why Route A is a measurement, not an argument.**
  *(added 2026-09-13: the 9–20 % is a **whole-cache dilution ratio** and almost
  certainly understates the hot kernels. This same document records the top
  kernel at 28 220 instructions carrying 256 `mma.sync` — 512 unpaired WMMAs, or
  128–256 tiles at 40–80 scaffolding ops each, i.e. **5 K–20 K instructions,
  18–70 % of that one kernel**. Scaffolding is concentrated in the chained
  kernels, so the honest projection is per-kernel: `ZLUDA_LAUNCH_TIMING=1`
  per-kernel time × that kernel's scaffolding share, summed — not a cache-wide
  ratio applied to the frame. Note "tile" is used in two senses in this document
  (one `mma.sync`, or one fused 16×16 pair) and the range above spans both; pin
  the definition before quoting it.)*

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
   O0 + sed rebuild.
   *(mechanically confirmed 2026-09-13 in the vendored tree rather than by
   disassembly: `ext/llvm-project/llvm/lib/IR/Verifier.cpp:2352-2354` states
   "Attribute 'optnone' requires 'noinline'!", so a source marked
   `[[clang::optnone]]` is guaranteed to carry noinline in the IR and
   `s/optnone//g` cannot help leaving it behind. The observation and the rule
   agree, and the noinline is not a leftover of something else. Note the
   companion rule at `:2348` — noinline and alwaysinline are mutually exclusive —
   which is why the fix below cannot be a bare `sed 's/^define/define
   alwaysinline/'`: it would produce a module `llvm-as` rejects unless the
   noinline is stripped first.)* Consequence: **the intrinsics never reach the kernel's
   basic blocks at any pipeline stage, so pairing is structurally
   impossible until this is fixed.**   Fix: add `sed 's/noinline//g'` to both
   bitcode build pipelines (the two intentional `noinline` sites —
   `__assert_fail`, `vprintf` — become inlineable, a harmless size change
   worth watching in the vgpr/spill notes). `ZLUDA_PTX_IMPL_DIGEST` bumps,
   invalidating the cache once by design.
   *(added 2026-09-13: removing `noinline` is necessary but **not sufficient**,
   and a Step 0 that stops there can return an inconclusive gate. Stripping the
   attribute only makes inlining *legal*; it does not make it *happen*. The
   callee will carry no inline hint of its own — the `[[clang::always_inline]]`
   in the C++ source is exactly what the O0 + sed rebuild already fails to
   preserve — while its body is large (8-element staging arrays plus
   `bpermute`/gather scaffolding). LLVM's cost model is free to decline. So the
   "mma-helper call count → 0" gate could fail for a reason that has nothing to
   do with pass timing, re-merging the two variables Step 0 exists to separate.
   Restore `alwaysinline` — and because noinline and alwaysinline are mutually
   exclusive (`Verifier.cpp:2348`), the route taken is to stop clang emitting the
   noinline at all: the `[[clang::optnone]]` was removed from the three mma
   wrappers in `ptx/lib/zluda_ptx_impl.cpp`, which is what lets the
   `[[clang::always_inline]]` already present at their call sites be honoured at
   .bc build time (evidence that clang does inline these: `fp8_mma_half`, a
   `static inline` neighbour, is absent from the shipped .bc's symbol table), and
   `sed 's/noinline//g'` was added to both pipelines for whatever noinline is
   still emitted. So the gate tests the hypothesis rather than the inliner's
   budget. Second-order hazard to watch: the `optnone` wrapper that the
   same build strips exists *because* ZLUDA's own passes can optimise the
   intrinsic away — so gate on the `llvm.zluda.mma` intrinsic count remaining
   unchanged as well, not merely on calls disappearing.)*
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

*(Two figures, two premises: **232–264 ms** is derived from cache-consistent static instruction scaling — the 9–20 % removal, measured on the 640×360 cache, applied to the **290 ms measured GPU average** from the solid-color runs; the cross-resolution extrapolation rides on the **scaffolding share of the instruction mix** being resolution-independent (the earlier "per-kernel latency is resolution-independent" wording does not survive this document's own numbers — 78.4 ms at 640×360 against 290 ms at 1080p is 3.7× for 9× the pixels) — and is the defensible baseline; **80–120 ms** is the legacy figure from the earlier scalar-emulation premise and requires LDS-latency removal to compound. Route A replaces both with a measurement).*

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
  **11.0.0**, which at the time suppressed the RDNA4 auto-config (injection then
  ran only when the variable was unset) and made ZLUDA compile gfx11 ELF for a
  gfx1201 device. **Still not confirmed whether removing it fixes the run** —
  that is the open question, and it is what the hand-off section below exists to
  close.
  *(line reference corrected 2026-09-13: that injection is at
  `gpu_detection.h:162-174`, not `:109-119`, and it no longer merely defers to an
  unset variable — it corrects a mismatched one. See consequence 2.)*

Consequences:

1. **Phase 4 may be a correctness prerequisite, not a performance nicety.**
   Today gfx12 falls into the `>= 11000 && < 13000` f16-widen branch
   (`zluda_ptx_impl.cpp:1424`), i.e. RDNA4 is handed the RDNA3 WMMA form.
   Whether that is merely slow or genuinely uncompilable on gfx12 is exactly
   what this report cannot yet distinguish — and resolving it should outrank the
   gfx11 frame-rate work, because gfx11 users are "slow" while RDNA4 users may
   be "broken".
   *(2026-09-13: correct about severity, misleading as an instruction. This
   project holds no gfx12 hardware — the development machine is a 7900 XT
   (gfx1100) — so it cannot be worked on here at all, which is why the ordered
   plan in Current conclusions puts the RDNA4 items under a separate "blocked"
   list instead of first. The gfx11 lever is the only one that can be developed
   and measured by whoever holds this repository.)*
2. ~~**`gpu_detection.h` must not defer to an obviously mismatched override.**~~ —
   **implemented (2026-09-13)**: the RDNA4 branch now corrects a non-gfx12
   override to 12.0.1 with a loud auto-config message, and the batch GUI
   surfaces the same warning in its log window at startup
   (`BatchWindow::hint_override_mismatch`). Note the original design intent
   visible at `zluda_ptx_impl.cpp:1201` — the optnone wrapper exists to stop
   ZLUDA-specific passes from optimizing away the intrinsic — which is why
   the fix strips `noinline`/`optnone` in the bitcode build rather than
   removing the wrapper blindly.

### For whoever has the RX 9070 XT — what to run, and what to send back

*(added 2026-09-13. Neither the retest nor Phase 4 can be run by this repository
— the development machine is a 7900 XT (gfx1100) — so the gfx12 question is
handed over rather than worked on. This is written so that one run produces
everything needed, instead of another round trip.)*

**Run the job that failed, on the build that carries the override correction, and
change nothing else.** `gpu_detection.h` now rewrites a non-gfx12
`HSA_OVERRIDE_GFX_VERSION` to `12.0.1` and says so in the startup log; the batch
GUI repeats the warning in its log window. The only question this run answers is
whether the fp8-Swin translation failure was the stale override or something in
gfx12 itself — every other variable you change destroys that answer.

**Remove the stale value at the source first.** If `HSA_OVERRIDE_GFX_VERSION` was
set system-wide (the older guides that recommend `11.0.0` are the usual cause),
delete it there rather than relying on the auto-correction. A machine-level value
is precisely what that correction is warning about, and leaving it set means the
next tool you run — not this one — inherits the stale value.

**Send back:**

1. The complete `[GPU-AutoConfig]` line from startup. It names the detected card
   and either the injected or the corrected override, and that is what separates
   "the override was the cause" from "the override was irrelevant".
2. `rocminfo` (or `hipInfo`) output, or at minimum the **gfx target** it reports.
   `gfx1200` and `gfx1201` are different parts and the auto-injection hardcodes
   `12.0.1` for both — if your card reports `gfx1200`, that value is being
   assumed rather than known.
3. The full precompile log, not just the summary line, from

   ```
   video_filter.exe --precompile nvngx_dlssnr.dll nvcuda.dll 1
   ```

   The trailing `1` is the job count: it serialises the translation so the
   failing module is unambiguously the one the failure appears after, and it
   bypasses the adaptive memory gate (`adaptive` is true only when the count is
   0). Budget for a full pass: the warm-start stamp is written only when every
   module succeeds, so a failed precompile leaves the cache cold and this re-runs
   all fifteen rather than short-circuiting. What matters is the module index and
   whether the child failed to *translate* or the module translated but then
   would not *load* — different bugs behind the same headline.
4. Whether a frame now appears, and **whether it is visually correct**. gfx12
   takes the `__oclc_ISA_version >= 11000 && < 13000` branch
   (`zluda_ptx_impl.cpp:1424`) — the RDNA3-shaped f16-widen path, which has only
   ever been measured on gfx11. "It ran" and "it is right" are separate answers,
   and this project cannot check the second one.

**What not to send back: a frame time.** It is interesting and it is not the
question, and a number from a machine whose translation path is still
unconfirmed would get quoted out of context in exactly the way the gfx12
projection already was.

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

*(ordering superseded 2026-09-13: a Phase -1 (the RDNA4 retest, one run) now
precedes Step 0 — see Current conclusions at the top. The steps themselves are
unchanged and still accurate.)*

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
4. **RDNA4 branch**: *(corrected 2026-09-13 against the fork's own source.)* The gate in `zluda_ptx_impl.cpp:1424` is `__oclc_ISA_version >= 11000 && __oclc_ISA_version < 13000`, which **already swallows gfx12** — so this is not "add a `>= 12000` branch ahead of it", it is "carve gfx12 out of it". More importantly, the blocker is not a branch: the comment at `zluda_ptx_impl.cpp:1312-1325` records that there is **deliberately no native fp8 path yet**, because (a) the PTX→AMD fragment mapping differs (PTX `m16n8k32` carries sixteen A bytes per lane over eight columns, the AMD instruction eight bytes over sixteen), so **pairs of PTX operations must be fused**, and (b) **`CombineMMAPass` has no case for a native fp8 instruction** — it recognises only `zluda_mma_m16n8k16_f32_f16_f16_f32`, its bf16 twin and `zluda_mma_m16n8k32_s32_s8_s8_s32` (`CombineMMA.cpp:66-91`). Read that narrowly: today's fp8 MMAs already lower through `fp8_mma_half` into the **f16** intrinsic, so they *are* pairable by Route A as things stand. What has no case is routing fp8 to gfx12's native fp8 WMMA. An unverified mapping "would compile and silently produce wrong pixels", which is why it was left out rather than guessed. Prerequisite ordering therefore is: fp8 pairing case in the pass + a hardware-verified fragment mapping, *then* the gfx12 gate. A gfx12 machine is the only instrument that can verify the mapping — which is the second reason the RDNA4 retest comes first.

### What was changed (2026-09-13)

Two edits, left uncommitted, and **neither is verified by a build on this
machine**:

| file | change |
|---|---|
| `ext/llvm-project/llvm/lib/Target/AMDGPU/AMDGPUTargetMachine.cpp` | the `CombineMMAPass` / `LowerMatrixConversionsPass` FPM moved from `registerOptimizerEarlyEPCallback` to `registerOptimizerLastEPCallback`, with `EarlyCSEPass` + `GVNPass` inserted **ahead** of the combiner (pairing is gated on `FirstA == SecondA`, and inlining alone does not unify the duplicated inlined operand chains). 40 insertions, 13 deletions. |
| `ptx/lib/zluda_ptx_impl.cpp` | `[[clang::optnone]]` removed from the three mma wrappers, so the `[[clang::always_inline]]` already at their call sites survives to the `.bc`; `sed 's/noinline//g'` added to **both** bitcode pipelines, with the header comment updated to record that the block is the only description of how the committed `.bc` was produced — and how to read the result back. |

A third change was drafted and then dropped, and the way it died is the reason it
is recorded here. The theory was that the fp8 path's `A` operand chain runs
through a lane-id read (`fp8_mma_half` picks which byte pair this lane takes from
`sreg_laneid()`), so an explicit `__attribute__((const))` on that function would
be needed to make the two halves of one `m16n8k32` produce a CSE-able operand.
Disassembling the shipped `.bc` retired it: the function has no call sites at all
(it is fully inlined) and FunctionAttrs already annotates it `memory(none)`. It is
left out rather than kept as harmless insurance, so the change set stays bounded
by what the evidence supports.

**Two ordering traps that make all of this a no-op if missed.**

1. `ptx/lib/zluda_ptx_impl.bc` and `_constrained.bc` are committed **build
   inputs**, and they have **not** been regenerated. Until they are, the `.cpp`
   changes reach no artifact at all, and `ZLUDA_PTX_IMPL_DIGEST` will not even
   move, because `zluda/build.rs` digests the `.bc` files, not the `.cpp`.
   *(corrected 2026-09-13: an earlier draft called this "the Linux + ROCm
   pipeline". It is not platform-specific, and the only two things in that header
   that look like they are prove to be nothing of the sort — `/opt/rocm/.../ocml.bc`
   is the Linux spelling of a file the Windows HIP SDK ships at the same relative
   path (`C:\Program Files\AMD\ROCm\7.1\amdgcn\bitcode\ocml.bc`, 209 104 bytes,
   with `bin\clang.exe` beside it), and the POSIX shell is Git Bash
   (`D:\Git\usr\bin\sed.exe`). What is genuinely required is **the LLVM built from
   `ext/llvm-project`**, because that tree is where the intrinsics are defined:
   `llvm/include/llvm/IR/IntrinsicsZLUDA.td` declares
   `int_zluda_mma_m16n8k16_f32_f16_f16_f32` and friends with
   `[IntrNoMem, IntrConvergent]`. The `__asm("llvm.zluda.mma...")` declarations in
   the .cpp only become intrinsics at all under that build; a stock LLVM sees an
   unknown `llvm.*` name, and the attributes the pairing and legalisation rely on
   go with it. That build — not an operating system — is what is missing here.)*

2. The LLVM change alters emitted code, but the module cache key
   (`zluda/src/impl/module.rs:407`) freezes `VERGEN_GIT_SHA` before the commit
   lands and its explicit marker covers only the Rust-side translation passes.
   Bump that marker (it currently reads `/fp8-inline-r1`) in the same change, or
   every user with a warm cache keeps running the modules the pre-change backend
   compiled and *nothing* of the above is observable.

**Verified since (2026-09-13) — the attribute question is no longer open.** This
machine has no `ext/llvm-project/build`, but it does not need one to *read* the
bitcode: the HIP SDK ships the LLVM tools (`llvm-dis.exe`, `llvm-as.exe`,
`clang.exe` under `%HIP_PATH%bin`, with `HIP_PATH = C:\Program Files\AMD\ROCm\7.1`),
and `llvm-dis ptx/lib/zluda_ptx_impl.bc -o -` disassembles cleanly into 9 378
lines of IR. What it shows:

| site | attribute group |
|---|---|
| `_ZL49__llvm_zluda_mma_...` wrapper definition | `#16 = { convergent mustprogress noinline nounwind ... }` |
| `__zluda_ptx_impl_mma_sync_aligned_*` outer helpers | `#14 = { mustprogress noinline nounwind ... }` |
| the wrapper's call sites | `#24 = { convergent noinline nounwind }` |

So noinline is on the callee *and* on the call site — the Step 0 premise holds
as stated, and the group number and contents match the disassembly quoted above.
One correction to that earlier note: the call-site group is `#24` and it does
carry noinline (the note recorded `#23 = { convergent nounwind }`), which
strengthens the case rather than weakening it. A second observation from the same
dump, which retired a change: `__zluda_ptx_impl_sreg_laneid` has **no call sites
at all** (fully inlined into its users) and FunctionAttrs already annotates its
neighbours `memory(none)`, so an explicit `__attribute__((const))` on it would
have been redundant and was dropped.

**Still not verified.** Neither edit has been compiled, `llvm-dis` only reads,
and the `.bc` has not been regenerated. The cargo-side LLVM tree
(`target/release/build/llvm_zluda-*/out/build/`, which has a `build.ninja`) can
supply the *writing* tools without a full rebuild — `ninja llvm-as llvm-dis clang`
there links against libraries already built — and it should be the one used for
the writing side regardless of convenience, because a `.bc` written by the HIP
SDK's LLVM 21 is not guaranteed to be readable by the older vendored LLVM that
translates modules.

### Toolchain: regenerating the bitcode on this machine (2026-09-13)

Everything needed is already installed. Nothing has to be built except two tools,
and no Linux is involved. Inventory, verified by running it rather than by
reading the comment:

| need | where | note |
|---|---|---|
| `clang` (compiles the `.cpp`) | `%HIP_PATH%bin\clang.exe` | HIP SDK 7.1, `clang 21.0.0git`; `HIP_PATH = C:\Program Files\AMD\ROCm\7.1` |
| `ocml.bc` | `%HIP_PATH%amdgcn\bitcode\ocml.bc` | 209 104 bytes — this is the Windows spelling of the pipeline's `/opt/rocm/amdgcn/bitcode/ocml.bc` |
| `llvm-dis` | `%HIP_PATH%bin\llvm-dis.exe` | reads the committed `.bc` cleanly (9 378 lines) |
| `llvm-as` | `target\release\build\llvm_zluda-*\out\build\bin\llvm-as.exe` | **not built yet**; `ninja llvm-as llvm-dis` there links against libraries that are already built. That tree's `LLVM_MAIN_SRC_DIR` is `ext/llvm-project/llvm`, so it *is* the patched LLVM 22 |
| a POSIX shell for the `sed` chain | `D:\Git\usr\bin\sed.exe` | or replicate the chain in PowerShell, which is what the script does |

`clang` is deliberately not in the cached LLVM tree
(`LLVM_ENABLE_PROJECTS=llvm;lld`), and adding it would mean a reconfigure. It
turns out not to be needed, for the reason below.

**The non-obvious part, and why mixing the two toolchains is safe.** A function's
intrinsic-ness is not something the bitcode records — it is a **name lookup done
by whoever reads the module** (`Function::getIntrinsicID` consults that LLVM's own
table). An assembler that has never heard of `zluda_mma_*` can therefore write a
valid `.bc` that contains the names, and the patched LLVM 22 which translates the
modules will resolve them to its intrinsics when it reads the file. Verified:
round-tripping the committed `.bc` through the HIP SDK's `llvm-dis`/`llvm-as`
returns a file of **exactly the same size, 71 616 bytes** (sizes compared, not
hashes), with the `llvm.zluda.mma.m16n8k16.f32.f16` names intact. What the
writer's version *does* decide is the bitcode format version — which is the one
reason the final assembly should still be the patched `llvm-as`, since the fork's
LLVM has to read the result back.

**The recipe** — also as `ptx/lib/rebuild-bitcode.ps1`, written next to the
pipeline comment it implements. That script's parse is checked and its
dis/sed/as half is exercised, but it has never been run end to end:

1. `ninja llvm-as llvm-dis` in the cached LLVM tree above.
2. Per variant: `clang` → `.bc`; `llvm-dis` → text; the `sed` chain including the
   new `s/noinline//g`; patched `llvm-as` → the committed path.
3. Bump the cache marker in `zluda/src/impl/module.rs`, then rebuild ZLUDA.

**Gates, in order. No frame time is worth measuring before 0a.**

| # | gate | how, and why it is the gate |
|---|---|---|
| 0a | the mma wrapper is **gone** from the new `.bc` | `llvm-dis new.bc -o new.ll`, then count `_ZL49__llvm_zluda_mma` in `new.ll` and expect **0**. This is the Step 0 gate — deliberately *not* the `alwaysinline` count, which points the other way: the attribute is consumed by the inlining it causes, so success leaves nothing to count. An earlier version of this table specified `alwaysinline` and thereby reported a success as a failure |
| 0b | `noinline` gone | same dump, expecting 0. **Already verified** against the committed `.bc` with the sed applied: 5 → 0, `llvm-as` accepting the result, `llvm.zluda.mma` names preserved |
| 1 | mma-helper `s_swappc` count → 0 | disassemble a compiled module; asymmetry with the total `s_swappc` count is what separates the two variables |
| 2 | `v_wmma` halves on module 14 | `-mllvm -print-after=zluda-combine-mma` |

**Step 0 is verified on the real artifact (2026-09-13).** Both `.bc` files were
regenerated — clang from the HIP SDK compiled the source, the HIP SDK's
`llvm-dis` disassembled it, the `sed` chain ran, and the patched LLVM 22's
`llvm-as` reassembled it — and the result was checked structurally rather than by
counting attributes:

| | committed | regenerated |
|---|---|---|
| `noinline` | 5 | **0** |
| mma wrapper (`_ZL49__llvm_zluda_mma`) references | 3 (one definition, two calls) | **0** |
| direct `llvm.zluda.mma.m16n8k16` call sites | 2 — one inside the wrapper | **3, all inside `FUNC(...)` helpers** |
| …of those, inside the fp8 helper | 0 | **2** (k 0..15 and k 16..31) |

The wrapper disappearing *is* the result: it is gone because it was inlined, so
the intrinsics now sit in the helper bodies instead of behind a `noinline` call.
`noinline` reaching zero also frees the **outer** helpers, which carried it too
(`#14 = { … noinline … }` in the committed file) and therefore could never be
inlined into the kernel — meaning Route A could not have worked against the old
`.bc` even with the pass moved. Sizes: 71 616 → 69 700 and 70 868 → 68 988 bytes;
the two outputs remain distinct files.

**Still open.** Whether those outer helpers then inline into the *kernel* at
module-translation time — that is what gates 1 and 2 test — and neither fork edit
has been compiled yet. The constrained variant also emits clang warnings
(`-ffp-model=strict` overridden by `-ffp-exception-behavior=ignore`, and an
unsupported rounding mode); those come from the flags in the pipeline header, not
from the changes, and the two outputs differ.

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
