#pragma once

#include <windows.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "dxgi.lib")

namespace dlssnr {

struct DetectedGpu {
    std::wstring name;
    UINT vendor_id = 0;
    UINT device_id = 0;
    size_t dedicated_vram_mb = 0;
    bool is_software = false;
    bool is_virtual = false;
};

inline bool is_virtual_adapter(const std::wstring &name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower.find(L"virtual") != std::wstring::npos) return true;
    if (lower.find(L"basic render") != std::wstring::npos) return true;
    if (lower.find(L"basic display") != std::wstring::npos) return true;
    if (lower.find(L"remote") != std::wstring::npos) return true;
    if (lower.find(L"todesk") != std::wstring::npos) return true;
    if (lower.find(L"gameviewer") != std::wstring::npos) return true;
    if (lower.find(L"iddsample") != std::wstring::npos) return true;
    if (lower.find(L"indirect") != std::wstring::npos) return true;
    return false;
}

inline std::vector<DetectedGpu> enumerate_gpus() {
    std::vector<DetectedGpu> gpus;
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) || !factory) {
        OutputDebugStringA("[GPU-AutoConfig] Warning: CreateDXGIFactory1 failed\n");
        return gpus;
    }

    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1 *adapter = nullptr;
        HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (FAILED(hr) || !adapter) break;

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            DetectedGpu gpu;
            gpu.name = desc.Description;
            gpu.vendor_id = desc.VendorId;
            gpu.device_id = desc.DeviceId;
            gpu.dedicated_vram_mb = (size_t)(desc.DedicatedVideoMemory / (1024 * 1024));
            gpu.is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            gpu.is_virtual = gpu.is_software || is_virtual_adapter(gpu.name);
            gpus.push_back(gpu);
        }
        adapter->Release();
    }
    factory->Release();
    return gpus;
}

// The GPU the network would run on, as a stable identity string for the
// precompile stamp. Mirrors the pick in auto_configure_gpu_environment above
// (largest non-virtual adapter, AMD preferred when an AMD discrete is
// present) -- keep the two in sync: a stamp naming a different GPU than the
// one this selection lands on must not answer warm, or a machine that
// swapped graphics cards would skip translating for its new chip.
inline std::wstring detected_gpu_identity() {
    auto gpus = enumerate_gpus();
    const DetectedGpu *best = nullptr;
    const DetectedGpu *best_amd = nullptr;
    for (const auto &gpu : gpus) {
        if (gpu.is_virtual) continue;
        if (!best || gpu.dedicated_vram_mb > best->dedicated_vram_mb) best = &gpu;
        if (gpu.vendor_id == 0x1002 &&
            (!best_amd || gpu.dedicated_vram_mb > best_amd->dedicated_vram_mb))
            best_amd = &gpu;
    }
    if (best_amd && (!best || best->vendor_id != 0x1002 ||
                     best_amd->dedicated_vram_mb > best->dedicated_vram_mb))
        best = best_amd;
    if (!best) {
        for (const auto &gpu : gpus)
            if (!gpu.is_software) { best = &gpu; break; }
    }
    if (!best) return {};
    wchar_t ids[64] = {};
    swprintf(ids, 64, L"|%04X|%04X|%zu", best->vendor_id, best->device_id,
             best->dedicated_vram_mb);
    return best->name + ids;
}

