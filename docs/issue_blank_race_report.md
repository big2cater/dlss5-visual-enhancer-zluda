## Summary

Measured data for the "intermittent blank picture" issue (README known issue) plus two related problems found on an **AMD RX 7900 XT (RDNA3, gfx1100)**:

1. **~50-60% of fresh processes produce a fully black image** — sticky within a process, fresh processes mostly recover, but "black storms" of 3-6 consecutive processes occur.
2. **ZLUDA ComputeCache does not persist** — every new process re-translates all 15 modules (~40-90 s) although `%LOCALAPPDATA%\zluda\ComputeCache\zluda.db` + `zluda2.db` exist (~61 MB each).
3. **Translation children can hang with zero CPU progress** — one compile-one child used 3.5 s CPU over 7+ minutes (parked on a driver/cache lock), stalling the whole pipeline because the parent waits for children indefinitely.

## Environment

- GPU: AMD Radeon RX 7900 XT (gfx1100, RDNA3, 20 GB)
- HIP SDK: ROCm 7.1 (`C:\Program Files\AMD\ROCm\7.1`)
- Windows 11, local physical desktop session
- `nvngx_dlssnr.dll` 310.8.0.0 (SHA256 e16bcf15...)
- Your release zip `first_release` DLLs (nvcuda.dll, nvapi64.dll, nvngx.dll), no lib changes
- Repro: `video_filter --image <photo> out.png nvngx_dlssnr.dll nvcuda.dll nvngx.dll nvapi64.dll --passes 3` (a 2516x1340 photo; smaller inputs behave the same)

## 1. Blank-output race — measurements

| Batch | first-attempt black rate |
|---|---|
| still image, 5 rounds | 2/5 black |
| still image, 6 rounds | 4/6 black |
| precompile jobs=16, 6 rounds | 4/6 black |
| precompile jobs=1 (serial), 6 rounds | 3/6 black |
| precompile jobs=16, 6 rounds | 4/6 black |

Observations:

- Black output is **deterministic in value**: RGB ≈ 1-3/255 (mean≈2.0/255, std≈0.8) for every black run, regardless of input size or content.
- **Sticky inside a process**: all 4 re-evaluations in a black process were black; re-evaluating does not help.
- **Fresh process usually recovers**, but consecutive black processes do happen (up to 6 in a row observed).
- Timing differs: black-run evaluations take ~300 ms/pass vs ~830 ms/pass for good runs — the kernels appear to not execute at all (no-op launch) rather than produce garbage.
- The NGX log is byte-for-byte equivalent between black and good runs (up to `EvaluateFeature` / `PollRuntimeParams - callback is NULL`) — the silent failure sits in the CUDA/HIP execution layer, not in NGX.
- Serial precompile (jobs=1) does **not** affect the black rate → the race is not caused by parallel cache writes.

## 2. ComputeCache never persists

- Every new process re-runs all 15 module translations (progress logs "translated 1..15 of 15" every run, ~40-90 s) even with `zluda.db`/`zluda2.db` present and growing.
- Your own precompile.cpp comment expects a warm run to take ~2.6 s ("a fully warm run of all fifteen modules of this network took 2.6 seconds").
- This makes every attempt expensive and, combined with issue 3, creates a 17-process HIP/driver bring-up burst per attempt.

## 3. Hung translation child

- One `--compile-one` child measured **3.5 s CPU over 7+ minutes** (zero progress, parked in a wait), while 16 other children + the main process loaded HIP 7.1 on the same GPU at the same time.
- Precompile waits on children with no timeout, so a single hung child stalls the entire run (and the GUI) indefinitely.

## Workarounds we shipped locally (for transparency)

- Blank detection + **process-level retry** (re-execute in a fresh process, `--retries N`; a blank result makes the runner report a retryable exit code).
- **First-frame gate for video**: a black first frame means the whole run is black → abort early and retry.
- **Watchdog**: translation children with zero CPU progress for 3 minutes are killed and counted failed.
- **Concurrency cap**: default translation parallelism capped at 4 (was adaptive up to 16); `DLSSNR_PRECOMPILE_JOBS=1` forces serial.

## Follow-up 2026-09-14: the local gate was over-eager

The first-frame gate described above condemned a whole class of footage. It asked
"output blank" and "input carries signal", and a fade-in from black satisfies both
on the frame that first crosses sRGB 27/255 — that frame is itself nearly black,
with a black network history behind it, so a correct output sits under the blank
bar too. Every retry re-reached that same frame, so the result was a deterministic
deadlock rather than the intermittent fault the gate was built for: a trailer with
a 0.23 s black opening failed at frame 7 in five consecutive fresh processes, then
an orchestrator shard restart, then a user-facing abort.

The gate now requires the input to be far brighter than the blank bar (sRGB
~93/255, about 22x the 0.005 linear bar) and the condition to persist for three
consecutive frames. Verified on that file (it now completes, warning once at a
genuinely dark frame — input 24672/65535 — that did not persist), on the ordinary
regression clip (unchanged: 106.4 ms/frame, `blanks=0`, exit 0), and on the abort
path itself with a temporarily forced blank verdict (warns on two consecutive
frames, aborts on the third, exit code 2).

Unchanged by this: the race the gate exists for is rarer than this report measured
— 0 of 24 fresh processes on 2026-09-14 against 2/5 to 4/6 on 2026-09-10 — but it
has not been shown to be gone.

Deliberately not changed here: the **still-image** path gates on
`output_is_blank(out) && image_has_signal(in)` — the same pair of tests whose
collision at a fade-in boundary this fix addressed for video — so a dark still can
be retried as a false blank. The symptom is milder (a still cannot deadlock an
entire film; it burns a few retries and reports blank), and fixing it means giving
that path its own brightness measure instead of reusing the video gate's
frame-level one.

## Requests

- Any hint on the blank race: the deterministic near-zero output with identical NGX logs suggests a launch that returns success without executing — is there a `hipGetLastError`-style check that could be surfaced per kernel? Happy to run diagnostic builds.
- Why does the cache not persist? Disk path/keying/env (`ZLUDA_CACHE_*`?) — would love the expected layout so we can verify writes.
- For the hang: is the driver/HIP initialization serialization between concurrent processes known? We'd be glad to collect whatever trace you need (we can run with extra logging on this card).