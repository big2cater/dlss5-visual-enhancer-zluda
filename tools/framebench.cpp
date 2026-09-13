// Throwaway per-frame timing harness: start once, then run N frames through
// the processor and print each one, so steady-state cost is visible next to
// the first-frame warmup. Same link line as processor_smoke.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../core/image_processor.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

std::wstring widen(const char *narrow) {
    wchar_t buffer[1024] = {};
    MultiByteToWideChar(CP_UTF8, 0, narrow, -1, buffer, 1024);
    return buffer;
}

bool load(IWICImagingFactory *wic, const wchar_t *path, enhancer::Image &image) {
    IWICBitmapDecoder *decoder = nullptr;
    if (FAILED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *converter = nullptr;
    bool ok = false;
    if (SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat64bppRGBAHalf,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(converter->GetSize(&image.width, &image.height))) {
        image.pixels.resize((size_t)image.width * image.height * 4);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, image.width * 8,
                                             (UINT)(image.pixels.size() * 2),
                                             (BYTE *)image.pixels.data()));
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    decoder->Release();
    return ok;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 5) {
        printf("usage: framebench <input> <frames> <snippet> <driver> [runtime] [nvapi]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        printf("[FAIL] WIC unavailable\n");
        return 1;
    }
    enhancer::Image in;
    if (!load(wic, widen(argv[1]).c_str(), in)) {
        printf("[FAIL] input unreadable\n");
        return 1;
    }
    const int frames = atoi(argv[2]);

    enhancer::Paths paths;
    paths.snippet = widen(argv[3]);
    paths.cuda_driver = widen(argv[4]);
    paths.ngx_runtime = argc > 5 ? widen(argv[5]) : L"nvngx.dll";
    paths.nvapi = argc > 6 ? widen(argv[6]) : L"nvapi64.dll";

    enhancer::Processor processor;
    std::string error;
    const ULONGLONG t0 = GetTickCount64();
    if (!processor.start(paths, error)) {
        printf("[FAIL] start: %s\n", error.c_str());
        return 1;
    }
    printf("[ OK ] start in %llu ms (%ls)\n", GetTickCount64() - t0,
           processor.device_name().c_str());

    enhancer::Image out;
    enhancer::Settings settings;
    settings.is_video = true; // exercise the temporal (motion/history) path
    double worst_after_warmup = 0.0;
    double sum_after_warmup = 0.0;
    int counted = 0;
    for (int i = 0; i < frames; ++i) {
        if (!processor.process(in, out, settings, error)) {
            printf("[FAIL] frame %d: %s\n", i, error.c_str());
            return 1;
        }
        printf("frame %2d: %7.1f ms\n", i, processor.last_ms());
        if (i >= 3) { // skip warmup frames for the steady-state summary
            sum_after_warmup += processor.last_ms();
            worst_after_warmup =
                worst_after_warmup > processor.last_ms() ? worst_after_warmup : processor.last_ms();
            ++counted;
        }
    }
    printf("[ OK ] steady-state avg %.1f ms, worst %.1f ms over %d frames\n",
           counted ? sum_after_warmup / counted : 0.0, worst_after_warmup, counted);
    processor.stop();
    return 0;
}
