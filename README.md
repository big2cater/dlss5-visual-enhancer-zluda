# DLSS 5 Image Enhancer - with AMD/Zluda support
This project is a proof-of-concept tool which uses Nvidia's DLSS5 to enhance a single picture.
It also was made as a testing ground for my attempt of running DLSS5 on AMD gpus, specifically RDNA4 and RDNA3 (tough fp8 emulation).
Older gpus *may* work too, but they aren't the target for this experiment.

It runs thanks to [my own Zluda Fork](https://github.com/RedDukeDev/ZLUDA), which implements the missing features required by the DLSS5 network.

I also forked Zluda's version of LLVM and made a small change which should, in theory, make possible to use the native FP16 hardware on supported cards instead of relying on software emulation, you can find it [HERE](https://github.com/RedDukeDev/llvm-project)

## How to use
Download the zip from the [Release section](https://github.com/RedDukeDev/dlss5-image-enhancer-zluda/releases), and run dlss5-image-enhancer.exe
On the top-right side, you have to select the required DLLs. 
For AMD, nvcuda.dll and nvapi64.dll are already included inside the "zluda" directory. they aren't the official nvidia libraries, those are actually from the Zluda project.

nvngx.dll is included too, this isn't the official dll, it's a custom re-implmentation, the source is included in the project under the "ngx_runtime" directory

AMD users will also have to install the [official HIP SDK for Windows](https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html)


The "Network" one is the nvngx_dlssnr.dll which is the library that actually contains the DLSS5 code. This one is the official Nvidia library, and it's not included in this project. you have to get it from a game which uses it (for example NBA 2K27), or get it from one of the countless community projects that are using it, like the RenoDX plugin for Reshade.

## Note on cache compilation
On the first launch, the program will have to translate the cuda modules to something that the AMD code can run natively, this will stored in AppData/Local/zluda/ComputeCache.
It will take A LOT of time, but it's only needed once.

## For Nvidia users
I also made a "nvidia mode", which tries to run the dlss using the official drivers. you still need to provide the nvngx_dlssnr.dll library.

NOTE: This feature isn't tested yet, since i don't have an nvidia gpu to test with at the moment.

## Video mode (video_filter)

`video_filter` is a console tool that runs the same DLSS path over a video,
frame by frame, with ffmpeg doing the decoding and encoding. It builds without
Qt, so a fork with no Qt installed can still use it.

```
video_filter input.mp4 output.mp4 <nvngx_dlssnr.dll> <ZLUDA nvcuda.dll> [nvngx.dll] [nvapi64.dll] [options]
```

Requires: ffmpeg on PATH (or `FFMPEG_PATH`), the network DLL, and the ZLUDA
DLLs from the release zip next to the program. Audio is passed through from
the source by default.

The network keeps an accumulation history between frames; by default it is
reset on the first frame and on detected scene cuts (`--reset auto`, cut
threshold 0.30). Without depth or motion vectors (absent for a plain video),
moving content may swim — `--reset always` trades quality for stability,
`--reset never` keeps one session for the whole clip.

Two settings decide whether the result flickers, and both default to the
stable end: `--passes 1` (evaluating a frame more than once stacks the effect
and amplifies the differences between neighbouring frames). With `--flow 1`,
the tool prefers a D3D12 compute motion guide: quarter-resolution block
matching runs on the GPU, then the small vector field is filtered/upscaled
for DLSS. If D3D12 setup fails, it automatically falls back to the CPU
estimator so conversion still completes. This is a compute-based guide, not
the proprietary NVIDIA Optical Flow SDK. `--flow 0` remains the most stable
choice when a clip has little motion. Raising either
buys a stronger effect at the cost of temporal stability.

Options: `--passes N`, `--reset auto|always|never|every=N`, `--cut-threshold F`,
`--flow 0|1`, `--intensity F`, `--global-tone F`, `--local-tone F`,
`--local-structure F`,
`--skin-structure F`, `--style N`, `--preset N`, `--no-auto-mask`,
`--crf N`, `--fps N`, `--no-audio`, `--max-frames N`, `--dump-frames DIR`.

Internal modes used by the parallel first-run translation:
`--compile-one <module> <driver>` and `--precompile <snippet> <driver> [jobs]`.
The first run of any mode translates the network's code and can take tens of
minutes; later runs reuse the cache.

# Known Issues
The program can sometimes fail to generate the picture, and you'll get a blank picture in output. if it does that, try loading a different picture or re-open the program. i'm currently trying to figure out what causes this.

# Frequently Asked Questions (FAQ)

### Why there isn't a pull request to the official ZLUDA project?
It's because most of the code is AI-generated and i'm not sure at all if all the code actually makes sense of if there's some garbage which shouldn't be there. 
The performance are still painfully bad
I'm not making a pull request containing code that i can't fully understand. But it's still available to everyone, hoping that people more skilled than me can help me and the whole community to achieve a proper way to handle this.

### Why didn't you make something to use this on games?
I actually built an experimental plugin for Reshade, but the performances are so bad that isn't really usable at the moment. I'll probably publish it if i can make some improvement.

### Will this work on Linux?
Not at the moment, but i'll probably try to put some effort to it if i get playable performance.

The problem is that while Zluda itself can work on Linux, it does trough Linux .so libraries, while the dlss5 is designed to run on windows only. and i'm not aware on way to run the windows version of Zluda and ROCm on Proton.

It should be possible, in theory, to make Proton/Wine to bridge nvcuda.dll to libcuda.so, but i didn't tried to that, yet.

### Do you know DLSS-NR-on-AMD by danielblnc?
Yes, i'm aware of that project, but that's totally unrelated to mine.
His approach is by far better performing right now, but since there's no code available, i really can't tell how the two project differ.
