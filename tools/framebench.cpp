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

// Nothing to convert here any more. This used MultiByteToWideChar(CP_UTF8) on the
// bytes main() received, and on a CP936 machine those bytes are GBK for any path
// containing Chinese -- decoding GBK as UTF-8 cannot produce a name that opens, and
// the tool reported "[FAIL] input unreadable" as though the file were corrupt.
// wmain hands over the real wide command line, the same way launchbench does.
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

int wmain(int argc, wchar_t **argv) {
    if (argc < 5) {
        printf("usage: framebench <input> <frames> <snippet> <driver> [runtime] [nvapi]\n");
        return 2;
    }
    // A frame count of zero -- atoi("abc"), or a typo -- used to run no frames at all,
    // print "steady-state avg 0.0 ms over 0 frames" and exit 0. That reads exactly
    // like a successful measurement of an extremely fast build. Refuse it instead.
    const int frames = _wtoi(argv[2]);
    if (frames <= 0) {
        printf("[FAIL] frames must be a positive integer (got \"%ls\")\n", argv[2]);
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
    if (!load(wic, argv[1], in)) {
        printf("[FAIL] input unreadable\n");
        return 1;
    }
    enhancer::Paths paths;
    paths.snippet = argv[3];
    paths.cuda_driver = argv[4];
    paths.ngx_runtime = argc > 5 ? std::wstring(argv[5]) : std::wstring(L"nvngx.dll");
    paths.nvapi = argc > 6 ? std::wstring(argv[6]) : std::wstring(L"nvapi64.dll");

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
