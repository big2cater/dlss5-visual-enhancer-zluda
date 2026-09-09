// video_filter — run the DLSS 5 neural rendering network over a video.
//
// The network itself is a still-frame feature, but it keeps an accumulation
// history between evaluations, and that history is what makes video work:
// frame after frame runs against the same feature session, the history only
// being reset at a scene cut. Without any depth or motion vectors (a real
// video has neither handy), the network cannot reproject the history, so
// moving content may swim or ghost -- the --reset options exist to trade a
// little of that away for stability.
//
// I/O is handled by ffmpeg on PATH (or $FFMPEG_PATH), as subprocesses:
//   decode:  ffmpeg -nostdin -v error -i in -an -f rawvideo -pix_fmt rgb48le -
//   encode:  ffmpeg -nostdin -v error -y -i in -vn \
//                -f rawvideo -pix_fmt rgb48le -s WxH -r FPS -i - \
//                -map 0:a? -map 1:v -c:v libx264 -crf N -pix_fmt yuv420p \
//                -c:a copy out
// Audio is passed through from the source; the video comes from the pipe.
//
// Usage:
//   video_filter <input> <output> <snippet> <driver> [runtime] [nvapi] [options]
//   video_filter --compile-one <module> <driver>      (internal, precompile)
//   video_filter --precompile <snippet> <driver> [jobs] (translation on its own)
//
// Options (after the six positional arguments):
//   --passes N            evaluate each frame N times (1 default; video rarely
//                         benefits, stills do -- it is the still-path trick)
//   --reset auto|always|never|every=N
//                         when to clear the accumulation history
//                         auto (default): reset on the first frame and on
//                           detected scene cuts; never: only the first frame;
//                           always: every frame (safest, can flicker);
//                           every=N: every N frames
//   --cut-threshold F     scene-cut difference threshold (0..1, 0.30 default)
//   --intensity F         network intensity  (default 1.0)
//   --global-tone F       (default 0.0)
//   --local-tone F        (default 1.0)
//   --local-structure F   (default 1.0)
//   --skin-structure F    (default 0.0)
//   --style N             network style: 0 default, 1 natural, 2 cinematic
//   --preset N            network preset, 0 = let the network choose
//   --no-auto-mask        disable the network's own mask
//   --crf N               libx264 quality (18 default; lower = better)
//   --fps N               override the output frame rate
//   --no-audio            don't mux the source audio into the output
//   --max-frames N        stop after N frames (for short test runs)
//   --dump-frames DIR     also write each output frame as PNG into DIR
//
// Exit codes: 0 ok, 1 runtime failure, 2 usage.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../core/image_processor.h"
#include "../core/precompile.h"
#include "half_float.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

// --------------------------------------------------------------------------
// Process plumbing
// --------------------------------------------------------------------------

// Children die when we do, cleanly or not: precompilation can run long, and an
// interrupted run would otherwise leave the translation copies behind.
void kill_children_when_this_process_ends() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info) ||
        !AssignProcessToJobObject(job, GetCurrentProcess())) {
        CloseHandle(job);
    }
}

struct Pipe {
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;

    bool make(bool inheritable_read, bool inheritable_write) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof attributes;
        attributes.bInheritHandle = TRUE;
        if (!CreatePipe(&read_end, &write_end, &attributes, 0)) return false;
        // Keep the ends we use ourselves from being inherited by mistake.
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, inheritable_read ? HANDLE_FLAG_INHERIT : 0);
        SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, inheritable_write ? HANDLE_FLAG_INHERIT : 0);
        return true;
    }

    void close() {
        if (read_end) { CloseHandle(read_end); read_end = nullptr; }
        if (write_end) { CloseHandle(write_end); write_end = nullptr; }
    }
};

struct ChildProcess {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE stdin_write = nullptr; // ends we own, closed when done
    HANDLE stdout_read = nullptr;
    HANDLE stderr_share = nullptr;

    void close() {
        if (stdin_write) { CloseHandle(stdin_write); stdin_write = nullptr; }
        if (stdout_read) { CloseHandle(stdout_read); stdout_read = nullptr; }
        if (stderr_share) { CloseHandle(stderr_share); stderr_share = nullptr; }
        if (thread) { CloseHandle(thread); thread = nullptr; }
        if (process) { CloseHandle(process); process = nullptr; }
    }
};

bool spawn(const std::wstring &command, Pipe &feed /*child stdin*/, Pipe &collect /*child stdout*/,
           ChildProcess &out) {
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = feed.read_end ? feed.read_end : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = collect.write_end ? collect.write_end : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION process{};
    std::wstring mutable_command = command;
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    out.process = process.hProcess;
    out.thread = process.hThread;
    // The halves handed to the child are no longer ours.
    if (feed.read_end) { CloseHandle(feed.read_end); feed.read_end = nullptr; }
    if (collect.write_end) { CloseHandle(collect.write_end); collect.write_end = nullptr; }
    return true;
}

bool wait_exit(HANDLE process, DWORD timeout_ms, DWORD &code) {
    if (WaitForSingleObject(process, timeout_ms) != WAIT_OBJECT_0) return false;
    code = 1;
    GetExitCodeProcess(process, &code);
    return true;
}

std::wstring widen(const char *narrow) {
    wchar_t buffer[1024] = {};
    MultiByteToWideChar(CP_UTF8, 0, narrow, -1, buffer, 1024);
    return buffer;
}

// True ffmpeg/ffprobe invocation used by the helpers below.
std::wstring tool_cmd(bool probe) {
    wchar_t from_env[4096] = {};
    if (GetEnvironmentVariableW(L"FFMPEG_PATH", from_env, 4096) && from_env[0]) {
        std::wstring dir = from_env;
        // A trailing backslash would make "dir\ffmpeg.exe" if we just joined.
        if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
        return dir + (probe ? L"ffprobe.exe" : L"ffmpeg.exe");
    }
    return probe ? L"ffprobe" : L"ffmpeg";
}

// --------------------------------------------------------------------------
// Frame conversion
// --------------------------------------------------------------------------

struct VideoParams {
    unsigned width = 0;
    unsigned height = 0;
    double fps = 0.0;
};

// ffprobe -> "width,height,r_frame_rate" csv line ("1920,1080,30000/1001").
bool probe_video(const std::wstring &input, VideoParams &params, std::string &error) {
    Pipe pipe;
    if (!pipe.make(false, true)) { error = "pipe creation failed"; return false; }
    ChildProcess child;
    std::wstring command = tool_cmd(true) + L" -v error -select_streams v:0 "
                           L"-show_entries stream=width,height,r_frame_rate -of csv=p=0 \"" +
                           input + L"\"";
    if (!spawn(command, Pipe{}, pipe, child)) {
        error = "ffprobe could not be started (is ffmpeg on PATH, or $FFMPEG_PATH set?)";
        pipe.close();
        return false;
    }
    std::string text;
    char buffer[512];
    DWORD read = 0;
    while (ReadFile(pipe.read_end, buffer, sizeof buffer, &read, nullptr) && read) {
        text.append(buffer, read);
    }
    pipe.close();
    DWORD code = 1;
    if (!wait_exit(child.process, 15000, code) || code != 0) {
        error = "ffprobe failed (code " + std::to_string(code) + "): " + text;
        child.close();
        return false;
    }
    child.close();

    unsigned w = 0, h = 0;
    char rate[64] = {};
    if (sscanf(text.c_str(), "%u,%u,%63s", &w, &h, rate) != 3 || !w || !h) {
        error = "could not parse ffprobe output: " + text;
        return false;
    }
    params.width = w;
    params.height = h;
    unsigned num = 0, den = 1;
    if (sscanf(rate, "%u/%u", &num, &den) == 2 && num && den) params.fps = (double)num / den;
    else if (sscanf(rate, "%u", &num) == 1 && num) params.fps = (double)num;
    return true;
}

// Decoder: ffmpeg -i in -an -f rawvideo -pix_fmt rgb48le -
bool start_decoder(const std::wstring &input, Pipe &collect, ChildProcess &child) {
    Pipe empty;
    std::wstring command = tool_cmd(false) + L" -nostdin -v error -i \"" + input +
                           L"\" -an -f rawvideo -pix_fmt rgb48le -";
    if (!spawn(command, empty, collect, child)) return false;
    return true;
}

// Encoder: audio by copy from the source, video from the raw pipe.
bool start_encoder(const std::wstring &input, const std::wstring &output,
                   const VideoParams &params, int crf, bool audio, Pipe &feed,
                   ChildProcess &child, unsigned output_width = 0, unsigned output_height = 0) {
    Pipe empty;
    char rate[64];
    if (params.fps > 0.0) {
        // Keep the source's exact fraction when we can ($fps came from ffprobe
        // as num/den, so pass it through as close as we can).
        snprintf(rate, sizeof rate, "%.6f", params.fps);
        char *dot = strchr(rate, '.');
        if (dot) {
            char *end = dot + 1;
            while (*end == '0') ++end;
            if (*end == '\0') *dot = '\0';
        }
    } else {
        strcpy(rate, "30");
    }
    if (!output_width) output_width = params.width;
    if (!output_height) output_height = params.height;
    std::wstring command = tool_cmd(false) + L" -nostdin -v error -y -i \"" + input + L"\" " +
                           L"-f rawvideo -pix_fmt rgb48le -s " +
                           std::to_wstring(params.width) + L"x" + std::to_wstring(params.height) +
                           L" -r " + widen(rate) + L" -i - " +
                           (audio ? L"-map 0:a? " : L"") +
                           L"-map 1:v " +
                           ((output_width != params.width || output_height != params.height) ?
                            (L"-vf scale=" + std::to_wstring(output_width) + L":" + std::to_wstring(output_height) + L":flags=lanczos ") : L"") +
                           L"-c:v libx264 -crf " + std::to_wstring(crf) +
                           L" -preset medium -pix_fmt yuv420p " +
                           (audio ? L"-c:a copy " : L"-an ") +
                           L"\"" + output + L"\"";
    if (!spawn(command, feed, empty, child)) return false;
    return true;
}

