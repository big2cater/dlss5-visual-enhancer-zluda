# RDNA4 precompile failure: `Cannot select: intrinsic llvm.amdgcn.wmma.f32.16x16x16.f16`

2026-09-14. Reported on an RX 9060 XT (gfx12, 16 GB), reproduced and diagnosed on
the RX 7900 XT development machine **without gfx12 hardware**.

## The report

```
7 of 15 modules could not be translated
[precompile-wait] 预热完成 (exit 1)
...
LLVM ERROR: Cannot select: intrinsic %llvm.amdgcn.wmma.f32.16x16x16.f16
>>> 进程异常崩溃 exit=-1073740791        (0xC0000409)
```

"Every RDNA4 card fails to precompile; only `DLSSNRFilter-ZLUDA-speed-test-2026-09-09-v2`
and earlier work on RDNA4."

## Root cause

Also described in the previous session's summary — see the conversation for the
full trail. In short:

- Five helpers in `ptx/lib/zluda_ptx_impl.cpp` branch on `__oclc_ISA_version >= 11000 && < 13000`
  to reach the native WMMA intrinsics (f16, bf16, s8, and both FP8 sites through
  `fp8_mma_half`).
- `int_amdgcn_wmma_f32_16x16x16_f16` **is** pattern-matched for gfx12 — but only for
  a different operand shape. gfx11's instruction is typed for `v16f16` A and B
  (`VOP3PInstructions.td:1439`, `VOP_V8F32_V16F16_V16F16_V8F32`); gfx12's is typed
  for `v8f16` (ibid:1991-1992, `F32_F16_WMMA_w32` is `[v8f32, v8f16, v8f16, v8f32]`).
  These helpers build the gfx11 shape (PTX m16n8k16 hands a lane its A fragment as
  four 32-bit registers), so instruction selection finds nothing to match and
  aborts the whole compilation.
- Consequently the failure is deterministic and total on gfx12: every module that
  contains an MMA dies, and the process aborts rather than falling back.

## Why it was fixable without a gfx12 card

The error happens during **code generation**, before anything executes, and the
target architecture comes from a string. `compiler/src/main.rs` takes `--arch`, so
the offline compiler can be pointed at gfx12 on any machine:

```
zoc --arch gfx1201 ptx/src/test/spirv_run/mma_m16n8k16_f32_f16_f16_f32.ptx
  → exit 0xC0000409, LLVM ERROR: Cannot select: intrinsic %llvm.amdgcn.wmma.f32.16x16x16.f16
zoc --arch gfx1100 <same input>
  → exit 0, .elf written
```

The reported failure and the offline one are the same error and the same exit code.
This turned the issue from "no hardware, cannot be worked on" into a locally
reproducible, locally verifiable one — and it is how step A below is accepted.

## Step A, shipped: gfx12 takes the software fallback

The five sites now share one predicate:

```c
#define ZLUDA_HAS_NATIVE_WMMA (__oclc_ISA_version >= 11000 && __oclc_ISA_version < 12000)
```

gfx12 therefore drops into the fallback path each of these helpers already carried
for gfx10 — correct, and slower than a native path would be. That is the trade
until step B (a good gfx12 fragment mapping, verifiable only on a card).

Verification, all offline (`zoc`), after rebuilding the bitcode **and** `zoc`
itself — both embed `ptx/lib/zluda_ptx_impl.bc` at build time
(`ptx/src/pass/mod.rs:37` is `include_bytes!`), so regenerating the `.bc` without
rebuilding the consumer tests the old bitcode:

| fixture | gfx1100 | gfx1201 |
|---|---|---|
| `mma_m16n8k16_f32_f16_f16_f32` | exit 0, `v_wmma` ×1 | exit 0, `v_wmma` ×0 |
| `mma_m16n8k16_f32_bf16_bf16_f32` | exit 0, `v_wmma` ×1 | exit 0, `v_wmma` ×0 |
| `mma_m16n8k16_f32_bf16_bf16_f32_2x` | exit 0, `v_wmma` ×1 | exit 0, `v_wmma` ×0 |
| `mma_m16n8k32_s32_s8_s8_s32` | exit 0, `v_wmma` ×2 | exit 0, `v_wmma` ×0 |
| `mma_m16n8k32_s32_s8_s8_s32_interleave` | exit 0, `v_wmma` ×4 | exit 0, `v_wmma` ×0 |

Both halves matter: gfx12 now compiles, **and** it demonstrably takes the software
path (no `v_wmma` in the output) rather than some accidental native form; gfx11 is
untouched (still native).

