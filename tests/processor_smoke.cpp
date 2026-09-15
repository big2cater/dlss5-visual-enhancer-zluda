// Exercises the processor the interface will call, without the interface.
//
// Qt is not needed to find out whether the DLSS side works, and separating the
// two means a failure lands in one place or the other rather than somewhere in
// between.
//
// Usage: processor_smoke <input> <output.png> <snippet> <driver> [runtime] [nvapi]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

#include <cstdio>

#include "../core/image_processor.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

// No narrow-to-wide conversion any more: this used MultiByteToWideChar(CP_UTF8) on the
// bytes main() received, and on a CP936 machine those are GBK for any path containing
// Chinese -- decoded as UTF-8 they cannot be opened, and the tool blamed the input file
// ("[FAIL] the input image could not be read"). wmain supplies the real wide command
// line, as launchbench already did.

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

bool save(IWICImagingFactory *wic, const wchar_t *path, const enhancer::Image &image) {
    IWICBitmap *bitmap = nullptr;
    if (FAILED(wic->CreateBitmapFromMemory(image.width, image.height,
                                           GUID_WICPixelFormat64bppRGBAHalf,
                                           (UINT)image.row_bytes(),
                                           (UINT)(image.row_bytes() * image.height),
                                           (BYTE *)image.pixels.data(), &bitmap)))
        return false;
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *encoder = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    IWICFormatConverter *converter = nullptr;
    bool ok = false;
    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(bitmap, GUID_WICPixelFormat32bppBGRA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(frame->WriteSource(converter, nullptr)) && SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit()))
        ok = true;
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    bitmap->Release();
    return ok;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 5) {
        printf("usage: processor_smoke <input> <output.png> <snippet> <driver> "
               "[runtime] [nvapi]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        printf("[FAIL] Windows Imaging Component unavailable\n");
        return 1;
    }

    enhancer::Image in;
    if (!load(wic, argv[1], in)) {
        printf("[FAIL] the input image could not be read\n");
        return 1;
    }
    printf("       input %ux%u\n", in.width, in.height);

    enhancer::Paths paths;
    paths.snippet = argv[3];
    paths.cuda_driver = argv[4];
    paths.ngx_runtime = argc > 5 ? std::wstring(argv[5]) : std::wstring(L"nvngx.dll");
    paths.nvapi = argc > 6 ? std::wstring(argv[6]) : std::wstring(L"nvapi64.dll");

    enhancer::Processor processor;
    std::string error;
    if (!processor.start(paths, error)) {
        printf("[FAIL] start: %s\n", error.c_str());
        return 1;
    }
    printf("[ OK ] started\n");

    enhancer::Image out;
    enhancer::Settings settings;
    if (!processor.process(in, out, settings, error)) {
        printf("[FAIL] process: %s\n", error.c_str());
        return 1;
    }
    printf("[ OK ] processed in %.0f ms\n", processor.last_ms());

    if (!save(wic, argv[2], out)) {
        printf("[FAIL] the result could not be written\n");
        return 1;
    }
    printf("[ OK ] written to %s\n", argv[2]);
    processor.stop();
    return 0;
}