void rgb48_to_half_rgba(const unsigned char *rgb48, enhancer::Image &image) {
    const uint16_t *src = (const uint16_t *)rgb48;
    uint16_t *dst = image.pixels.data();
    const size_t count = (size_t)image.width * image.height;
    for (size_t i = 0; i < count; ++i) {
        const uint16_t r = src[i * 3 + 0];
        const uint16_t g = src[i * 3 + 1];
        const uint16_t b = src[i * 3 + 2];
        dst[i * 4 + 0] = enhancer::float_to_half(r / 65535.0f);
        dst[i * 4 + 1] = enhancer::float_to_half(g / 65535.0f);
        dst[i * 4 + 2] = enhancer::float_to_half(b / 65535.0f);
        dst[i * 4 + 3] = 0x3C00; // 1.0
    }
}

// --------------------------------------------------------------------------
// Output tone handling. The network writes LINEAR (HDR-style) half floats;
// storing them directly as sRGB makes images look dark and over-saturated.
// The default applies the linear->sRGB gamma (2.2) so a 0.5 input comes back
// as a 0.5 output; --gamma 1 disables it.
// --------------------------------------------------------------------------
double g_gamma = 2.2;

static inline double to_sdr(double value) {
    if (value <= 0.0) return 0.0;
    if (value >= 1.0) return 1.0;
    return std::pow(value, 1.0 / g_gamma);
}

uint16_t clamp_half_to_u16(uint16_t half) {
    float value = enhancer::half_to_float(half);
    if (value <= 0.0f) return 0;
    if (value >= 1.0f) return 65535;
    return (uint16_t)(to_sdr(value) * 65535.0 + 0.5);
}

// half RGBA -> rgb48, and while we are in there, a cheap blank-frame check.
void half_rgba_to_rgb48(const enhancer::Image &image, unsigned char *rgb48, bool &blank) {
    const uint16_t *src = image.pixels.data();
    uint16_t *dst = (uint16_t *)rgb48;
    const size_t count = (size_t)image.width * image.height;
    double sum = 0.0, sum_sq = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const uint16_t r = clamp_half_to_u16(src[i * 4 + 0]);
        const uint16_t g = clamp_half_to_u16(src[i * 4 + 1]);
        const uint16_t b = clamp_half_to_u16(src[i * 4 + 2]);
        dst[i * 3 + 0] = r;
        dst[i * 3 + 1] = g;
        dst[i * 3 + 2] = b;
        const double luma = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 65535.0;
        sum += luma;
        sum_sq += luma * luma;
    }
    const double mean = sum / (double)count;
    const double variance = sum_sq / (double)count - mean * mean;
    blank = mean < 0.05 && variance < 0.01;
}

void resize_rgb48(const unsigned char *src, unsigned sw, unsigned sh,
                  unsigned char *dst, unsigned dw, unsigned dh) {
    const uint16_t *in = (const uint16_t *)src; uint16_t *out = (uint16_t *)dst;
    for (unsigned y=0; y<dh; ++y) {
        double fy = ((double)y + 0.5) * sh / dh - 0.5; int y0=(int)floor(fy); double ty=fy-y0;
        if(y0<0){y0=0;ty=0;} if(y0>=(int)sh-1){y0=(int)sh-1;ty=0;} int y1=(y0+1<(int)sh)?y0+1:y0;
        for (unsigned x=0; x<dw; ++x) {
            double fx=((double)x+0.5)*sw/dw-0.5; int x0=(int)floor(fx); double tx=fx-x0;
            if(x0<0){x0=0;tx=0;} if(x0>=(int)sw-1){x0=(int)sw-1;tx=0;} int x1=(x0+1<(int)sw)?x0+1:x0;
            for(int c=0;c<3;++c){ double a=in[((size_t)y0*sw+x0)*3+c]*(1-tx)+in[((size_t)y0*sw+x1)*3+c]*tx; double b=in[((size_t)y1*sw+x0)*3+c]*(1-tx)+in[((size_t)y1*sw+x1)*3+c]*tx; out[((size_t)y*dw+x)*3+c]=(uint16_t)(a*(1-ty)+b*ty+0.5); }
        }
    }
}

// --------------------------------------------------------------------------
// Scene-cut detection (reset trigger)
// --------------------------------------------------------------------------

struct SceneDetector {
    double threshold = 0.30; // mean luma difference in 0..1 that counts as a cut
    std::vector<uint16_t> previous_luma; // strided grid, null until the first frame
    unsigned step_x = 1, step_y = 1;

    void configure(unsigned width, unsigned height, double threshold_value) {
        threshold = threshold_value;
        // A grid of roughly 64x36 samples; striding reads the raw 16-bit
        // planes directly, no resampling needed for a cut test.
        step_x = width / 64; if (step_x < 1) step_x = 1;
        step_y = height / 36; if (step_y < 1) step_y = 1;
    }

    // Returns true when the incoming frame should reset the accumulation.
    bool consider(const unsigned char *rgb48, unsigned width, unsigned height) {
        const uint16_t *src = (const uint16_t *)rgb48;
        std::vector<uint16_t> current;
        for (unsigned y = 0; y < height; y += step_y) {
            for (unsigned x = 0; x < width; x += step_x) {
                const uint16_t r = src[((size_t)y * width + x) * 3 + 0];
                const uint16_t g = src[((size_t)y * width + x) * 3 + 1];
                const uint16_t b = src[((size_t)y * width + x) * 3 + 2];
                current.push_back((uint16_t)(((uint32_t)r * 19595 + (uint32_t)g * 38470 +
                                              (uint32_t)b * 7471) >> 16));
            }
        }
        if (previous_luma.empty()) {
            previous_luma = std::move(current);
            return true; // first frame always resets
        }
        if (current.size() != previous_luma.size()) {
            previous_luma = std::move(current);
            return true;
        }
        uint64_t total = 0;
        for (size_t i = 0; i < current.size(); ++i) {
            uint64_t d = current[i] > previous_luma[i] ? current[i] - previous_luma[i]
                                                       : previous_luma[i] - current[i];
            total += d;
        }
        // Save the sample count before moving the vector.  Reading
        // current.size() after std::move() usually returns zero, which made
        // mean_diff become infinity and caused auto mode to reset every frame
        // (visible as severe video flicker).
        const size_t sample_count = current.size();
        previous_luma = std::move(current);
        const double mean_diff = sample_count
                                     ? (double)total / ((double)sample_count * 65535.0)
                                     : 1.0;
        return mean_diff >= threshold;
    }
};

// --------------------------------------------------------------------------
// Frame dump (debug aid)
// --------------------------------------------------------------------------