Cache keys move by themselves for this change, which was checked at the source
rather than assumed: `zluda/build.rs` hashes both `.bc` files (FNV-1a) into
`ZLUDA_PTX_IMPL_DIGEST`, which is part of the module cache key
(`zluda/src/impl/module.rs`, `get_cache_key`). Observed in practice while
verifying: a driver built with the new bitcode re-translated all 15 modules instead
of serving the warm cache. No manual marker was added, so this does not force
gfx11 users through a needless re-translation.

## What this costs RDNA4 users, stated plainly

- **Precompile is slow and runs to completion.** The fallback expands every MMA
  into software; the source comment for the gfx11 branch already notes "a DLSS
  module with a thousand of them takes minutes to compile". On the 9060 XT this
  means a long first precompile rather than an error — better than a crash, worse
  than the native path would be.
- **Frame times on gfx12 will be worse** than the numbers quoted for gfx11 in the
  release note. Those figures are RX 7900 XT / gfx11 and must not be read as
  gfx12 expectations.
- **Not verified on a gfx12 card by this project.** Acceptance so far is the
  offline codegen check above; the first real confirmation will be the reporter's
  precompile completing and a clip processing.

## Still open (step B)

A native gfx12 path needs a verified fragment mapping, and that verification needs
a card. Two concrete options, in the order they look worth trying:

1. The gfx12-native FP8 instruction (`v_wmma_f32_16x16x16_fp8_fp8`, and the
   gfx12.5 `16x16x64` forms) — FP8 is where the ray-reconstruction kernels spend
   their time, and RDNA4 has the instruction the FP8 helpers currently widen away
   from.
2. The gfx12 f16 form with its own operand shape (`v8f16` A and B) — a smaller
   change than FP8, but it only helps the f16 path.

Both need `CombineMMAPass` (which pairs MMAs sharing an A operand) to understand
the new shape, and both need a gfx12 card to confirm the mapping does not silently
produce wrong pixels.

## The second one, found while re-checking the first: the GPU was mis-identified

Not part of the codegen failure, but it sat on the same path and would have broken
cards that work today, so it is recorded here.

`core/gpu_detection.h` decides whether a card is RDNA 4 by name and by PCI device
id, and a "yes" makes it set `HSA_OVERRIDE_GFX_VERSION=12.0.1`. The id test
accepted `0x7480..0x74DF` -- which is **Navi 33**, the RX 7600 and its siblings
(0x7480 is the RX 7600 itself; the block also covers the 7600 XT, 7650 GRE, 7400
OEM part, 7600M XT, 7600S, 7700S and PRO W7600). RDNA 4 reports **0x7550**
(Navi 48) and **0x7590** (Navi 44), so the id half matched RDNA 3 and no RDNA 4
card at all, while the name half listed two models that do not exist (RX 9080 and
RX 9090 -- RDNA 4 has no flagship) and missed the workstation Radeon AI PRO R9700.
Because of that override, the cards it did match were handed the architecture of a
chip they are not, and per the header's own note the runtime then builds gfx12 ELF
the driver refuses to load: every module fails before the first frame.

Introduced by `9154fa0` (2026-09-11) as `0x7480..0x749F` and widened to `..0x74DF`
by `7b38195` (2026-09-12). `BUG_REVIEW_2026-09-11_FRESH.md` discusses this same
function (C8) and reaches the right conclusion -- use the PCI id table rather than
model strings -- but records the ids backwards, describing `0x7480..0x749F` as
"Navi 48, missing Navi 44". Following that literally would have kept the RDNA 3
range in place and added 0x7590 beside it, leaving the RX 7600 broken; the fix has
to remove the RDNA 3 block rather than extend it.

Fixed in `dcf7873`: the predicate is now `is_rdna4_gpu()` at the top of
`core/gpu_detection.h`, keyed on 0x7550/0x7590 plus the real model names, and
`tools/test_rdna4_detect.cpp` drives it with 28 real name-and-id pairs so that the
next change to it is checked against cards rather than against a comment.

Two things in that area remain open and are not fixed by any of the above:

- **Navi 44 gets gfx1201's override.** The injection value is a constant
  `12.0.1`, which names gfx1201 / Navi 48; a 9060 XT is gfx1200 and would want
  `12.0.0`. The reported 9060 XT did reach translation with the mismatched value,
  so it is evidently not fatal there -- but whether code compiled for gfx1201 is
  fully correct on gfx1200 is not something this machine can establish.
- **The auto-configuration cannot be seen.** Its messages go to
  `OutputDebugStringA` and `stderr`, and `dlssnr_gui.exe` is a GUI: the line
  saying what was detected and what was injected is invisible to the user, which is
  precisely the line needed to debug an RDNA 4 report.
