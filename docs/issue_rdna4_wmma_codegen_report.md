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