bool dump_png(IWICImagingFactory *wic, const std::wstring &path, const enhancer::Image &image) {
    // 8-bit BGRA in the order WIC wants for 32bppBGRA.
    std::vector<unsigned char> bgra((size_t)image.width * image.height * 4);
    const uint16_t *src = image.pixels.data();
    for (size_t i = 0; i < (size_t)image.width * image.height; ++i) {
        bgra[i * 4 + 0] = clamp_half_to_u16(src[i * 4 + 2]) >> 8;
        bgra[i * 4 + 1] = clamp_half_to_u16(src[i * 4 + 1]) >> 8;
        bgra[i * 4 + 2] = clamp_half_to_u16(src[i * 4 + 0]) >> 8;
        bgra[i * 4 + 3] = 255;
    }
    IWICBitmap *bitmap = nullptr;
    if (FAILED(wic->CreateBitmapFromMemory(image.width, image.height,
                                           GUID_WICPixelFormat32bppBGRA, image.width * 4,
                                           (UINT)((size_t)image.width * 4 * image.height),
                                           bgra.data(), &bitmap)))
        return false;
    // The PNG encoder takes the frame through a converter (WriteSource), the
    // same route the still-image path uses -- direct SetPixelFormat/WritePixels
    // with BGRA is refused by some WIC PNG builds.
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *encoder = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    IWICFormatConverter *converter = nullptr;
    bool ok = false;
    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
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

// --------------------------------------------------------------------------
// Options
// --------------------------------------------------------------------------

enum class ResetMode { Auto, Always, Never, Every };

struct Options {
    std::wstring input, output, snippet, driver, runtime, nvapi;
    enhancer::Settings settings;
    int crf = 18;
    // Higher than it used to be (0.15). Every reset discards the accumulation
    // history, and the frame after one comes out at a different level from the
    // ones before it, so a cut test that trips on ordinary motion reads as
    // flicker. 0.30 is a mean luma change only a real cut reaches.
    double cut_threshold = 0.30;
    double gamma = 2.2;      // linear -> sRGB conversion for saved pixels
    // Off by default. The estimator is a coarse CPU block matcher -- a grid at
    // 1/16 of the frame, whole-pixel SAD, ties resolved by scan order -- so the
    // field it produces jitters from frame to frame, and the network reprojects
    // its history by that jitter, which shimmers. With no vectors the history is
    // blended as it stands: stable, at the cost of some ghosting behind fast
    // motion. --flow 1 for that trade the other way.
    bool flow = false;
    ResetMode reset = ResetMode::Auto;
    unsigned reset_every = 60;
    double fps_override = 0.0;
    int max_frames = 0;
    bool audio = true;
    std::wstring dump_dir;
    double upscale = 1.0;
    double model_scale = 1.0; // compatibility; legacy GUI flag is ignored
    std::string dlss_model_preset = "default";
};

void usage() {
    printf(
        "video_filter -- run the DLSS 5 neural rendering network over a video\n\n"
        "usage: video_filter <input> <output> <snippet> <driver> [runtime] [nvapi] [options]\n"
        "       video_filter --compile-one <module> <driver>   (internal)\n"
        "       video_filter --precompile <snippet> <driver> [jobs]\n"
        "       (--precompile-wait anywhere: strict-serial prewarm, main process\n"
        "        then runs without its own parallel translation)\n\n"
        "needs ffmpeg on PATH (or $FFMPEG_PATH), the network dll, and ZLUDA's\n"
        "nvcuda.dll/nvapi64.dll + nvngx.dll from this project's release zip.\n\n"
        "options:\n"
        "  --passes N            evaluate each frame N times (1 default)\n"
        "  --reset auto|always|never|every=N   accumulation reset policy (auto)\n"
        "  --cut-threshold F     scene-cut threshold, 0..1 (0.30)\n"
        "  --gamma F             output gamma, linear->sRGB (2.2 default, 1 = off)\n"
        "  --flow 0|1           motion-vector guidance for video (0 default; 1 = on)\n"
        "  --upscale-mode P     native, quality, balanced, performance, ultra\n"
        "  --dlss-model-preset P DLSS model preset: default, J, K, L, M\n"
        "  --intensity F --global-tone F --local-tone F --local-structure F\n"
        "  --skin-structure F --style N --preset N --no-auto-mask\n"
        "  --crf N               x264 quality (18)\n"
        "  --fps N               override output frame rate\n"
        "  --no-audio            don't copy the source audio\n"
        "  --max-frames N        stop after N frames\n"
        "  --dump-frames DIR     write each output frame as PNG into DIR\n");
}

bool parse_args(int argc, char **argv, Options &options) {
    if (argc < 7) { usage(); return false; }
    options.input = widen(argv[1]);
    options.output = widen(argv[2]);
    options.snippet = widen(argv[3]);
    options.driver = widen(argv[4]);
    options.runtime = argc > 5 ? widen(argv[5]) : L"nvngx.dll";
    options.nvapi = argc > 6 ? widen(argv[6]) : L"nvapi64.dll";

    for (int i = 7; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", name); return nullptr; }
            return argv[++i];
        };
        if (arg == "--passes") {
            const char *v = need("--passes"); if (!v) return false;
            options.settings.passes = atoi(v);
        } else if (arg == "--reset") {
            const char *v = need("--reset"); if (!v) return false;
            if (!strcmp(v, "auto")) options.reset = ResetMode::Auto;
            else if (!strcmp(v, "always")) options.reset = ResetMode::Always;
            else if (!strcmp(v, "never")) options.reset = ResetMode::Never;
            else if (!strncmp(v, "every=", 6)) {
                options.reset = ResetMode::Every;
                options.reset_every = (unsigned)atoi(v + 6);
                if (!options.reset_every) options.reset_every = 60;
            } else { fprintf(stderr, "unknown --reset mode: %s\n", v); return false; }
        } else if (arg == "--cut-threshold") {
            const char *v = need("--cut-threshold"); if (!v) return false;
            options.cut_threshold = atof(v);
        } else if (arg == "--gamma") {
            const char *v = need("--gamma"); if (!v) return false;
            options.gamma = atof(v);
            if (options.gamma < 0.1) options.gamma = 0.1;
        } else if (arg == "--flow") {
            const char *v = need("--flow"); if (!v) return false;
            options.flow = atoi(v) != 0;
        } else if (arg == "--upscale-mode") {
            const char *v = need("--upscale-mode"); if (!v) return false;
            if (!_stricmp(v, "quality")) options.upscale = 1.5;
            else if (!_stricmp(v, "balanced")) options.upscale = 1.724;
            else if (!_stricmp(v, "performance")) options.upscale = 2.0;
            else if (!_stricmp(v, "ultra")) options.upscale = 3.0;
            else options.upscale = 1.0;
        } else if (arg == "--model-scale") {
            need("--model-scale"); // legacy option; internal model scaling is removed
        } else if (arg == "--dlss-model-preset") {
            const char *v = need("--dlss-model-preset"); if (!v) return false;
            options.dlss_model_preset = v;
        } else if (arg == "--retries") {
            // Consumed by the outer process-level retry wrapper; accept it here
            // so the video argument list rejects nothing.
            if (!need("--retries")) return false;
        } else if (arg == "--flow-only") {
            // Diagnostic short-circuit handled later in run_main_once; just
            // accept the flag here.
        } else if (arg == "--intensity") {
            const char *v = need("--intensity"); if (!v) return false;
            options.settings.intensity = (float)atof(v);
        } else if (arg == "--global-tone") {
            const char *v = need("--global-tone"); if (!v) return false;
            options.settings.global_tone = (float)atof(v);
        } else if (arg == "--local-tone") {
            const char *v = need("--local-tone"); if (!v) return false;
            options.settings.local_tone = (float)atof(v);
        } else if (arg == "--local-structure") {
            const char *v = need("--local-structure"); if (!v) return false;
            options.settings.local_structure = (float)atof(v);
        } else if (arg == "--skin-structure") {
            const char *v = need("--skin-structure"); if (!v) return false;
            options.settings.skin_structure = (float)atof(v);
        } else if (arg == "--style") {
            const char *v = need("--style"); if (!v) return false;
            options.settings.style = atoi(v);
        } else if (arg == "--preset") {
            const char *v = need("--preset"); if (!v) return false;
            options.settings.preset = atoi(v);
        } else if (arg == "--no-auto-mask") {
            options.settings.auto_mask = false;
        } else if (arg == "--crf") {
            const char *v = need("--crf"); if (!v) return false;
            options.crf = atoi(v);
        } else if (arg == "--fps") {
            const char *v = need("--fps"); if (!v) return false;
            options.fps_override = atof(v);
        } else if (arg == "--no-audio") {
            options.audio = false;
        } else if (arg == "--max-frames") {
            const char *v = need("--max-frames"); if (!v) return false;
            options.max_frames = atoi(v);
        } else if (arg == "--dump-frames") {
            const char *v = need("--dump-frames"); if (!v) return false;
            options.dump_dir = widen(v);
        } else if (arg == "--precompile-wait" || arg == "--retries" || arg == "--retry-delay") {
            // 外壳模式标志：解析层忽略，由 main() 统一处理
            if (arg == "--retries" || arg == "--retry-delay") need(arg.c_str());
        } else {
            fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage();
            return false;
        }
    }
    return true;
}

// --------------------------------------------------------------------------
// CPU motion-vector generation (mvec guidance for the network). This remains
// the compatibility fallback; video mode prefers the D3D12 GpuFlow below.
//
// The DLSS-NR feature accepts a motion-vector texture: backward screen-space
// displacement in pixels ("where the current pixel was in the previous
// frame"). Feeding it gives the network temporal reprojection guidance, which
// stops ghosting/swimming when video frames chain through accumulation. This
// module estimates the vectors with a pure-CPU coarse-to-fine block matcher
// (no OpenCV, no compute shaders), matches video2dlssnr's convention: RG16
// half-float, backward, pixels, zeroed on reset/scene-cut.
// --------------------------------------------------------------------------
struct CpuFlow {
    unsigned fw = 0, fh = 0;                 // full resolution
    unsigned qw = 0, qh = 0;                 // 1/4 grid
    unsigned ew = 0, eh = 0;                 // 1/8 grid
    std::vector<unsigned char> prevQ;        // previous 1/4 luma
    std::vector<unsigned char> prevE;        // previous 1/8 luma
    std::vector<short> flowE;                // 1/8 dx,dy pairs
    std::vector<uint16_t> previousMotion;    // smoothed full-resolution field
    bool has_prev = false;

    void reset() { has_prev = false; previousMotion.clear(); }

