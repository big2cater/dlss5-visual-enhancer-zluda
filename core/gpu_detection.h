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

// Where the auto-configuration messages go.
//
// They used to go to OutputDebugStringA and stderr, neither of which a GUI user
// can see. The line saying what was detected and what was injected is the first
// thing anyone needs when an RDNA 4 machine misbehaves, and it was invisible in
// exactly the case where it matters -- dlssnr_gui.exe. It is appended to a file in
// %TEMP% as well now, and diagnose_gpu.ps1 prints the tail of that file.
//
// Appended rather than rewritten: the same machine will have several runs, and
// the interesting question is usually whether the behaviour changed between them.
//
// Defined above its first caller rather than next to the function that fills in
// the message, because enumerate_gpus() below reports a warning of its own.
inline void report_auto_config(const char *msg) {
    if (!msg || !*msg) return;
    OutputDebugStringA(msg);
    fprintf(stderr, "%s", msg);
    char temp[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableA("TEMP", temp, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    std::string file = std::string(temp) + "\\dlssnr_gpu_autoconfig.log";
    FILE *f = nullptr;
    if (fopen_s(&f, file.c_str(), "a") != 0 || !f) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u pid %lu] %s",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            (unsigned long)GetCurrentProcessId(), msg);
    fclose(f);
}

inline std::vector<DetectedGpu> enumerate_gpus() {
    std::vector<DetectedGpu> gpus;
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) || !factory) {
        report_auto_config("[GPU-AutoConfig] Warning: CreateDXGIFactory1 failed\n");
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
    swprintf_s(ids, 64, L"|%04X|%04X|%zu", best->vendor_id, best->device_id,
               best->dedicated_vram_mb);
    return best->name + ids;
}

// Which AMD cards are RDNA 4 (gfx12) -- the ones that need the HSA override
// because ROCm's device list does not know them yet. Split out of the caller so
// that tools/test_rdna4_detect.cpp can drive it with real cards instead of
// relying on whatever machine happens to be plugged in.
//
// Names: the consumer line-up is RX 9070 XT / 9070 / 9070 GRE and RX 9060 XT /
// 9060, plus the workstation Radeon AI PRO R9700. There is no 9080 or 9090 --
// RDNA 4 ships no flagship, the 9070 XT is the top part -- and those two names
// sat in this test for a while as though the line-up had them.
//
// IDs are the part that has to be right, because a wrong "yes" here sets
// HSA_OVERRIDE_GFX_VERSION=12.0.1 on a card that is not gfx12, and the note under
// Self-Healing below records the result: the runtime builds gfx12 ELF that the
// driver refuses to load, so every module fails before the first frame.
//
//   Navi 48   RX 9070 XT / 9070 / 9070 GRE, Radeon AI PRO R9700   0x7550
//   Navi 44   RX 9060 XT (16 / 8 GB), RX 9060                     0x7590
//
// This used to accept 0x7480..0x74DF, which is RDNA 3 territory: 0x7480 is the
// RX 7600 and its mobile variants (Navi 33), 0x747E is Navi 32 and 0x744C is
// Navi 31. So the id test matched RDNA 3 and no RDNA 4 card at all. RDNA 2 is
// lower still -- Navi 21/22/23/24 are 0x73BF, 0x73DF, 0x73FF, 0x743F -- and must
// not match either: gfx1030 has no matrix units, so nothing there is helped by
// 12.0.1.
//
// The neighbourhood above each id is an assumption, not a lookup; only 0x7550 and
// 0x7590 are the ids themselves (DeviceHunt, PCI 1002). It fails in the safe
// direction: 0x755x and 0x759x are a whole 0x100 away from the nearest RDNA 3 id,
// so a wrong guess can only fail to inject -- which shows up as the cuInit failure
// the override exists to prevent -- and cannot hand a working card the wrong
// architecture.
inline bool is_rdna4_gpu(const std::wstring &name_lower, UINT device_id) {
    return (name_lower.find(L"9070") != std::wstring::npos ||
            name_lower.find(L"9060") != std::wstring::npos ||
            name_lower.find(L"r9700") != std::wstring::npos ||
            name_lower.find(L"rx 9") != std::wstring::npos ||
            (device_id >= 0x7550 && device_id <= 0x755F) ||
            (device_id >= 0x7590 && device_id <= 0x759F));
}

inline void auto_configure_gpu_environment() {
    // 1. Enumerate GPUs via DXGI
    auto gpus = enumerate_gpus();
    if (gpus.empty()) {
        report_auto_config("[GPU-AutoConfig] Warning: CreateDXGIFactory1 failed\n");
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

        // RDNA 4, per is_rdna4_gpu above -- which is where the reasoning lives.
        bool is_rdna4 = is_rdna4_gpu(name_lower, best_gpu->device_id);

        char msg[768] = {};
        if (is_rdna4) {
            char env_hsa[64] = {};
            DWORD len = GetEnvironmentVariableA("HSA_OVERRIDE_GFX_VERSION", env_hsa, sizeof(env_hsa));
            const bool has_override = (len > 0 && env_hsa[0] != '\0');
            // A stale RDNA3-era override (11.*) on a gfx12 device makes the
            // runtime build gfx11 ELF that the driver refuses to load: every
            // module fails before the first frame (field report, RX 9070 XT,
            // HSA_OVERRIDE_GFX_VERSION=11.0.0 -> cuModuleLoadFile failed).
            // An override that does not name a gfx12 part is therefore
            // corrected rather than honored -- loudly, because a manually set
            // variable usually has a system-level copy that should be removed
            // by hand.
            const bool override_mismatch =
                has_override && strncmp(env_hsa, "12", 2) != 0;
            if (override_mismatch) {
                SetEnvironmentVariableA("HSA_OVERRIDE_GFX_VERSION", "12.0.1");
                _putenv("HSA_OVERRIDE_GFX_VERSION=12.0.1");
                snprintf(msg, sizeof(msg),
                         "[GPU-AutoConfig] Detected %ls (RDNA 4, %zu MB Dedicated VRAM) with "
                         "HSA_OVERRIDE_GFX_VERSION=%s.\n"
                         "[GPU-AutoConfig] Self-Healing: an override that does not name a gfx12 part "
                         "breaks module loading on RDNA 4 -- corrected to 12.0.1.\n"
                         "[GPU-AutoConfig] If you set this variable system-wide (older guides suggest "
                         "11.0.0), remove it there too.\n",
                         best_gpu->name.c_str(), best_gpu->dedicated_vram_mb, env_hsa);
            } else if (!has_override) {
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
            report_auto_config(msg);

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
            report_auto_config(msg);
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
        report_auto_config(msg);
    }
}

} // namespace dlssnr
