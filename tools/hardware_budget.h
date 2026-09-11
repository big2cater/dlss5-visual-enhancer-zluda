#pragma once

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <thread>
#include <string>
#include <algorithm>
#include <cstdio>

namespace dlssnr_budget {

struct HardwareInfo {
    size_t total_vram_mb = 0;
    size_t avail_vram_mb = 0;
    size_t total_ram_mb = 0;
    size_t avail_ram_mb = 0;
    unsigned int cpu_threads = 0;
    int recommended_concurrency = 1;
    int ffmpeg_threads_per_worker = 2;
    std::string summary;
};

inline HardwareInfo detect_hardware_budget(double video_duration_sec = 0.0, int width = 0, int height = 0) {
    HardwareInfo info{};

    // 1. CPU cores
    info.cpu_threads = std::thread::hardware_concurrency();
    if (info.cpu_threads == 0) info.cpu_threads = 4;

    // 2. System RAM
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        info.total_ram_mb = (size_t)(mem.ullTotalPhys / (1024 * 1024));
        info.avail_ram_mb = (size_t)(mem.ullAvailPhys / (1024 * 1024));
    }

    // 3. GPU VRAM via DXGI
    IDXGIFactory1 *factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) && factory) {
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                // Skip Microsoft Basic Render Driver
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                    adapter->Release();
                    continue;
                }
                size_t vram_mb = (size_t)(desc.DedicatedVideoMemory / (1024 * 1024));
                if (vram_mb > info.total_vram_mb) {
                    info.total_vram_mb = vram_mb;
                    info.avail_vram_mb = vram_mb; // baseline fallback

                    // Check for DXGI 1.4 memory info
                    IDXGIAdapter3 *adapter3 = nullptr;
                    if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void **)&adapter3)) && adapter3) {
                        DXGI_QUERY_VIDEO_MEMORY_INFO qinfo{};
                        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &qinfo))) {
                            if (qinfo.Budget > qinfo.CurrentUsage) {
                                info.avail_vram_mb = (size_t)((qinfo.Budget - qinfo.CurrentUsage) / (1024 * 1024));
                            }
                        }
                        adapter3->Release();
                    }
                }
            }
            adapter->Release();
        }
        factory->Release();
    }

    // 4. Calculate safe concurrency
    int safe = 1;
    char buf[512];

    // Gate 1: Video duration check
    if (video_duration_sec > 0.0 && video_duration_sec < 15.0) {
        snprintf(buf, sizeof(buf),
                 "[硬件评估] 视频时长较短 (%.1fs < 15s)，多进程切片开销大于收益 -> 使用单进程标准模式",
                 video_duration_sec);
        info.recommended_concurrency = 1;
        info.ffmpeg_threads_per_worker = 0; // default/auto
        info.summary = buf;
        return info;
    }

    // Gate 2: VRAM check (<8GB is strictly single worker)
    if (info.total_vram_mb < 7500) {
        snprintf(buf, sizeof(buf),
                 "[硬件评估] 显存不足 8GB (当前检测到 %zu MB) -> 为防显存溢出锁定单进程模式",
                 info.total_vram_mb);
        safe = 1;
    }
    // Gate 3: System RAM check (< 12GB total or < 3.5GB avail is single worker)
    else if (info.total_ram_mb < 12000 || info.avail_ram_mb < 3500) {
        snprintf(buf, sizeof(buf),
                 "[硬件评估] 系统可用内存紧缺 (可用 %zu MB / 总量 %zu MB) -> 锁定单进程保护系统平稳",
                 info.avail_ram_mb, info.total_ram_mb);
        safe = 1;
    }
    // Gate 4: CPU cores check (<= 4 cores is single worker to avoid desktop freeze)
    else if (info.cpu_threads <= 4) {
        snprintf(buf, sizeof(buf),
                 "[硬件评估] CPU 逻辑核心较少 (%u 线程) -> 保持单进程避免占用过高",
                 info.cpu_threads);
        safe = 1;
    }
    // Gate 5: 4K video resolution check
    else if (width * height >= 3840 * 2160 && info.avail_vram_mb < 12000) {
        snprintf(buf, sizeof(buf),
                 "[硬件评估] 4K 超高分辨率且可用显存不足 12GB (可用 %zu MB) -> 安全降级为单进程模式",
                 info.avail_vram_mb);
        safe = 1;
    }
    // All checks passed! Hardware is capable of safe parallelization
    else {
        safe = 2; // Default optimal concurrency: 2 workers
        snprintf(buf, sizeof(buf),
                 "[硬件评估] 显存: %zu MB (充沛) | 可用内存: %zu MB | CPU: %u 线程 -> 自动启用 2 进程分片加速 (低优先级防死机)",
                 info.total_vram_mb, info.avail_ram_mb, info.cpu_threads);
    }

    info.recommended_concurrency = safe;
    if (safe > 1) {
        // Limit FFmpeg CPU threads per worker so 2 FFmpegs don't peg CPU to 100%
        info.ffmpeg_threads_per_worker = std::max(2, (int)(info.cpu_threads / 4));
        if (info.ffmpeg_threads_per_worker > 4) info.ffmpeg_threads_per_worker = 4;
    } else {
        info.ffmpeg_threads_per_worker = 0;
    }
    info.summary = buf;

    return info;
}

} // namespace dlssnr_budget