    static void luma_scale(const unsigned char *rgb48, unsigned W, unsigned H, unsigned s,
                           std::vector<unsigned char> &out) {
        unsigned w = (W + s - 1) / s, h = (H + s - 1) / s;
        out.assign((size_t)w * h, 0);
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                unsigned px = x * s < W ? x * s : W - 1;
                unsigned py = y * s < H ? y * s : H - 1;
                const uint16_t *p = (const uint16_t *)(rgb48 + ((size_t)py * W + px) * 6);
                unsigned v = ((unsigned)p[0] * 19595 + (unsigned)p[1] * 38470 +
                              (unsigned)p[2] * 7471) >> 16;
                out[(size_t)y * w + x] = (unsigned char)(v >> 8);
            }
    }

    // Coarse-to-fine block matcher between `prev` and `cur` luma grids.
    // `init` optionally holds the displacement from the coarser level, one
    // (dx,dy) per output cell (same indexing); the search refines around it.
    static void match(const std::vector<unsigned char> &prev, const std::vector<unsigned char> &cur,
                      unsigned W, unsigned H, unsigned step, unsigned radius, unsigned block,
                      const std::vector<short> &init, bool haveInit, std::vector<short> &out) {
        const unsigned cw = (W + step - 1) / step, ch = (H + step - 1) / step;
        out.assign((size_t)cw * ch * 2, 0);
        for (unsigned cy = 0, oy = 0; oy < ch; cy += step, ++oy) {
            if (cy >= H) cy = H - 1;
            for (unsigned cx = 0, ox = 0; ox < cw; cx += step, ++ox) {
                if (cx >= W) cx = W - 1;
                short baseDx = 0, baseDy = 0;
                if (haveInit) {
                    size_t idx = ((size_t)oy * cw + ox) * 2;
                    if (idx + 1 < init.size()) {
                        baseDx = init[idx];
                        baseDy = init[idx + 1];
                    }
                }
                // Flat-region gate: a textureless block (all-black sky, flat
                // walls) makes the SAD tie out equally for every displacement,
                // and tie-breaking picks arbitrary noise. If the current block
                // has almost no local contrast, force zero flow -- the network
                // then treats that region as "no motion", which is right.
                const unsigned byf = H - cy < block ? H - cy : block;
                const unsigned bxf = W - cx < block ? W - cx : block;
                long cMean = 0, cEnergy = 0;
                for (unsigned yy = 0; yy < byf; ++yy)
                    for (unsigned xx = 0; xx < bxf; ++xx)
                        cMean += cur[((size_t)(cy + yy) * W) + (cx + xx)];
                cMean /= (long)(byf * bxf);
                for (unsigned yy = 0; yy < byf; ++yy)
                    for (unsigned xx = 0; xx < bxf; ++xx) {
                        int d = (int)cur[((size_t)(cy + yy) * W) + (cx + xx)] - (int)cMean;
                        cEnergy += d < 0 ? -d : d;
                    }
                if (cEnergy < (long)(byf * bxf) * 3) {
                    out[((size_t)oy * cw + ox) * 2 + 0] = 0;
                    out[((size_t)oy * cw + ox) * 2 + 1] = 0;
                    continue;
                }
                int bestDx = baseDx, bestDy = baseDy, bestSad = -1;
                for (int dy = -(int)radius; dy <= (int)radius; ++dy) {
                    for (int dx = -(int)radius; dx <= (int)radius; ++dx) {
                        int dx2 = (int)baseDx + dx, dy2 = (int)baseDy + dy;
                        long sad = 0;
                        const unsigned by = H - cy < block ? H - cy : block;
                        const unsigned bx = W - cx < block ? W - cx : block;
                        for (unsigned yy = 0; yy < by; ++yy) {
                            int sy = (int)(cy + yy) + dy2;
                            if (sy < 0 || (unsigned)sy >= H) { sad += 64 * bx; continue; }
                            for (unsigned xx = 0; xx < bx; ++xx) {
                                int sx = (int)(cx + xx) + dx2;
                                if (sx < 0 || (unsigned)sx >= W) { sad += 256; continue; }
                                int d = (int)cur[((size_t)(cy + yy) * W) + (cx + xx)] -
                                        (int)prev[((size_t)sy * W) + sx];
                                sad += d < 0 ? -d : d;
                            }
                        }
                        if (bestSad < 0 || sad < bestSad) {
                            bestSad = (int)sad; bestDx = dx2; bestDy = dy2;
                        }
                    }
                }
                out[((size_t)oy * cw + ox) * 2 + 0] = (short)bestDx;
                out[((size_t)oy * cw + ox) * 2 + 1] = (short)bestDy;
            }
        }
    }

    // 3x3 median of the (dx,dy) fields, per component -- removes speckle
    // without smearing edges (DIS/Farneback standard practice).
    static void median3(std::vector<short> &f, unsigned W, unsigned H) {
        if (W < 3 || H < 3) return;
        std::vector<short> src = f;
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x) {
                size_t i = (size_t)y * W + x;
                for (int c = 0; c < 2; ++c) {
                    short v[9];
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int xx = (int)x + dx, yy = (int)y + dy;
                            if (xx < 0) xx = 0; if (xx >= (int)W) xx = W - 1;
                            if (yy < 0) yy = 0; if (yy >= (int)H) yy = H - 1;
                            v[n++] = src[((size_t)yy * W + xx) * 2 + c];
                        }
                    std::sort(v, v + 9);
                    f[i * 2 + c] = v[4];
                }
            }
    }

    // Upsample the 1/4-scale flow to full resolution, scaling vectors by 4.
    static void upscale4(const std::vector<short> &f, unsigned qw, unsigned qh,
                         unsigned W, unsigned H, std::vector<uint16_t> &out) {
        for (unsigned y = 0; y < H; ++y) {
            float fy = ((float)y + 0.5f) * (float)qh / (float)H - 0.5f;
            int iy = (int)floorf(fy); if (iy < 0) iy = 0; if (iy >= (int)qh - 1) iy = qh - 2;
            float ty = fy - iy;
            for (unsigned x = 0; x < W; ++x) {
                float fx = ((float)x + 0.5f) * (float)qw / (float)W - 0.5f;
                int ix = (int)floorf(fx); if (ix < 0) ix = 0; if (ix >= (int)qw - 1) ix = qw - 2;
                float tx = fx - ix;
                size_t i00 = ((size_t)iy * qw + ix) * 2;
                size_t i10 = i00 + 2, i01 = ((size_t)(iy + 1) * qw + ix) * 2, i11 = i01 + 2;
                float vx = (f[i00] * (1 - tx) + f[i10] * tx) * (1 - ty) +
                           (f[i01] * (1 - tx) + f[i11] * tx) * ty;
                float vy = (f[i00 + 1] * (1 - tx) + f[i10 + 1] * tx) * (1 - ty) +
                           (f[i01 + 1] * (1 - tx) + f[i11 + 1] * tx) * ty;
                out[((size_t)y * W + x) * 2 + 0] = enhancer::float_to_half(vx * 4.0f);
                out[((size_t)y * W + x) * 2 + 1] = enhancer::float_to_half(vy * 4.0f);
            }
        }
    }

    // Estimates motion for the current frame vs the previous one. On `reset`
    // (first frame or scene cut) the output is all zeros and history restarts.
    void compute(const unsigned char *rgb48, unsigned W, unsigned H, bool reset,
                 std::vector<uint16_t> &outMotion) {
        outMotion.assign((size_t)W * H * 2, 0);
        if (reset) { this->reset(); return; }   // `reset` is shadowed by the bool param
        if (!has_prev) {
            // Start of a history: remember the current frame, no motion yet.
            const unsigned S = 4;
            qw = (W + S - 1) / S; qh = (H + S - 1) / S;
            luma_scale(rgb48, W, H, S, prevQ);
            const unsigned S8 = 8;
            ew = (W + S8 - 1) / S8; eh = (H + S8 - 1) / S8;
            luma_scale(rgb48, W, H, S8, prevE);
            has_prev = true;
            return;
        }
        const unsigned S = 4;                       // compute at 1/4
        qw = (W + S - 1) / S; qh = (H + S - 1) / S;
        const unsigned S8 = 8;
        ew = (W + S8 - 1) / S8; eh = (H + S8 - 1) / S8;
        std::vector<unsigned char> curQ, curE;
        luma_scale(rgb48, W, H, S, curQ);
        luma_scale(rgb48, W, H, S8, curE);
        if (prevQ.size() != (size_t)qw * qh) prevQ.assign((size_t)qw * qh, 0);
        if (prevE.size() != (size_t)ew * eh) prevE.assign((size_t)ew * eh, 0);

        // level 1/8: init from zero, radius 4, block 8, step 4
        std::vector<short> fE;
        match(prevE, curE, ew, eh, 4, 4, 8, std::vector<short>(), false, fE);
        median3(fE, (ew + 3) / 4, (eh + 3) / 4);
        // level 1/4: init from the 1/8 flow. Cell correspondence: a 1/4 cell
        // (ox,oy) sits on 8x8 *input* pixels, i.e. two 1/8 cells; sample the
        // 1/8-flow grid at (ox/2, oy/2), scaled 2x. The 1/8 flow grid has
        // cw8 x ch8 cells -- NOT ew x eh pixels (mixing the two overflowed
        // the fE vector and corrupted the heap).
        const unsigned cw8 = (ew + 3) / 4, ch8 = (eh + 3) / 4;
        std::vector<short> initQ((size_t)((qw + 3) / 4) * ((qh + 3) / 4) * 2, 0);
        for (size_t i = 0; i + 1 < initQ.size(); i += 2) {
            unsigned oc = i / 2;
            unsigned ox = oc % ((qw + 3) / 4), oy = oc / ((qw + 3) / 4);
            unsigned fx = ox / 2; if (fx >= cw8) fx = cw8 - 1;
            unsigned fy = oy / 2; if (fy >= ch8) fy = ch8 - 1;
            initQ[i] = (short)(fE[((size_t)fy * cw8 + fx) * 2] * 2);
            initQ[i + 1] = (short)(fE[((size_t)fy * cw8 + fx) * 2 + 1] * 2);
        }
        std::vector<short> fQ;
        match(prevQ, curQ, qw, qh, 4, 4, 8, initQ, true, fQ);
        median3(fQ, (qw + 3) / 4, (qh + 3) / 4);
        upscale4(fQ, (qw + 3) / 4, (qh + 3) / 4, W, H, outMotion);

        // The block matcher is deliberately conservative, but ties on fine
        // texture still move by a pixel from frame to frame. Smooth the field
        // in time and reject implausible jumps; this keeps DLSS reprojection
        // from turning tiny flow noise into visible shimmer.
        if (previousMotion.size() == outMotion.size()) {
            for (size_t i = 0; i + 1 < outMotion.size(); i += 2) {
                float cx = enhancer::half_to_float(outMotion[i]);
                float cy = enhancer::half_to_float(outMotion[i + 1]);
                float px = enhancer::half_to_float(previousMotion[i]);
                float py = enhancer::half_to_float(previousMotion[i + 1]);
                float dx = cx - px, dy = cy - py;
                const float jump = sqrtf(dx * dx + dy * dy);
                if (!std::isfinite(cx) || !std::isfinite(cy) || jump > 24.0f) {
                    cx = px;
                    cy = py;
                } else {
                    const float current_weight = (fabsf(cx) + fabsf(cy)) >= 8.0f ? 0.88f : 0.70f;
                    cx = px * (1.0f - current_weight) + cx * current_weight;
                    cy = py * (1.0f - current_weight) + cy * current_weight;
                }
                outMotion[i] = enhancer::float_to_half(cx);
                outMotion[i + 1] = enhancer::float_to_half(cy);
            }
        }
        previousMotion = outMotion;

        prevQ.swap(curQ);
        prevE.swap(curE);
        has_prev = true;
    }
};

// GPU motion guide. The decoder still supplies CPU frames, but the expensive
// quarter-resolution block matching runs as a D3D12 compute dispatch. This is
// intentionally a conservative fallback path: if device/shader creation or
// a dispatch fails, the caller keeps using CpuFlow rather than losing output.
struct GpuFlow {
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *cmd = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;
    ID3D12RootSignature *root = nullptr;
    ID3D12PipelineState *pso = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    ID3D12Resource *prev = nullptr, *cur = nullptr, *out = nullptr, *full_out = nullptr, *history = nullptr, *readback = nullptr, *cb = nullptr;
    unsigned fw = 0, fh = 0, qw = 0, qh = 0;
    std::vector<unsigned> prev_luma;
    bool ready = false, has_prev = false, full_copy_ready = false;

