# DLSS 5 Image & Video Enhancer for AMD / ZLUDA

> 在 AMD GPU（RDNA 3 / RDNA 4）上通过深度优化版 ZLUDA 满血运行 NVIDIA DLSS 5 Neural Rendering (DLSS-NR) 的图片增强与超清视频降噪工具。
> High-performance DLSS 5 Neural Rendering image enhancement and video denoising pipeline running on AMD GPUs via ZLUDA.

![Platform](https://img.shields.io/badge/platform-Windows_10%2F11-0078D4)
![GPU](https://img.shields.io/badge/GPU-AMD%20Radeon%20(RDNA3%20%2F%20RDNA4)-red)
![Acceleration](https://img.shields.io/badge/backend-ZLUDA%20%2B%20HIP%20%2B%20D3D12-orange)
![Release](https://img.shields.io/badge/release-v2026.09.10--multipass-brightgreen)

---

## 目录 / Table of Contents
- [中文文档](#中文文档)
  - [核心更新特性 (2026-09-10)](#核心更新特性-2026-09-10)
  - [架构与性能概览](#架构与性能概览)
  - [快速开始](#快速开始)
  - [视频命令行工具 (video_filter)](#视频命令行工具-video_filter)
  - [系统调优与排障工具](#系统调优与排障工具)
  - [常见问题与解答 (FAQ)](#常见问题与解答-faq)
- [English Documentation](#english-documentation)
  - [Key Features](#key-features)
  - [Quick Start](#quick-start)
  - [CLI Reference](#cli-reference)
  - [Troubleshooting Tools](#troubleshooting-tools)

---

# 中文文档

## 项目简介
本项目将 NVIDIA DLSS 5 Neural Rendering（DLSS-NR / DLSS 5 降噪与细节重构网络）完整接入 Windows 图形与视频处理链路。通过深度重构优化的 **ZLUDA** 与 **D3D12 共享显存管道**，使 AMD Radeon 显卡（RX 7000 系列 RDNA 3、RX 9000 系列 RDNA 4 如 RX 9070 XT 等）能够满血执行原生的 AI 图像超清与视频降噪增强。

- **GitHub 主仓库**: [big2cater/dlss5-visual-enhancer-zluda](https://github.com/big2cater/dlss5-visual-enhancer-zluda)
- **最新发布包**: [GitHub Releases](https://github.com/big2cater/dlss5-visual-enhancer-zluda/releases)
- **优化版 ZLUDA 源码**: [big2cater/ZLUDA](https://github.com/big2cater/ZLUDA)

---

## 核心更新特性 (2026-09-10)

### 1. 物理级 IEC 61966-2-1 双程色彩空间转换（彻底根治“蒙灰雾 / 变黑”缺陷）
- **根因修复**：彻底消除旧版本“输入将 sRGB 误当线性光强导致网络下压变黑、输出缺少逆转换导致暗部泛灰”的缺陷；
- **全链路物理直通**：输入端严格执行标准 sRGB $\to$ 物理线性辐射度（Linear Radiance）解算，网络在真实的物理量级上运行 HDR 局部色调映射与双边保边滤波，黑场纯净沉底，暗部层次分明；输出端精确映射回标准 sRGB，高光无死黑；默认伽马回归物理直通标准值 **`1.0`**（原图 1:1 物理无失真还原）。

### 2. 双引擎独立时序级联多轮降噪（Cascaded Dual-Engine Multipass）
- **架构革新**：借鉴 OptiScaler-DLSSNR-PreSR 架构思想，废除了在单一 Feature 上多次累加时钟历史的旧做法；
- **乒乓时序解耦**：构建 `Feature 0 (粗去噪) -> Intermediate -> Feature 1 (残差与微细节精修) -> Result` 级联管线。两个引擎每帧严格推进一次时序历史，在多轮高强度去噪的同时，**100% 消除画面频闪与动态拖影**！

### 3. RDNA 4（RX 9070 XT / gfx12）深度硬件级优化
- **硬件矩阵核全面释放（Hardware WMMA）**：ZLUDA 适配放宽至 `< 13000`，使 RDNA 4 调用第三代独立 Matrix Core 硬件单元（1024 flops/clk/CU），消除原本上千条 MMA 算子退化为标量模拟导致的编译卡死与运行慢速；
- **100% WGP 满血模式**：LLVM TargetMachine 与函数级属性全面强制生效 `-cumode`，以双 CU 聚合的 WGP 粒度共享高速 LDS 与 L1 缓存；
- **零堆分配（Zero-Allocation Launch）**：`cuLaunchKernel` 核心分发路径重构为纯栈上内存原位翻译，消除微秒级内存分配延迟；
- **占用率调优**：修复 `waves-per-eu` 映射边界，并将 `.maxnreg` 物理约束映射至 AMD LLVM `amdgpu-num-vgpr`，遏制寄存器溢出。

### 4. 视频管道 D3D12 映射显存零拷贝（Zero-Copy Video Pipeline）
- **管道流显存直写**：FFmpeg 解码流通过 `D3D12_HEAP_TYPE_UPLOAD` 映射内存直接灌入 GPU VRAM/GTT 显存，彻底消灭 CPU 堆内存分配与数据拷贝；
- **GPU Compute 着色器硬件解包**：编写专用 HLSL 计算着色器 `kRawComputeSource`，直接在 GPU 向量算力上完成 16-bit RGB48 解包与 IEC 61966-2-1 物理转换（耗时 $< 0.05$ ms），大幅降低 CPU 负载并提升吞吐（实测 720p 稳定态仅 **146 ms/帧**）。

### 5. 全链路防驱动崩溃与防 TDR 超时让出机制
- **轮次间/帧间自适应让出**：在多轮 Pass 间与视频帧间主动引入毫秒级调度让出点（`yield_ms`，支持命令行 `--yield-ms`），给 Windows 桌面窗口管理器（DWM）保留调度心跳，彻底终结大分辨率/多轮次下的 Windows 驱动超时重置（`LiveKernelEvent 0x141`）；
- **D3D12 栅栏 10 秒超时防御**：将无限等待重构为安全超时检测与 `GetDeviceRemovedReason` 捕获，杜绝死锁。

### 6. 皮肤微结构保护（Skin Structure）与单帧定帧对比
- **防塑料感/防蜡像感**：完整开放 DLSS-NR 底层 `SkinStructureStrength`（0.00 ~ 1.00），在人脸/特写中智能保留真实皮肤毛孔微结构；
- **极速定帧对比视窗**：GUI 提供【单帧定帧对比】功能，秒级渲染单帧并弹出可拖动中轴线的分屏画质对比器（CompareDialog）。

---

## 架构与性能概览

```
[FFmpeg 视频流 / 本地图片]
            │
            │ (D3D12 Upload Heap Mapped 显存直写 / 零拷贝)
            ▼
[D3D12 GPU Compute Shader: kRawComputeSource]
     ├── RGB48 硬件对齐解包
     └── IEC 61966-2-1 物理辐射度 (Linear Radiance) 转换
            │
            ▼
[ZLUDA / CUDA 共享纹理 (RGBA16_FLOAT)] ───► [独立 Backbuffer (切断自反馈)]
            │
            ▼
[NVIDIA DLSS-NR AI 推理网络 (crazy-cuckoo 核心)]
     ├── RDNA 3 / RDNA 4 硬件 WMMA 矩阵张量单元加速
     └── 100% 满血 WGP 缓存架构
            │
            ▼
[IEC 61966-2-1 逆向 sRGB 输出 / 显存直读回传]
            │
            ▼
[AMD AMF 原生硬件视频编码器 (H.264 / HEVC) / 图像写盘]
```

---

## 快速开始

### 1. 环境需求
- **操作系统**：Windows 10 / 11（x64）
- **显卡硬件**：AMD Radeon RX 6000 / RX 7000 / RX 9000 系列 GPU（推荐 RX 7900 XT / RX 9070 XT 等）
- **驱动支持**：AMD Software: Adrenalin Edition 24.x 或 25.x 官方驱动（推荐带有 `amdhip64_7.dll` 的最新驱动）
- **FFmpeg 支持**：视频处理需要 `ffmpeg.exe` 与 `ffprobe.exe`，请将其所在目录添加至系统环境变量 `PATH`。

### 2. 使用方法
1. 从 [Releases 页面](https://github.com/big2cater/dlss5-visual-enhancer-zluda/releases) 下载最新的发布包（例如 `DLSSNRFilter-v2026.09.11-multipass-nodlssnr.zip`）；
2. 解压整个文件夹（**请解压至全英文路径**，不要单独拷贝某个 DLL）；
3. **放置专有模型文件**：将合法的 `nvngx_dlssnr.dll`（推荐 Build 310.8.0）放入解压后的根目录（与 `video_filter.exe` / `dlssnr_gui.exe` 同级目录）；
4. 双击运行 `dlssnr_gui.exe`（Qt 6 统一现代图形界面；原 WinForms 界面已正式退役）；
5. **首次使用请先预热**：点击 **Warm up cache** 按钮预热翻译缓存（进度实时显示在日志窗，约 20~40 分钟，仅此一次），状态栏显示 Cache ready 后再点 **Enhance** 即可秒级出图！

> ⚠️ **关于首次运行**：首次使用（以及每次更换程序版本或显卡之后）需要把网络编译为本机缓存，全程约 20~40 分钟。强烈建议先点 **Warm up cache** 预热——进度可见，不会像卡死；编译结果永久保存在本机（绑定显卡），之后每次启动秒级拉起。直接点 Enhance 跳过预热也可以，程序会主动询问是否先预热。

---

## 视频命令行工具 (video_filter)

```text
video_filter.exe <输入视频> <输出视频> <nvngx_dlssnr.dll路径> <nvcuda.dll路径> [nvngx.dll] [nvapi64.dll] [选项]
```

### 核心参数表

| 参数 | 默认值 | 作用说明 |
| :--- | :--- | :--- |
| `--parallel auto\|2\|off` | `auto` | **时序切片多进程并发加速**：`auto` (智能硬件守护，自动检测显存与CPU安全分配)、`2` (强制双进程并发切片，提速近2倍)、`off` (单进程) |
| `--passes N` | `1` | 每帧降噪轮次。视频建议 `1` 或 `2`（设为 2 时自动激活双引擎级联乒乓时序管线） |
| `--yield-ms N` | `1` | 帧间调度让出时间（毫秒）。有效防止重负载下触发 Windows 驱动超时重置（TDR） |
| `--gamma F` | `1.0` | 输出伽马调节。默认 `1.0`（纯物理标准 sRGB 直通映射，绝不偏色发灰） |
| `--intensity F` | `1.0` | DLSS-NR 降噪总强度 (0.00 ~ 1.00) |
| `--skin-structure F` | `0.0` | 皮肤微结构保留强度 (0.00 ~ 1.00)，人脸场景推荐开启防止蜡像感 |
| `--encoder P` | `auto` | 视频编码器：`auto`、`amf` (AMD 硬件编码 H.264)、`hevc_amf`、`x264` |
| `--crf N` | `18` | 视频编码质量 / CQP 恒定质量基准 |
| `--flow 0\|1` | `0` | 运动矢量光流引导：`0` 为静态稳定融合，`1` 为开启动态光流重投影 |
| `--reset auto` | `auto` | 时序历史累积复位策略：`auto` (自动切镜检测)、`always`、`never`、`every=N` |
| `--cut-threshold F`| `0.30` | 自动转场切镜检测灵敏度 |
| `--max-frames N` | `0` | 限制处理帧数（0 为全片处理，调试时可填 30/60 测试效果） |
| `--no-audio` | 关 | 不拷贝原视频音轨 |

---

## 系统调优与排障工具

发布包中附带了两个专为解决 AMD 显卡与 Windows 驱动异常设计的实用脚本：

1. **`diagnose_gpu.bat` (GPU 与 HIP 环境一键诊断工具)**：
   - 快速扫描检测本机的 AMD 显卡型号、驱动版本；
   - 测试 `System32` 与 `HIP_PATH` 中的 `amdhip64_7.dll` / `amdhip64_6.dll` 调用情况；
   - 验证 ZLUDA 与 RDNA 4 (`gfx1200` / `gfx1201`) 环境兼容性，精准排查 `cuInit failed: 100` 报错。
2. **`fix_tdr.bat` (Windows 显卡驱动防掉卡超时修复工具)**：
   - Windows 默认的显卡超时阈值仅为 2 秒，重度 AI 渲染或大分辨率运算极易误判为“显卡死机”而强行重启驱动（错误 `0x141`）；
   - 双击运行该脚本可一键安全提权，将 Windows `TdrDelay` 与 `TdrDdiDelay` 优化至深度学习行业标准的 **10 秒**，彻底告别掉驱动。

---

## 常见问题与解答 (FAQ)

**Q1：为什么之前处理出来的视频/图片感觉蒙了一层灰白雾，或者暗部发黑？**  
A：这是早期版本缺失 sRGB $\leftrightarrow$ Linear 物理双程转换导致的。在 `v2026.09.10-multipass` 版本中已通过 IEC 61966-2-1 标准转换彻底根治，黑场纯黑沉底，默认伽马回归 1.0，色彩纯正通透。

**Q2：我的显卡是 RX 9070 XT（RDNA 4），为什么提示 `cuInit failed: 100`？**  
A：这通常是因为安装了旧版的 HIP SDK（如 HIP 6.1/6.2 早期安装包），其安装目录的旧 DLL 覆盖了系统的环境变量。实际上 AMD Adrenalin 25.x 显卡驱动自带的 `C:\Windows\System32\amdhip64_7.dll` 已经具备 RDNA 4 原生支持。请运行包内的 `diagnose_gpu.bat`，工具会自动识别并输出具体排查方案。

**Q3：多轮降噪（Multipass）会造成视频闪烁吗？**  
A：旧版如果简单重复处理会扰乱网络时序导致频闪，但在最新版本中，我们重构为了**双引擎乒乓级联时序架构**，每帧每个引擎严格仅推进一步历史，实测多轮降噪画面极致纯净且完全无频闪。

---

# English Documentation

## Overview
**DLSS 5 Image & Video Enhancer** is a high-performance framework designed to run NVIDIA DLSS 5 Neural Rendering (DLSS-NR) natively on AMD Radeon graphics cards (RDNA 3 and RDNA 4 architectures, including RX 7900 XT, RX 9070 XT, etc.) on Windows through a heavily optimized fork of **ZLUDA** and Direct3D 12 shared texture memory.

- **Primary Repository**: [big2cater/dlss5-visual-enhancer-zluda](https://github.com/big2cater/dlss5-visual-enhancer-zluda)
- **Latest Release**: [GitHub Releases](https://github.com/big2cater/dlss5-visual-enhancer-zluda/releases)
- **Optimized ZLUDA Runtime**: [big2cater/ZLUDA](https://github.com/big2cater/ZLUDA)

---

## Key Features

### 1. Physical IEC 61966-2-1 Color Space Pipeline (Zero Fog / Zero Crushed Blacks)
- Fully implements two-way standard sRGB $\leftrightarrow$ Linear radiance transformation.
- Color inputs are correctly transformed into physical radiance values, allowing DLSS-NR's tone mapper and bilateral edge-preserving filter to operate at physical scale. True blacks remain deep and unwashed; default gamma is set to **1.0** (1:1 physical passthrough).

### 2. Cascaded Dual-Engine Multipass Pipeline
- Eliminates temporal flicker in multi-pass video denoising.
- Operates two independent temporal sessions in cascade (`Feature 0 -> intermediate -> Feature 1 -> output`). Each engine advances temporal state exactly once per frame, yielding maximum grain reduction with 100% temporal consistency.

### 3. Native RDNA 4 (RX 9070 XT / gfx12) Hardware Acceleration
- **Hardware Matrix Cores (WMMA)**: Expands ISA dispatch to `< 13000`, unlocking RDNA 4's 3rd-generation Matrix Cores (1024 flops/clk/CU) and replacing scalar software emulation.
- **100% WGP Mode**: Enforces dual-CU Workgroup Processor mode across TargetMachine and function-level codegen.
- **Zero-Allocation Hot Path**: Eliminates dynamic heap vector allocations in `cuLaunchKernel`.

### 4. Direct D3D12 Mapped Upload Zero-Copy Pipeline
- FFmpeg video streams write directly into GPU mapped upload memory (`D3D12_HEAP_TYPE_UPLOAD`), completely bypassing CPU host memory allocations and multithreaded LUT overhead.
- Dedicated HLSL compute shader unpacks RGB48 and converts to linear float directly on GPU vector ALUs in $<0.05$ ms.

### 5. Full-Pipeline Anti-TDR & Driver Recovery Safeguards
- Inter-pass and inter-frame adaptive yielding (`--yield-ms`) grants Windows DWM scheduler breathing room, eliminating display driver watchdog resets (`0x141`).
- D3D12 fence timeout detection gracefully handles device state changes without system lockups.

---

## Quick Start

1. Download the release archive from [Releases](https://github.com/big2cater/dlss5-visual-enhancer-zluda/releases).
2. Extract the archive into a directory with a standard English path.
3. Place your legally acquired `nvngx_dlssnr.dll` (Build 310.8.0 recommended) in the same directory as `dlssnr_gui.exe` and `video_filter.exe`.
4. Ensure `ffmpeg.exe` is available on your system `PATH`.
5. Launch `dlssnr_gui.exe` (Qt 6 modern unified GUI; legacy WinForms has been retired).
6. **Warm up first**: click **Warm up cache** before the first Enhance -- it
   translates the network into the local cache (about 20-40 minutes, once),
   with the module progress visible in the log. Every later start is fast.

> ⚠️ **About the first run**: the first use (and every version or graphics
> card change) has to compile the network for this machine, which takes a
> while. The compiled cache is bound to the GPU it was built for and persists
> locally; skipping the warm-up still works -- the program offers it before a
> cold first run.

---

## CLI Reference

```text
video_filter.exe <input> <output> <nvngx_dlssnr.dll> <nvcuda.dll> [nvngx.dll] [nvapi64.dll] [options]
```

- `--parallel auto|2|off`: Temporal chunk-based multi-process parallel acceleration (`auto`: intelligent hardware budget guard for VRAM & CPU safety; `2`: 2 workers; `off`: 1 worker).
- `--passes N`: Number of denoising passes per frame (1 or 2).
- `--yield-ms N`: Milliseconds to yield execution between frames to prevent Windows TDR (default 1).
- `--gamma F`: Output gamma adjustment (1.0 default = standard physical sRGB).
- `--skin-structure F`: Skin structure preservation strength (0.00 to 1.00).
- `--encoder P`: Encoder backend (`auto`, `amf`, `hevc_amf`, `x264`).
- `--crf N`: Quality/CQP target.

---

## Troubleshooting Tools

- **`diagnose_gpu.bat`**: Scans AMD GPU drivers, HIP SDK configurations, and verifies ZLUDA/RDNA 4 compatibility to diagnose `cuInit failed: 100`.
- **`fix_tdr.bat`**: One-click administrator utility to configure Windows `TdrDelay` to 10 seconds for stable deep-learning/heavy AI execution.

---

## License & Notice
This project is an open-source research and compatibility tool. It does **not** include or distribute any proprietary NVIDIA DLLs or weights (`nvngx_dlssnr.dll`). All trademarks belong to their respective owners.