inline void auto_configure_gpu_environment() {
    // 1. Enumerate GPUs via DXGI
    auto gpus = enumerate_gpus();
    if (gpus.empty()) {
        OutputDebugStringA("[GPU-AutoConfig] Warning: CreateDXGIFactory1 failed\n");
        return;
    }

    // 2. Find primary discrete GPU (highest VRAM, non-virtual)
    const DetectedGpu *best_gpu = nullptr;
    for (const auto &gpu : gpus) {
        if (gpu.is_virtual) continue;
        if (!best_gpu || gpu.dedicated_vram_mb > best_gpu->dedicated_vram_mb) {
            best_gpu = &gpu;
        }
    }
    if (!best_gpu) {
        // Fallback: take any first non-software GPU
        for (const auto &gpu : gpus) {
            if (!gpu.is_software) { best_gpu = &gpu; break; }
        }
    }
    if (!best_gpu) return;

    // 3. Inspect if primary or any discrete GPU is AMD (0x1002), prioritizing largest AMD discrete VRAM
    const DetectedGpu *best_amd_gpu = nullptr;
    for (const auto &gpu : gpus) {
        if (!gpu.is_virtual && gpu.vendor_id == 0x1002) {
            if (!best_amd_gpu || gpu.dedicated_vram_mb > best_amd_gpu->dedicated_vram_mb) {
                best_amd_gpu = &gpu;
            }
        }
    }
    bool is_amd = (best_amd_gpu != nullptr) || (best_gpu->vendor_id == 0x1002);
    if (best_amd_gpu && (best_gpu->vendor_id != 0x1002 || best_amd_gpu->dedicated_vram_mb > best_gpu->dedicated_vram_mb)) {
        best_gpu = best_amd_gpu;
    }

    if (is_amd) {
        std::wstring name_lower = best_gpu->name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::towlower);

        // Check for RDNA 4 (RX 9000 series, 9070/9060/9080/9090, Navi 48/44, or device IDs 0x7480..0x74DF)
        bool is_rdna4 = (name_lower.find(L"9070") != std::wstring::npos ||
                         name_lower.find(L"9060") != std::wstring::npos ||
                         name_lower.find(L"9080") != std::wstring::npos ||
                         name_lower.find(L"9090") != std::wstring::npos ||
                         name_lower.find(L"rx 9") != std::wstring::npos ||
                         name_lower.find(L"radeon 9") != std::wstring::npos ||
                         (best_gpu->device_id >= 0x7480 && best_gpu->device_id <= 0x74DF));

        char msg[512] = {};
        if (is_rdna4) {
            char env_hsa[64] = {};
            DWORD len = GetEnvironmentVariableA("HSA_OVERRIDE_GFX_VERSION", env_hsa, sizeof(env_hsa));
            if (len == 0 || env_hsa[0] == '\0') {
                // Auto-inject HSA_OVERRIDE_GFX_VERSION=12.0.1 for RDNA 4
                SetEnvironmentVariableA("HSA_OVERRIDE_GFX_VERSION", "12.0.1");
                _putenv("HSA_OVERRIDE_GFX_VERSION=12.0.1");
                snprintf(msg, sizeof(msg),
                         "[GPU-AutoConfig] Detected %ls (RDNA 4, %zu MB Dedicated VRAM).\n"
                         "[GPU-AutoConfig] Self-Healing: Injected HSA_OVERRIDE_GFX_VERSION=12.0.1\n",
                         best_gpu->name.c_str(), best_gpu->dedicated_vram_mb);
            } else {
                snprintf(msg, sizeof(msg),
                         "[GPU-AutoConfig] Detected %ls (RDNA 4). Using existing HSA_OVERRIDE_GFX_VERSION=%s\n",
                         best_gpu->name.c_str(), env_hsa);
            }
            OutputDebugStringA(msg);
            fprintf(stderr, "%s", msg);

            char env_disp[64] = {};
            DWORD dlen = GetEnvironmentVariableA("AMD_DIRECT_DISPATCH", env_disp, sizeof(env_disp));
            if (dlen == 0 || env_disp[0] == '\0') {
                SetEnvironmentVariableA("AMD_DIRECT_DISPATCH", "0");
                _putenv("AMD_DIRECT_DISPATCH=0");
            }
        } else {
            snprintf(msg, sizeof(msg),
                     "[GPU-AutoConfig] Detected %ls (AMD Radeon, %zu MB Dedicated VRAM).\n",
                     best_gpu->name.c_str(), best_gpu->dedicated_vram_mb);
            OutputDebugStringA(msg);
            fprintf(stderr, "%s", msg);
        }

        // Ensure NVAPI reports sm_120 Blackwell arch for DLSS neural network
        char env_nvapi[64] = {};
        DWORD nlen = GetEnvironmentVariableA("ZLUDA_NVAPI_GPU_ARCH", env_nvapi, sizeof(env_nvapi));
        if (nlen == 0 || env_nvapi[0] == '\0') {
            SetEnvironmentVariableA("ZLUDA_NVAPI_GPU_ARCH", "0x1B0");
            _putenv("ZLUDA_NVAPI_GPU_ARCH=0x1B0");
        }
    } else {
        char msg[512] = {};
        snprintf(msg, sizeof(msg), "[GPU-AutoConfig] Detected %ls (%zu MB Dedicated VRAM).\n",
                 best_gpu->name.c_str(), best_gpu->dedicated_vram_mb);
        OutputDebugStringA(msg);
        fprintf(stderr, "%s", msg);
    }
}

} // namespace dlssnr