    template<class T> static void rel(T *&p) { if (p) { p->Release(); p = nullptr; } }
    ~GpuFlow() { stop(); }
    void reset() { has_prev = false; prev_luma.clear(); }
    ID3D12Resource *gpu_motion() const { return full_out; }
    unsigned gpu_motion_pitch() const { return ((fw * 4u + 255u) & ~255u); }
    void stop() {
        if (queue && fence && fence_value) { queue->Signal(fence, ++fence_value); }
        if (fence && fence_event && fence->GetCompletedValue() < fence_value) {
            fence->SetEventOnCompletion(fence_value, fence_event); WaitForSingleObject(fence_event, 3000);
        }
        rel(prev); rel(cur); rel(out); rel(full_out); rel(history); rel(readback); rel(cb); rel(heap); rel(pso); rel(root);
        rel(cmd); rel(allocator); rel(fence); rel(queue); rel(device);
        if (fence_event) { CloseHandle(fence_event); fence_event = nullptr; }
        ready = false; has_prev = false; full_copy_ready = false; prev_luma.clear();
    }
    bool init(unsigned W, unsigned H, ID3D12Device *external_device = nullptr,
              ID3D12CommandQueue *external_queue = nullptr) {
        stop(); fw = W; fh = H; qw = (W + 3) / 4; qh = (H + 3) / 4;
        IDXGIAdapter1 *adapter = nullptr; IDXGIFactory6 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        if (external_device) {
            device = external_device; device->AddRef();
            if (external_queue) { queue = external_queue; queue->AddRef(); }
        } else for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) break;
            rel(adapter);
        }
        if (!device) { rel(adapter); factory->Release(); return false; }
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if ((!queue && FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator, nullptr, IID_PPV_ARGS(&cmd))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { rel(adapter); factory->Release(); stop(); return false; }
        cmd->Close(); fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const char *src = R"(
cbuffer Params : register(b0) { uint W; uint H; uint FW; uint FH; };
StructuredBuffer<uint> Prev : register(t0); StructuredBuffer<uint> Cur : register(t1);
StructuredBuffer<uint> Hist : register(t2); RWStructuredBuffer<int2> Flow : register(u0); RWStructuredBuffer<uint> Full : register(u1);
uint sample(ByteAddressBuffer b, uint i) { return b.Load(i * 4); }
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= W || id.y >= H) return; uint x=id.x, y=id.y; int bestX=0,bestY=0,best=0x7fffffff;
  for(int dy=-4;dy<=4;dy++) for(int dx=-4;dx<=4;dx++) { int sad=0;
    for(uint by=0;by<4;by++) for(uint bx=0;bx<4;bx++) { int sx=int(x+bx)+dx, sy=int(y+by)+dy;
      uint a=Cur[(min(y+by,H-1))*W+min(x+bx,W-1)]; uint b=0;
      if(sx>=0 && sy>=0 && sx<int(W) && sy<int(H)) b=Prev[uint(sy)*W+uint(sx)];
      sad += abs(int(a)-int(b)); }
    if(sad<best) { best=sad; bestX=dx; bestY=dy; } }
  Flow[y*W+x]=int2(bestX,bestY);
  uint hx = f32tof16(float(bestX * 4)); uint hy = f32tof16(float(bestY * 4));
    uint old = Hist[py*FW+px]; float ox=f16tof32(old & 0xffff), oy=f16tof32(old >> 16);
    float motion_mag = abs(float(bestX)) + abs(float(bestY));
    float current_weight = motion_mag >= 2.0 ? 0.88 : 0.70;
    float sx = ox * (1.0 - current_weight) + float(bestX * 4) * current_weight;
    float sy = oy * (1.0 - current_weight) + float(bestY * 4) * current_weight;
    uint packed = (f32tof16(sx) & 0xffff) | ((f32tof16(sy) & 0xffff) << 16);
  for(uint by=0; by<4 && y*4+by < FH; ++by) for(uint bx=0; bx<4 && x*4+bx < FW; ++bx) {
    uint px=min(x*4+bx, FW-1), py=min(y*4+by, FH-1);
    Full[py*FW+px]=packed;
  }
}
)";
        ID3DBlob *cs = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(src, strlen(src), "gpu_flow", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) { if(err) err->Release(); rel(adapter); factory->Release(); stop(); return false; }
        D3D12_DESCRIPTOR_RANGE ranges[2]{}; ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors=3; ranges[0].BaseShaderRegister=0; ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors=2; ranges[1].BaseShaderRegister=0;
        D3D12_ROOT_PARAMETER rp[3]{}; rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues=4; rp[0].Constants.ShaderRegister=0; rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges=1; rp[1].DescriptorTable.pDescriptorRanges=&ranges[0]; rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[2].DescriptorTable.NumDescriptorRanges=1; rp[2].DescriptorTable.pDescriptorRanges=&ranges[1];
        D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters=3; rsd.pParameters=rp; rsd.Flags=D3D12_ROOT_SIGNATURE_FLAG_NONE; ID3DBlob *sig=nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rsd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err)) || FAILED(device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root)))) { if(err) err->Release(); if(sig) sig->Release(); cs->Release(); rel(adapter); factory->Release(); stop(); return false; }
        sig->Release(); D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root; pd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
        bool ok=SUCCEEDED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso))); cs->Release(); if(!ok) { rel(adapter); factory->Release(); stop(); return false; }
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=5; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)))) { rel(adapter); factory->Release(); stop(); return false; }
        rel(adapter); factory->Release(); ready=true; return resize();
    }
    bool resize() {
        rel(prev); rel(cur); rel(out); rel(readback); rel(cb);
        auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE ht,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES st,ID3D12Resource **r){ D3D12_HEAP_PROPERTIES hp{}; hp.Type=ht; D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=bytes; d.Height=1; d.DepthOrArraySize=1; d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags=flags; return device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,st,nullptr,IID_PPV_ARGS(r)); };
        UINT64 n=(UINT64)qw*qh*4; if(FAILED(buffer(n,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,&prev)) || FAILED(buffer(n,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,&cur)) || FAILED(buffer((UINT64)qw*qh*8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,&out)) || FAILED(buffer((UINT64)fw*fh*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,&full_out)) || FAILED(buffer((UINT64)fw*fh*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,&history)) || FAILED(buffer((UINT64)qw*qh*8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,&readback)) || FAILED(buffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,&cb))) return false;
        return true;
    }
    bool compute(const std::vector<unsigned> &cur_luma, std::vector<short> &flow) {
        if(!ready || cur_luma.size()!=(size_t)qw*qh) return false;
        if(!has_prev) { prev_luma=cur_luma; has_prev=true; flow.assign((size_t)qw*qh*2,0); return true; }
        unsigned *p=nullptr; D3D12_RANGE z{0,0}; prev->Map(0,&z,(void**)&p); memcpy(p,prev_luma.data(),prev_luma.size()*4); prev->Unmap(0,nullptr); cur->Map(0,&z,(void**)&p); memcpy(p,cur_luma.data(),cur_luma.size()*4); cur->Unmap(0,nullptr);
        unsigned *c=nullptr; cb->Map(0,&z,(void**)&c); c[0]=qw;c[1]=qh;c[2]=fw;c[3]=fh;cb->Unmap(0,nullptr);
        auto cpu=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); auto base=heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; sv.Format=DXGI_FORMAT_UNKNOWN; sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Buffer.NumElements=qw*qh; sv.Buffer.StructureByteStride=4; device->CreateShaderResourceView(prev,&sv,{base.ptr}); device->CreateShaderResourceView(cur,&sv,{base.ptr+cpu}); D3D12_SHADER_RESOURCE_VIEW_DESC hv=sv; hv.Buffer.NumElements=fw*fh; device->CreateShaderResourceView(history,&hv,{base.ptr+cpu*2});
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; uv.Format=DXGI_FORMAT_UNKNOWN; uv.Buffer.NumElements=qw*qh; uv.Buffer.StructureByteStride=8; device->CreateUnorderedAccessView(out,nullptr,&uv,{base.ptr+cpu*2});
        D3D12_UNORDERED_ACCESS_VIEW_DESC fv{}; fv.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; fv.Format=DXGI_FORMAT_UNKNOWN; fv.Buffer.NumElements=fw*fh; fv.Buffer.StructureByteStride=4; device->CreateUnorderedAccessView(full_out,nullptr,&fv,{base.ptr+cpu*3});
        allocator->Reset(); cmd->Reset(allocator,pso); ID3D12DescriptorHeap *hs[]={heap}; cmd->SetDescriptorHeaps(1,hs); cmd->SetComputeRootSignature(root); cmd->SetComputeRoot32BitConstants(0,4,c,0); auto gpu=heap->GetGPUDescriptorHandleForHeapStart(); cmd->SetComputeRootDescriptorTable(1,gpu); cmd->SetComputeRootDescriptorTable(2,{gpu.ptr+cpu*2}); cmd->Dispatch((qw+7)/8,(qh+7)/8,1); D3D12_RESOURCE_BARRIER b[2]{}; b[0].Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; b[0].UAV.pResource=out; b[1].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[1].Transition.pResource=full_out; b[1].Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE; b[1].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; cmd->ResourceBarrier(2,b); cmd->CopyResource(readback,out); cmd->Close(); ID3D12CommandList *ls[]={cmd}; queue->ExecuteCommandLists(1,ls); queue->Signal(fence,++fence_value); if(fence->GetCompletedValue()<fence_value){fence->SetEventOnCompletion(fence_value,fence_event);WaitForSingleObject(fence_event,5000);}
        allocator->Reset(); cmd->Reset(allocator,nullptr); D3D12_RESOURCE_BARRIER hb[2]{}; hb[0].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; hb[0].Transition.pResource=history; hb[0].Transition.StateBefore=D3D12_RESOURCE_STATE_GENERIC_READ; hb[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST; hb[0].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; hb[1].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; hb[1].Transition.pResource=full_out; hb[1].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE; hb[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE; hb[1].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; cmd->ResourceBarrier(1,&hb[0]); cmd->CopyResource(history,full_out); std::swap(hb[0].Transition.StateBefore,hb[0].Transition.StateAfter); cmd->ResourceBarrier(1,&hb[0]); cmd->Close(); ID3D12CommandList *hls[]={cmd}; queue->ExecuteCommandLists(1,hls); queue->Signal(fence,++fence_value); if(fence->GetCompletedValue()<fence_value){fence->SetEventOnCompletion(fence_value,fence_event);WaitForSingleObject(fence_event,5000);}
        int *r=nullptr; D3D12_RANGE rr{0,(SIZE_T)qw*qh*8}; if(FAILED(readback->Map(0,&rr,(void**)&r))) return false; flow.resize((size_t)qw*qh*2); for(size_t i=0;i<(size_t)qw*qh;i++){flow[i*2]=(short)r[i*2];flow[i*2+1]=(short)r[i*2+1];} readback->Unmap(0,nullptr); prev_luma=cur_luma; return true;
    }
};
bool output_is_blank(const enhancer::Image &); // defined below the mode

