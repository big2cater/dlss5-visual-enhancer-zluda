# DLSS 5 Image Enhancer for AMD / ZLUDA

> 在 AMD GPU 上运行 DLSS 5 Neural Rendering 的实验性图片与视频工具。

![Platform](https://img.shields.io/badge/platform-Windows-0078D4)
![GPU](https://img.shields.io/badge/GPU-AMD%20%2B%20ZLUDA-red)
![GUI](https://img.shields.io/badge/GUI-Qt%206-41CD52)

## 中文

### 项目简介

本项目将 NVIDIA DLSS 5 Neural Rendering（DLSS-NR）接入 Windows 图像处理流程，目标是在 AMD GPU 上通过 ZLUDA 运行图片增强和视频逐帧处理。

项目目前属于实验性软件，重点是验证 DLSS-NR、ZLUDA、ROCm/HIP 与 AMD GPU 的兼容性。性能、稳定性和不同网络 DLL 的兼容性可能会有明显差异。

相关项目：

- [ZLUDA fork](https://github.com/RedDukeDev/ZLUDA)
- [LLVM fork](https://github.com/RedDukeDev/llvm-project)
- [项目 Release](https://github.com/big2cater/dlss5-image-enhancer-zluda/releases)

### 当前功能

- 图片模式：PNG 等图片的 DLSS-NR 增强
- 视频模式：通过 FFmpeg 解码、逐帧处理并重新编码
- Qt 6 图形界面与命令行 `video_filter.exe`
- GPU 运动向量引导，并在不可用时回退到 CPU
- 场景切换复位、音轨直通、帧数限制、抽帧调试
- 处理日志、进度显示、原图与结果对比
- AMD/ZLUDA 模式，以及实验性的 NVIDIA 模式代码路径

### 快速开始

1. 从 [Release 页面](https://github.com/big2cater/dlss5-image-enhancer-zluda/releases) 下载压缩包。
2. 将整个目录解压，不要只复制某一个 DLL。
3. 安装 AMD [HIP SDK](https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html)。
4. 确认 `ffmpeg.exe` 和 `ffprobe.exe` 在系统 `PATH` 中（视频模式需要）。
5. 启动 `dlssnr_gui.exe`。
6. 在界面中确认以下文件路径指向当前目录中的 DLL：`nvngx_dlssnr.dll`、ZLUDA 的 `nvcuda.dll`、项目提供的 `nvngx.dll` 和 ZLUDA 的 `nvapi64.dll`。

### 关于 DLL

`nvngx_dlssnr.dll` 是实际包含 DLSS-NR 网络的 NVIDIA DLL，本项目和 Release 压缩包均不提供该文件。

当前最终 low-latency 版本是在 nvngx_dlssnr.dll Build 310.8.0 上测试的。建议使用同一 Build；其他版本可能出现模型加载失败、空白输出、0xBAD00005 或不同的时域表现，项目不保证兼容。请仅从合法来源获取，并自行确认其许可和使用条件。

发布包中的 `nvcuda.dll` 与 `nvapi64.dll` 是 ZLUDA 组件，不是 NVIDIA 官方 CUDA 驱动文件。`nvngx.dll` 是本项目的 NGX 运行时实现，源代码位于 `ngx_runtime/`。

### 首次运行与缓存

AMD/ZLUDA 模式首次运行需要翻译 CUDA 模块。第一次启动可能耗时较长，翻译结果会写入 ZLUDA ComputeCache，后续运行通常会复用缓存。

请不要把个人视频、日志、缓存目录或包含绝对路径的配置文件上传到仓库或分享给他人。

### 视频命令行

```text
video_filter.exe input.mp4 output.mp4 nvngx_dlssnr.dll nvcuda.dll nvngx.dll nvapi64.dll [options]
```

常用选项：

```text
--passes N                 每帧处理次数，视频建议保持 1
--reset auto|always|never|every=N
--cut-threshold F         场景切换阈值，默认 0.30
--flow 0|1                 运动向量引导，默认关闭
--intensity F --global-tone F --local-tone F
--local-structure F --skin-structure F
--style N --preset N --no-auto-mask
--crf N --fps N --max-frames N --no-audio
--dump-frames DIR
```

当前最终 low-latency 发布版的视频后端固定使用 native 输出和 DLL 默认模型预设。实验性的 Upscaling 与独立 Model Preset 参数暂未在最终稳定包中启用。

### 视频闪烁建议

- `--passes 1`：视频不要重复处理同一帧。
- `--flow 0`：通常更稳定；快速运动场景可尝试 `--flow 1`。
- 使用 `--reset auto`，不要在普通运动中频繁复位。
- 先用默认强度测试，再逐步提高局部色调、结构和强度。

### 已知限制

- 这是实验性项目，不保证所有 AMD GPU、驱动、HIP SDK 或网络 DLL 都兼容。
- ZLUDA 翻译、网络 DLL、运动向量误差和时域累积都可能造成闪烁、拖影或延迟。
- 某些 DLL/驱动组合可能出现空白输出；遇到此情况请保存日志并重新启动测试。
- 性能通常明显低于 NVIDIA 原生路径。
- Linux、macOS 和游戏内实时注入目前不属于稳定支持范围。

### FAQ

**为什么不直接向官方 ZLUDA 提交？**

本项目包含大量实验性代码，作者无法保证每一部分都适合合并到上游。欢迎提交 Issue、测试结果和改进建议，但请先说明 GPU、驱动、HIP SDK、网络 DLL 和日志版本。

**能用于游戏吗？**

目前主要面向图片和离线视频。实时游戏注入的性能和稳定性还不足以作为可靠方案。

**为什么没有 Linux 版本？**

当前 DLSS-NR 和项目运行链路主要面向 Windows DLL、D3D12 和 Windows 版 ZLUDA。Linux/Proton 需要额外的 DLL 与 `.so` 桥接工作，尚未完成。

## English

### Overview

DLSS 5 Image Enhancer is an experimental Windows image and video tool that attempts to run NVIDIA DLSS 5 Neural Rendering on AMD GPUs through ZLUDA.

It is primarily a compatibility and research project. Performance, temporal stability, and support for different DLSS-NR DLL builds can vary significantly.

Related projects:

- [ZLUDA fork](https://github.com/RedDukeDev/ZLUDA)
- [LLVM fork](https://github.com/RedDukeDev/llvm-project)
- [Releases](https://github.com/big2cater/dlss5-image-enhancer-zluda/releases)

### Features

- Still-image DLSS-NR enhancement
- Frame-by-frame video processing through FFmpeg
- Qt 6 GUI and the `video_filter.exe` command-line tool
- GPU motion-vector guidance with CPU fallback
- Scene-cut resets, audio passthrough, frame limits, and frame dumping
- Processing logs, progress reporting, and before/after comparison
- AMD/ZLUDA mode plus an experimental NVIDIA path

### Quick Start

1. Download a package from the [Releases page](https://github.com/big2cater/dlss5-image-enhancer-zluda/releases).
2. Extract the complete directory; do not copy individual DLLs by themselves.
3. Install the AMD [HIP SDK](https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html).
4. Put `ffmpeg.exe` and `ffprobe.exe` on `PATH` for video processing.
5. Launch `dlssnr_gui.exe`.
6. Verify that the GUI points to the bundled `nvngx_dlssnr.dll`, ZLUDA `nvcuda.dll`, project `nvngx.dll`, and ZLUDA `nvapi64.dll`.

### DLL and licensing notes

`nvngx_dlssnr.dll` contains the actual DLSS-NR network. It is not included in this repository or in the Release archives.

The final low-latency build was tested with nvngx_dlssnr.dll Build 310.8.0. Using the same build is recommended; other versions may fail to load the network, produce blank output, return 0xBAD00005, or show different temporal behavior. Obtain the DLL only from a lawful source and follow the applicable license terms.

The bundled `nvcuda.dll` and `nvapi64.dll` are ZLUDA components, not NVIDIA's official CUDA driver files. `nvngx.dll` is this project's NGX runtime implementation; its source is under `ngx_runtime/`.

### First Run and Cache

AMD/ZLUDA mode translates CUDA modules on the first run. This can take a long time. The translated modules are cached by ZLUDA and later runs normally reuse that cache.

Do not share personal media, logs, cache directories, or configuration files containing absolute local paths.

### Video CLI

```text
video_filter.exe input.mp4 output.mp4 nvngx_dlssnr.dll nvcuda.dll nvngx.dll nvapi64.dll [options]
```

The final low-latency release uses native output and the DLL's default model preset. Experimental Upscaling and independent Model Preset controls are intentionally disabled in the stable package.

### Temporal Stability Tips

- Keep `--passes 1` for video.
- Keep `--flow 0` for maximum stability; try `--flow 1` for fast motion.
- Use `--reset auto` and avoid resetting on ordinary movement.
- Start with the default strengths and increase them gradually.

### Limitations

- Compatibility is not guaranteed across AMD GPUs, drivers, HIP SDK versions, or DLSS-NR DLL builds.
- ZLUDA translation, network behavior, motion-vector errors, and temporal accumulation can cause flicker, ghosting, or latency.
- Some DLL/driver combinations may produce blank output; keep the logs and retry from a fresh process.
- Performance is generally much lower than the native NVIDIA path.
- Linux, macOS, and reliable real-time game injection are not currently supported targets.

## Credits

This project builds on the work of the DLSS, ZLUDA, ROCm/HIP, Qt, and FFmpeg communities. Please respect the licenses of all bundled and externally supplied components.
