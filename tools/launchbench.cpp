// Throwaway microbenchmark: how much does one ZLUDA cuLaunchKernel cost on
// the CPU, and one cuCtxSynchronize? Loads the small cg2r_copy_kernel PTX
// grabbed by zluda_trace, builds a real texture + surface for it, launches
// it many times, and reports per-call wall time.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <vector>

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) {
        wprintf(L"usage: launchbench <nvcuda.dll> <module.ptx>\n");
        return 2;
    }
    HMODULE cuda = LoadLibraryW(argv[1]);
    if (!cuda) { wprintf(L"[FAIL] driver load\n"); return 1; }
    auto cuInit = (int (*)(unsigned))GetProcAddress(cuda, "cuInit");
    auto cuDeviceGet = (int (*)(int *, int))GetProcAddress(cuda, "cuDeviceGet");
    auto cuCtxCreate = (int (*)(void **, unsigned, int))GetProcAddress(cuda, "cuCtxCreate_v2");
    auto cuModuleLoadData = (int (*)(void **, const void *))GetProcAddress(cuda, "cuModuleLoadData");
    auto cuModuleGetFunction = (int (*)(void **, void *, const char *))GetProcAddress(cuda, "cuModuleGetFunction");
    auto cuLaunchKernel = (int (*)(void *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                                   unsigned, void *, void **, void **))GetProcAddress(cuda, "cuLaunchKernel");
    auto cuCtxSynchronize = (int (*)(void))GetProcAddress(cuda, "cuCtxSynchronize");
    auto cuMemAlloc = (int (*)(void **, size_t))GetProcAddress(cuda, "cuMemAlloc_v2");
    auto cuArrayCreate = (int (*)(void **, const void *))GetProcAddress(cuda, "cuArrayCreate_v2");
    auto cuSurfObjectCreate = (int (*)(void *, const void *))GetProcAddress(cuda, "cuSurfObjectCreate");
    auto cuTexObjectCreate = (int (*)(void *, const void *, const void *, const void *))GetProcAddress(cuda, "cuTexObjectCreate");
    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuModuleLoadData || !cuModuleGetFunction ||
        !cuLaunchKernel || !cuCtxSynchronize || !cuMemAlloc || !cuArrayCreate ||
        !cuSurfObjectCreate || !cuTexObjectCreate) {
        wprintf(L"[FAIL] driver exports missing\n");
        return 1;
    }
    if (cuInit(0) != 0) { wprintf(L"[FAIL] cuInit\n"); return 1; }
    int device = 0;
    void *ctx = nullptr;
    if (cuDeviceGet(&device, 0) != 0 || cuCtxCreate(&ctx, 0, device) != 0) {
        wprintf(L"[FAIL] ctx\n"); return 1;
    }

    std::ifstream file(argv[2], std::ios::binary);
    if (!file) {
        // Without this check a path that does not exist produced an empty buffer, the
        // module load failed, and the tool reported "[FAIL] module load" -- pointing
        // the reader at the kernel rather than at the path they mistyped.
        wprintf(L"[FAIL] cannot open %ls\n", argv[2]);
        return 1;
    }
    std::vector<unsigned char> ptx((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
    ptx.push_back(0);
    void *module = nullptr;
    if (cuModuleLoadData(&module, ptx.data()) != 0) { wprintf(L"[FAIL] module load\n"); return 1; }
    void *kernel = nullptr;
    if (cuModuleGetFunction(&kernel, module, "cg2r_copy_kernel") != 0) {
        wprintf(L"[FAIL] get function\n"); return 1;
    }

    // An 8x8 RGBA float array for the kernel's texture read and surface write.
    // CUDA_ARRAY_DESCRIPTOR: size_t Width, size_t Height, Format, NumChannels.
    struct ArrayDesc {
        unsigned long long width = 8, height = 8;
        unsigned format = 0x20; // CU_AD_FORMAT_FLOAT
        unsigned channels = 4;
    } array_desc;
    void *array = nullptr;
    if (cuArrayCreate(&array, &array_desc) != 0) { wprintf(L"[FAIL] array\n"); return 1; }
    // CUDA_RESOURCE_DESC: resType 0 (array) + hArray at +8; the same desc
    // serves both cuSurfObjectCreate and cuTexObjectCreate.
    unsigned long long res[16] = {};
    res[0] = 0; res[1] = (unsigned long long)array;
    unsigned long long surf = 0, tex = 0;
    if (cuSurfObjectCreate(&surf, res) != 0) { wprintf(L"[FAIL] surf\n"); return 1; }
    unsigned long long tex_desc[8] = {}; // CUDA_TEXTURE_DESC all defaults
    if (cuTexObjectCreate(&tex, res, tex_desc, nullptr) != 0) { wprintf(L"[FAIL] tex\n"); return 1; }

    // The kernel's 72-byte parameter block: tex handle, surface handle, six
    // ints, then width and height.
    unsigned long long params[9] = {};
    params[0] = tex;
    params[1] = surf;
    for (int i = 2; i < 8; ++i) params[i] = 0x3F800000; // 1.0f pattern
    params[8] = (unsigned long long)8 | ((unsigned long long)8 << 32);
    void *kernel_params[1] = {params};

    const int warmup = 100;
    const int launches = 2000;
    int first_error = 0;
    for (int i = 0; i < warmup; ++i) {
        const int rc = cuLaunchKernel(kernel, 1, 1, 1, 8, 8, 1, 0, nullptr, kernel_params, nullptr);
        if (rc != 0 && !first_error) first_error = rc;
    }
    cuCtxSynchronize();

    auto t0 = Clock::now();
    for (int i = 0; i < launches; ++i) {
        const int rc = cuLaunchKernel(kernel, 1, 1, 1, 8, 8, 1, 0, nullptr, kernel_params, nullptr);
        if (rc != 0 && !first_error) first_error = rc;
    }
    const double launch_ms = ms_since(t0);
    cuCtxSynchronize();
    // A refused launch still costs time, so a run where every launch failed printed a
    // perfectly plausible "0.6 us per launch" -- a benchmark of the failure path,
    // quoted into docs/perf_bottleneck_analysis_2026-09-12.md as a real number.
    if (first_error) {
        wprintf(L"[FAIL] cuLaunchKernel returned %d: this is not a launch timing\n", first_error);
        return 1;
    }
    wprintf(L"[ OK ] %d launches: %.1f ms total, %.1f us per launch\n", launches, launch_ms,
            launch_ms * 1000.0 / launches);

    const int syncs = 200;
    auto t1 = Clock::now();
    for (int i = 0; i < syncs; ++i) cuCtxSynchronize();
    wprintf(L"[ OK ] %d ctx syncs (GPU idle): %.1f us per sync\n", syncs,
            ms_since(t1) * 1000.0 / syncs);
    return 0;
}