bool load_image(IWICImagingFactory *wic, const std::wstring &path, enhancer::Image &image) {
    IWICBitmapDecoder *decoder = nullptr;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
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

int run_image_mode(int argc, char **argv) {
    // argv: --image <in> <out> <snippet> <driver> [runtime] [nvapi] [options]
    if (argc < 6) {
        fprintf(stderr,
                "usage: video_filter --image <input.png> <output.png> <snippet> <driver> "
                "[runtime] [nvapi] [--passes N] [--retries N] [--intensity F] [--global-tone F] "
                "[--local-tone F] [--local-structure F] [--skin-structure F] [--style N] "
                "[--preset N] [--no-auto-mask]\n");
        return 2;
    }
    enhancer::Settings settings;
    settings.passes = 3; // stills settle over a few evaluations
    int retries = 3;     // blank-output races are re-evaluated this many times
    enhancer::Paths paths;
    paths.snippet = widen(argv[4]);
    paths.cuda_driver = widen(argv[5]);
    paths.ngx_runtime = argc > 6 ? widen(argv[6]) : L"nvngx.dll";
    paths.nvapi = argc > 7 ? widen(argv[7]) : L"nvapi64.dll";
    for (int i = 8; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", name); return nullptr; }
            return argv[++i];
        };
        if (arg == "--passes") { const char *v = need("--passes"); if (!v) return 2; settings.passes = atoi(v); }
        else if (arg == "--retries") { const char *v = need("--retries"); if (!v) return 2; retries = atoi(v); if (retries < 0) retries = 0; }
        else if (arg == "--gamma") { const char *v = need("--gamma"); if (!v) return 2; g_gamma = atof(v); if (g_gamma < 0.1) g_gamma = 0.1; }
        else if (arg == "--intensity") { const char *v = need("--intensity"); if (!v) return 2; settings.intensity = (float)atof(v); }
        else if (arg == "--global-tone") { const char *v = need("--global-tone"); if (!v) return 2; settings.global_tone = (float)atof(v); }
        else if (arg == "--local-tone") { const char *v = need("--local-tone"); if (!v) return 2; settings.local_tone = (float)atof(v); }
        else if (arg == "--local-structure") { const char *v = need("--local-structure"); if (!v) return 2; settings.local_structure = (float)atof(v); }
        else if (arg == "--skin-structure") { const char *v = need("--skin-structure"); if (!v) return 2; settings.skin_structure = (float)atof(v); }
        else if (arg == "--style") { const char *v = need("--style"); if (!v) return 2; settings.style = atoi(v); }
        else if (arg == "--preset") { const char *v = need("--preset"); if (!v) return 2; settings.preset = atoi(v); }
        else if (arg == "--dlss-model-preset") { const char *v = need("--dlss-model-preset"); if (!v) return 2; SetEnvironmentVariableA("DLSS_PRESET", v); }
        else if (arg == "--no-auto-mask") { settings.auto_mask = false; }
        else if (arg == "--precompile-wait" || arg == "--retry-delay") {
            if (arg == "--retry-delay") need("--retry-delay"); // 外壳标志，忽略
        }
        else { fprintf(stderr, "unknown option: %s\n", arg.c_str()); return 2; }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
        fprintf(stderr, "[FAIL] WIC unavailable\n");
        return 1;
    }
    enhancer::Image in;
    if (!load_image(wic, widen(argv[2]), in)) {
        fprintf(stderr, "[FAIL] could not read %s\n", argv[2]);
        wic->Release();
        return 1;
    }
    fprintf(stderr, "[info] image %ux%u, %d passes\n", in.width, in.height, settings.passes);

    enhancer::Processor processor;
    std::string error;
    if (!processor.start(paths, error, [](const std::string &message) {
            fprintf(stderr, "[precompile] %s\n", message.c_str());
            fflush(stderr);
        })) {
        fprintf(stderr, "[FAIL] start: %s\n", error.c_str());
        wic->Release();
        return 1;
    }
    enhancer::Image out;
    // The blank race is sticky within a process (all evaluations of a bad
    // process come out black), so there is no point re-evaluating here: a
    // blank result makes the whole run report exit code 2, and the outer
    // main() re-executes this program with a fresh process instead.
    settings.reset_accumulation = true;
    if (!processor.process(in, out, settings, error)) {
        fprintf(stderr, "[FAIL] process: %s\n", error.c_str());
        processor.stop();
        wic->Release();
        return 1;
    }
    const bool blank = output_is_blank(out);
    if (blank)
        fprintf(stderr, "[warn] 输出空白：本轮求值竞态，交给外层重跑\n");
    if (!dump_png(wic, widen(argv[3]), out)) {
        fprintf(stderr, "[FAIL] could not write %s\n", argv[3]);
        processor.stop();
        wic->Release();
        return 1;
    }
    fprintf(stderr, "[done] wrote %s in %d passes, %.0f ms%s\n", argv[3], settings.passes,
            processor.last_ms(), blank ? " -- WARNING: output was blank" : "");
    processor.stop();
    wic->Release();
    return blank ? 2 : 0;
}

// --------------------------------------------------------------------------
// Blank-output detection. The ZLUDA-backed network has a known first-shot
// race (the author's own README: intermittent blank pictures, "try re-opening
// the program"); empirically a re-evaluation flips the result to a good one.
// --------------------------------------------------------------------------

// True when the frame looks blank: luma mean near zero and almost no variance.
// Samples every 8th pixel, so a 4K frame costs a few thousand half->float
// conversions only.
bool output_is_blank(const enhancer::Image &image) {
    const uint16_t *src = image.pixels.data();
    const size_t count = (size_t)image.width * image.height;
    const size_t step = 8;
    double sum = 0.0, sum_sq = 0.0;
    size_t samples = 0;
    for (size_t i = 0; i < count; i += step) {
        const float r = enhancer::half_to_float(src[i * 4 + 0]);
        const float g = enhancer::half_to_float(src[i * 4 + 1]);
        const float b = enhancer::half_to_float(src[i * 4 + 2]);
        const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        sum += luma;
        sum_sq += luma * luma;
        ++samples;
    }
    if (samples == 0) return true;
    const double mean = sum / (double)samples;
    const double variance = sum_sq / (double)samples - mean * mean;
    return mean < 0.05 && variance < 0.01;
}

} // namespace

// One full run of the tool; returns 0 ok, 1 failure, 2 retryable (blank race).
static int run_main_once(int argc, char **argv) {
    // The parallel translation spawns copies of this program.
    if (argc == 4 && !strcmp(argv[1], "--compile-one"))
        return enhancer::compile_one(widen(argv[2]), widen(argv[3]));
    if (argc >= 4 && !strcmp(argv[1], "--precompile")) {
        std::string error;
        const bool ok = enhancer::precompile(
            widen(argv[2]), widen(argv[3]), argc > 4 ? (unsigned)atoi(argv[4]) : 0u,
            [](const enhancer::Progress &progress) {
                fprintf(stderr, "[precompile] %s\n", progress.message.c_str());
                fflush(stderr);
            },
            error);
        if (!ok) fprintf(stderr, "%s\n", error.c_str());
        return ok ? 0 : 1;
    }

    kill_children_when_this_process_ends();

    // Still-image mode: one picture through the network, multi-pass settle.
    if (argc >= 2 && !strcmp(argv[1], "--image"))
        return run_image_mode(argc, argv);

    Options options;
    if (!parse_args(argc, argv, options)) return 2;
    g_gamma = options.gamma;

    // --- probe the source -------------------------------------------------
    VideoParams params;
    std::string error;
    if (!probe_video(options.input, params, error)) {
        fprintf(stderr, "[FAIL] %s\n", error.c_str());
        return 1;
    }
    if (options.fps_override > 0.0) params.fps = options.fps_override;
    fprintf(stderr, "[info] %ux%u %.3f fps\n", params.width, params.height, params.fps);
    const unsigned model_w = params.width;
    const unsigned model_h = params.height;
    unsigned output_w = (std::max)(2u, (unsigned)std::lround(params.width * options.upscale));
    unsigned output_h = (std::max)(2u, (unsigned)std::lround(params.height * options.upscale));
    output_w &= ~1u;
    output_h &= ~1u;
    if (options.upscale != 1.0)
        fprintf(stderr, "[upscale] ratio=%.3f output=%ux%u\n", options.upscale, output_w, output_h);

    // --- start ffmpeg decode + encode sides ------------------------------
    // Decoder: the child writes its raw frames to the pipe, so the write end
    // is the inherited one; we only read.
    Pipe decoder_stdout;
    if (!decoder_stdout.make(false, true)) { fprintf(stderr, "[FAIL] pipe\n"); return 1; }
    ChildProcess decoder;
    if (!start_decoder(options.input, decoder_stdout, decoder)) {
        fprintf(stderr, "[FAIL] decoder: %s\n", error.c_str());
        decoder_stdout.close();
        return 1;
    }

    // --flow-only diagnostic: decode and run the motion estimator without the
    // network (blank-race immune), printing per-frame flow statistics.
    {
        bool flow_only = false;
        for (int i = 7; i < argc; ++i)
            if (!strcmp(argv[i], "--flow-only")) flow_only = true;
        if (flow_only) {
            CpuFlow fg;
            const size_t fbytes = (size_t)params.width * params.height * 6;
            std::vector<unsigned char> frame(fbytes);
            std::vector<uint16_t> mv;
            unsigned fi = 0;
            while (!options.max_frames || (int)fi < options.max_frames) {
                size_t got = 0;
                while (got < fbytes) {
                    DWORD chunk = 0;
                    if (!ReadFile(decoder_stdout.read_end, frame.data() + got,
                                  (DWORD)(fbytes - got), &chunk, nullptr) || chunk == 0)
                        break;
                    got += chunk;
                }
                if (got != fbytes) break;
                fg.compute(frame.data(), params.width, params.height, fi == 0, mv);
                double sum = 0.0;
                float mx = 0.0f;
                size_t nz = 0;
                for (size_t i = 0; i + 1 < mv.size(); i += 2) {
                    float a = fabsf(enhancer::half_to_float(mv[i]));
                    float b = fabsf(enhancer::half_to_float(mv[i + 1]));
                    if (a > mx) mx = a;
                    if (b > mx) mx = b;
                    if (a + b > 0.1f) ++nz;
                    sum += a + b;
                }
                fprintf(stderr, "[flow] frame %u mean|mv|=%.3f max=%.1f nonzero_px=%.1f%%\n", fi,
                        mv.empty() ? 0.0f : (float)(sum / (double)(mv.size() / 2)), mx,
                        100.0 * (double)nz / (double)(mv.size() / 2));
                ++fi;
            }
            decoder_stdout.close();
            DWORD code = 1;
            wait_exit(decoder.process, 15000, code);
            decoder.close();
            return 0;
        }
    }

    // Encoder: the child reads its raw frames from the pipe, so the read end
    // is the inherited one; we only write.
    Pipe encoder_stdin;
    if (!encoder_stdin.make(true, false)) { fprintf(stderr, "[FAIL] pipe\n"); return 1; }
    ChildProcess encoder;
    if (!start_encoder(options.input, options.output, params, options.crf, options.audio,
                       encoder_stdin, encoder, output_w, output_h)) {
        fprintf(stderr, "[FAIL] encoder could not be started (invalid output path or ffmpeg)\n");
        encoder_stdin.close();
        return 1;
    }

    // --- the DLSS side ----------------------------------------------------
    enhancer::Paths paths;
    paths.snippet = options.snippet;
    paths.cuda_driver = options.driver;
    paths.ngx_runtime = options.runtime;
    paths.nvapi = options.nvapi;
    if (!options.dlss_model_preset.empty() && options.dlss_model_preset != "default")
        SetEnvironmentVariableA("DLSS_PRESET", options.dlss_model_preset.c_str());
    else
        SetEnvironmentVariableA("DLSS_PRESET", nullptr);

    enhancer::Processor processor;
    if (!processor.start(paths, error, [&](const std::string &message) {
            fprintf(stderr, "[precompile] %s\n", message.c_str());
            fflush(stderr);
        })) {
        fprintf(stderr, "[FAIL] start: %s\n", error.c_str());
        decoder.close();
        encoder.close();
        return 1;
    }

    IWICImagingFactory *wic = nullptr;
    bool dump_ok = options.dump_dir.empty();
    if (!options.dump_dir.empty()) {
        CreateDirectoryW(options.dump_dir.c_str(), nullptr); // may already exist; ignore
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&wic))))
            dump_ok = true;
        else
            fprintf(stderr, "[warn] WIC unavailable, --dump-frames disabled\n");
    }

    // --- the frame loop ---------------------------------------------------
    SceneDetector detector;
    detector.configure(params.width, params.height, options.cut_threshold);
    CpuFlow flowgen; // motion-vector estimator for temporal guidance
    GpuFlow gpuflow; // D3D12 compute estimator; CPU remains the fallback
    bool gpu_flow_ready = false;
    if (options.flow) {
        // Keep the flow command queue independent until cross-queue fence
        // handoff is fully validated; this is the stable GPU path.
        gpu_flow_ready = gpuflow.init(params.width, params.height,
                                      processor.native_device(), nullptr);
        fprintf(stderr, "[flow] %s\n", gpu_flow_ready ?
                "D3D12 GPU motion guide enabled (quarter-resolution compute)" :
                "D3D12 GPU motion guide unavailable; using CPU fallback");
    }

    const size_t frame_bytes = (size_t)params.width * params.height * 6;
    const size_t model_bytes = (size_t)model_w * model_h * 6;
    std::vector<unsigned char> input_frame(frame_bytes);
    std::vector<unsigned char> model_frame(model_bytes);
    std::vector<unsigned char> output_frame(frame_bytes);
    std::vector<unsigned char> model_output(model_bytes);
    enhancer::Image in, out;
    in.width = model_w;
    in.height = model_h;
    in.pixels.resize((size_t)model_w * model_h * 4);
    enhancer::Settings settings = options.settings;
    settings.output_width = params.width;
    settings.output_height = params.height;

    double elapsed_total = 0.0;
    int reset_count = 0;
    int blank_count = 0;
    unsigned frame_index = 0;
    bool failed = false;
    bool retryable = false; // a blank first frame: the whole run is a loss,
                            // report exit code 2 so outer main() re-runs it

    while (true) {
        // Read one frame from the decoder; EOF on a frame boundary = the end.
        size_t got = 0;
        while (got < frame_bytes) {
            DWORD chunk = 0;
            if (!ReadFile(decoder_stdout.read_end, input_frame.data() + got,
                          (DWORD)(frame_bytes - got), &chunk, nullptr) || chunk == 0) {
                break;
            }
            got += chunk;
        }
        if (got == 0) break; // clean end of video
        if (got != frame_bytes) {
            fprintf(stderr, "[FAIL] short read at frame %u (%zu of %zu bytes)\n", frame_index,
                    got, frame_bytes);
            failed = true;
            break;
        }
        if (options.max_frames && (int)frame_index >= options.max_frames) break;

        // Decide whether this frame resets the network's accumulation history.
        bool reset = false;
        switch (options.reset) {
            case ResetMode::Always: reset = true; break;
            case ResetMode::Never: reset = frame_index == 0; break;
            case ResetMode::Every: reset = (frame_index % options.reset_every) == 0; break;
            case ResetMode::Auto: reset = detector.consider(input_frame.data(), params.width,
                                                            params.height);
        }
        if (reset) ++reset_count;
        settings.reset_accumulation = reset;

        memcpy(model_frame.data(), input_frame.data(), frame_bytes);
        rgb48_to_half_rgba(model_frame.data(), in);
        const auto began = std::chrono::steady_clock::now();

        // Optional motion-vector guidance: backward flow (where each pixel was
        // in the previous frame, in pixels). Zeroed on reset frames.
        enhancer::Image motion;
        ID3D12Resource *gpu_motion = nullptr;
        unsigned gpu_motion_pitch = 0;
    const bool useFlow = options.flow;
        if (useFlow) {
            motion.width = model_w;
            motion.height = model_h;
            motion.pixels.resize((size_t)model_w * model_h * 2);
            bool generated = false;
            if (gpu_flow_ready) {
                if (reset) gpuflow.reset();
                const unsigned qw = (model_w + 3) / 4, qh = (model_h + 3) / 4;
                std::vector<unsigned> luma((size_t)qw * qh);
                for (unsigned y = 0; y < qh; ++y) for (unsigned x = 0; x < qw; ++x) {
                    unsigned px = std::min(model_w - 1, x * 4u);
                    unsigned py = std::min(model_h - 1, y * 4u);
                    const uint16_t *p = (const uint16_t *)(model_frame.data() + ((size_t)py * model_w + px) * 6);
                    luma[(size_t)y * qw + x] = ((unsigned)p[0] * 19595u + (unsigned)p[1] * 38470u + (unsigned)p[2] * 7471u) >> 16;
                }
                std::vector<short> qflow;
                generated = gpuflow.compute(luma, qflow);
                if (generated) {
                    CpuFlow::median3(qflow, qw, qh);
                    CpuFlow::upscale4(qflow, qw, qh, model_w, model_h, motion.pixels);
                    // Extreme neural controls amplify tiny vector errors. Keep
                    // the temporally smoothed CPU field for that regime; the
                    // direct GPU buffer remains the fast path for normal video
                    // settings until GPU history filtering is enabled.
                    const bool high_temporal_risk =
                        settings.intensity > 1.0f || settings.global_tone > 1.0f ||
                        settings.local_tone > 1.0f || settings.local_structure > 1.0f ||
                        settings.skin_structure > 1.0f || settings.passes > 1;
                    if (!reset && !high_temporal_risk && gpuflow.gpu_motion()) {
                        gpu_motion = gpuflow.gpu_motion();
                        gpu_motion_pitch = gpuflow.gpu_motion_pitch();
                    }
                }
            }
            if (!generated) flowgen.compute(model_frame.data(), model_w, model_h, reset, motion.pixels);
            double msum = 0.0;
            size_t mcnt = motion.pixels.size() / 2;
            const uint16_t *mp = motion.pixels.data();
            size_t nf = 0;
            float mn = 0.0f, mx = 0.0f;
            for (size_t i = 0; i + 1 < motion.pixels.size(); i += 2) {
                float dx = enhancer::half_to_float(mp[i]);
                float dy = enhancer::half_to_float(mp[i + 1]);
                if (!std::isfinite(dx) || !std::isfinite(dy)) {
                ++nf;
                if (nf == 1)
                    fprintf(stderr, "[flow-debug] first nonfinite at px %zu/%zu half=0x%04X,0x%04X\n",
                            (i / 2) % params.width, (i / 2) / params.width, mp[i], mp[i + 1]);
            }
                msum += fabsf(dx) + fabsf(dy);
                float a = fabsf(dx), b = fabsf(dy);
                if (a > mx) mx = a; if (b > mx) mx = b;
                if (i == 0) { mn = a < b ? a : b; }
                if (mn > (a < b ? a : b)) mn = a < b ? a : b;
            }
            fprintf(stderr, "[flow] frame %u mean|mv|=%.3f px min=%.2f max=%.2f nonfinite=%zu\n",
                    frame_index, mcnt ? (float)(msum / (double)mcnt) : 0.0f, mn, mx, nf);
            fflush(stderr);
        }

        if (!processor.process(in, out, settings, error,
                               gpu_motion ? nullptr : (useFlow ? &motion : nullptr),
                               gpu_motion, gpu_motion_pitch)) {
            fprintf(stderr, "[FAIL] frame %u: %s\n", frame_index, error.c_str());
            if (error.find("blank image") != std::string::npos) {
                // Processor-level validation catches the race before the
                // frame reaches the encoder; keep this retryable so main()
                // can re-execute the whole pipeline in a fresh process.
                retryable = true;
            }
            failed = true;
            break;
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - began)
                              .count();
        elapsed_total += ms;

        // The blank race is sticky per process: if the first frame came out
        // black every later frame will too, so stop right here and let the
        // outer main() re-run the whole thing in a fresh process (~90% of
        // re-runs recover).
        if (frame_index == 0 && output_is_blank(out)) {
            fprintf(stderr, "[FAIL] 首帧输出空白（本轮竞态），交由外层重跑\n");
            failed = true;
            retryable = true;
            break;
        }

        bool blank = false;
        half_rgba_to_rgb48(out, model_output.data(), blank);
        if (out.width != params.width || out.height != params.height) {
            fprintf(stderr, "[FAIL] frame %u: unexpected output size %ux%u (wanted %ux%u)\n", frame_index, out.width, out.height, params.width, params.height);
            failed = true;
            break;
        }
        memcpy(output_frame.data(), model_output.data(), frame_bytes);
        if (blank) {
            ++blank_count;
            fprintf(stderr, "[warn] frame %u output looks blank\n", frame_index);
        }

        // Feed the encoder; a dead pipe means it gave up (bad args, disk full).
        DWORD written = 0;
        if (!WriteFile(encoder_stdin.write_end, output_frame.data(), (DWORD)frame_bytes,
                       &written, nullptr) || written != frame_bytes) {
            fprintf(stderr, "[FAIL] encoder pipe broke at frame %u\n", frame_index);
            failed = true;
            break;
        }

        if (!options.dump_dir.empty() && dump_ok) {
            wchar_t name[512];
            swprintf(name, 512, L"%s\\frame_%05u.png", options.dump_dir.c_str(), frame_index);
            if (!dump_png(wic, name, out))
                fprintf(stderr, "[warn] could not dump frame %u\n", frame_index);
        }

        if ((frame_index % 5) == 0 || (frame_index < 5)) {
            fprintf(stderr, "[%.5u] %.0f ms (avg %.1f, reset=%d, blanks=%d)\n", frame_index, ms,
                    elapsed_total / (frame_index + 1), reset_count, blank_count);
            fflush(stderr);
        }
        ++frame_index;
    }

    // --- finish the pipes ------------------------------------------------
    decoder_stdout.close();
    encoder_stdin.close();

    DWORD decoder_code = 1, encoder_code = 1;
    wait_exit(decoder.process, 15000, decoder_code);
    wait_exit(encoder.process, 60000, encoder_code);

    if (wic) wic->Release();
    processor.stop();
    decoder.close();
    encoder.close();

    if (!failed && encoder_code != 0) {
        fprintf(stderr, "[FAIL] ffmpeg encoder exited with code %u\n", encoder_code);
        failed = true;
    }

    fprintf(stderr,
            "[done] %u frames, avg %.1f ms/frame (%.2f fps), resets=%d, blanks=%d%s\n",
            frame_index, frame_index ? elapsed_total / frame_index : 0.0,
            frame_index ? 1000.0 * frame_index / elapsed_total : 0.0, reset_count, blank_count,
            failed ? " -- FAILED" : "");
    // 2 = retryable failure (blank race) -> outer main() re-runs this program.
    return failed ? (retryable ? 2 : 1) : 0;
}

// ---------------------------------------------------------------------------
// Process-level retry for the blank race.
//
// The ZLUDA-backed network comes out all-black in roughly half of the fresh
// processes, stickily (every evaluation of a bad process is black), while a
// fresh process almost always recovers. Retrying INSIDE the process is
// useless, so the runnable body reports exit code 2 on a blank outcome and
// this wrapper re-executes it in a fresh process, up to --retries extra times
// (default 3, i.e. up to 4 attempts in total).
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    // Internal modes never retry: --compile-one and --precompile belong to
    // the translation machinery and have no image pipeline to go black.
    if (argc >= 2 && (!strcmp(argv[1], "--compile-one") || !strcmp(argv[1], "--precompile")))
        return run_main_once(argc, argv);

    // --precompile-wait: 严格串行预热。黑屏率与"进程内/并发翻译"高度相关
    // （实测：单进程无并发 0/10 黑，并发翻译时 50-66% 黑）。此模式先起一个
    // 子进程把全部模块翻译到【彻底退出】，再开主流程；并把
    // DLSSNR_PRECOMPILE_SKIP=1 传给本进程及其重试子进程，主流程不再自行
    // 并行预编译，从而消除黑屏的主要诱因。
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--precompile-wait")) {
            const char* snippet = nullptr;
            const char* driver = nullptr;
            for (int j = 2; j < argc; ++j) {
                if (!snippet && strstr(argv[j], "nvngx_dlssnr")) snippet = argv[j];
                else if (snippet && !driver && strstr(argv[j], "nvcuda")) driver = argv[j];
            }
            if (snippet && driver) {
                fprintf(stderr, "[precompile-wait] 串行预热: %s %s ...\n", snippet, driver);
                fflush(stderr);
                wchar_t self[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, self, MAX_PATH);
                std::wstring wcmd = std::wstring(L"\"") + self + L"\" --precompile \"" +
                                    widen(snippet) + L"\" \"" + widen(driver) + L"\"";
                STARTUPINFOW si = {sizeof si};
                PROCESS_INFORMATION pi = {};
                // Forward the parent's redirected stderr/stdout so the GUI
                // can see module progress from the precompile child.
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
                si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
                const bool can_inherit = si.hStdError && si.hStdError != INVALID_HANDLE_VALUE;
                if (CreateProcessW(self, wcmd.data(), nullptr, nullptr, can_inherit, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    unsigned waited = 0;
                    for (;;) {
                        const DWORD wait = WaitForSingleObject(pi.hProcess, 1000);
                        if (wait != WAIT_TIMEOUT) break;
                        ++waited;
                        if ((waited % 10) == 0) {
                            fprintf(stderr, "[precompile-wait] 仍在预热，已等待 %u 秒...\n", waited);
                            fflush(stderr);
                        }
                    }
                    DWORD code = 0;
                    GetExitCodeProcess(pi.hProcess, &code);
                    CloseHandle(pi.hProcess);
                    fprintf(stderr, "[precompile-wait] 预热完成 (exit %lu)，主流程不再并发编译\n", code);
                } else {
                    fprintf(stderr, "[precompile-wait] 启动预热子进程失败 (error %lu)，继续尝试\n", GetLastError());
                }
            }
            SetEnvironmentVariableA("DLSSNR_PRECOMPILE_SKIP", "1");
            break;
        }
    }

    // Upstream (RedDukeDev) has not fixed the blank race yet. The black state
    // is sticky inside a process AND the NGX snippet refuses to be
    // initialised twice in one process ("already loaded before us"), so a
    // retry MUST be a brand-new process: on a blank outcome (exit 2) this
    // wrapper spawns a fresh copy of itself with the retry budget decreased
    // by one and exits with the child's result. Default 5 retries = up to 6
    // attempts, each in its own process (~1-2% residual at a coin-flip rate).
    int retries = 5;
    int retry_delay_ms = 3000;
    for (int i = 2; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--retries")) {
            retries = atoi(argv[i + 1]);
            if (retries < 0) retries = 0;
        } else if (!strcmp(argv[i], "--retry-delay")) {
            retry_delay_ms = atoi(argv[i + 1]) * 1000;
            if (retry_delay_ms < 0) retry_delay_ms = 0;
        }
    }

    const int rc = run_main_once(argc, argv);
    if (rc != 2 || retries <= 0) return rc;

    fprintf(stderr, "[warn] 本轮输出空白，以全新进程重跑（剩余 %d 次）…\n", retries);
    fflush(stderr);
    // Back-to-back fresh processes share the same GPU/driver churn; a short
    // cool-down between attempts lets the driver settle (helps storm days).
    if (retry_delay_ms > 0) Sleep((DWORD)retry_delay_ms);

    // Re-execute ourselves with --retries budget decremented. The command
    // line comes from Windows so quoting survives untouched.
    std::wstring cmdline = GetCommandLineW();
    {
        // Replace an existing "--retries N" pair, or append one.
        std::wstring token = L"--retries";
        size_t pos = cmdline.find(token);
        const std::wstring repl =
            token + L" " + std::to_wstring(retries > 0 ? retries - 1 : 0);
        if (pos != std::wstring::npos) {
            size_t end = cmdline.find(L' ', pos + token.size());
            if (end != std::wstring::npos)
                cmdline.replace(pos, end - pos, repl);
            else {
                cmdline.erase(pos);
                cmdline += L" " + repl;
            }
        } else {
            cmdline += L" " + repl;
        }
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        fprintf(stderr, "[FAIL] 无法启动重跑进程（错误 %lu）\n", GetLastError());
        return 2;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD childCode = 2;
    GetExitCodeProcess(pi.hProcess, &childCode);
    CloseHandle(pi.hProcess);
    return (int)childCode;
}
