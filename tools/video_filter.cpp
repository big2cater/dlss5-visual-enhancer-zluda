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
#include <shellapi.h>
#include <wincodec.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#pragma comment(lib, "shell32.lib")

#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "../core/image_processor.h"
#include "../core/precompile.h"
#include "../core/gpu_detection.h"
#include "half_float.h"
#include "hardware_budget.h"

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

    Pipe() = default;
    ~Pipe() { close(); }
    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;
    Pipe(Pipe &&o) noexcept : read_end(o.read_end), write_end(o.write_end) {
        o.read_end = nullptr;
        o.write_end = nullptr;
    }
    Pipe &operator=(Pipe &&o) noexcept {
        if (this != &o) {
            close();
            read_end = o.read_end;
            write_end = o.write_end;
            o.read_end = nullptr;
            o.write_end = nullptr;
        }
        return *this;
    }

    bool make(bool inheritable_read, bool inheritable_write, DWORD buffer_size = 16 << 20) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof attributes;
        attributes.bInheritHandle = TRUE;
        // Raw video frames are multi-megabyte. The Win32 default pipe buffer
        // is only a few KiB, which turns every frame into thousands of wakeups
        // and context switches between ffmpeg and this process.
        // We use 16MB by default so an entire 1080p raw RGB48 frame (12.44MB)
        // can be passed in a single atomic burst without stalling.
        if (!CreatePipe(&read_end, &write_end, &attributes, buffer_size)) return false;
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

// --------------------------------------------------------------------------
// Lightweight worker pool for CPU format conversions
// --------------------------------------------------------------------------
class WorkerPool {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable cv_task;
    bool stop = false;

public:
    static WorkerPool &instance() {
        static WorkerPool pool;
        return pool;
    }

    WorkerPool() {
        unsigned int num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;
        if (num_threads > 16) num_threads = 16;
        for (unsigned int i = 0; i < num_threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        cv_task.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    ~WorkerPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        cv_task.notify_all();
        for (auto &w : workers) {
            if (w.joinable()) w.join();
        }
    }

    size_t thread_count() const {
        return workers.size();
    }

    template <typename F>
    void parallel_for(size_t total_items, F &&func) {
        if (total_items == 0) return;
        const size_t n_threads = workers.size();
        if (n_threads <= 1 || total_items < 16384) {
            func(0, total_items, 0);
            return;
        }
        const size_t chunk_size = (total_items + n_threads - 1) / n_threads;
        std::mutex local_mtx;
        std::condition_variable local_cv;
        std::atomic<int> remaining{0};
        size_t scheduled = 0;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            for (size_t t = 0; t < n_threads; ++t) {
                size_t start = t * chunk_size;
                if (start >= total_items) break;
                size_t end = std::min(start + chunk_size, total_items);
                ++scheduled;
                tasks.push([start, end, t, &func, &remaining, &local_mtx, &local_cv] {
                    func(start, end, t);
                    if (--remaining == 0) {
                        std::lock_guard<std::mutex> lk(local_mtx);
                        local_cv.notify_one();
                    }
                });
            }
            remaining.store((int)scheduled);
        }
        cv_task.notify_all();
        {
            std::unique_lock<std::mutex> lock(local_mtx);
            local_cv.wait(lock, [&] { return remaining.load() == 0; });
        }
    }
};

// --------------------------------------------------------------------------
// Thread-safe bounded channel for streaming frames across stages
// --------------------------------------------------------------------------
template <typename T>
class Channel {
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_push_;
    std::condition_variable cv_pop_;
    size_t capacity_;
    bool closed_ = false;

public:
    explicit Channel(size_t capacity = 3) : capacity_(capacity) {}

    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_push_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
        if (closed_) return false;
        queue_.push(std::move(item));
        cv_pop_.notify_one();
        return true;
    }

    bool pop(T &item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_pop_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop();
        cv_push_.notify_one();
        return true;
    }

    void close() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_push_.notify_all();
        cv_pop_.notify_all();
    }

    bool is_closed() {
        std::unique_lock<std::mutex> lock(mutex_);
        return closed_;
    }

    void clear() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!queue_.empty()) queue_.pop();
        cv_push_.notify_all();
    }
};

struct InputFrame {
    unsigned index = 0;
    std::vector<unsigned char> raw_bytes;
    enhancer::Image in;
    enhancer::Image motion;
    bool reset = false;
    bool is_eos = false;
    ID3D12Resource *upload_buf = nullptr;
    void *mapped_ptr = nullptr;

    ~InputFrame() {
        if (upload_buf) {
            if (mapped_ptr) upload_buf->Unmap(0, nullptr);
            upload_buf->Release();
            upload_buf = nullptr;
            mapped_ptr = nullptr;
        }
    }
};


struct OutputFrame {
    unsigned index = 0;
    enhancer::Image out;
    std::vector<unsigned char> model_output;
    double ms = 0.0;
    bool is_eos = false;
    bool blank = false;
};

struct ChildProcess {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE stdin_write = nullptr; // ends we own, closed when done
    HANDLE stdout_read = nullptr;
    HANDLE stderr_share = nullptr;

    ChildProcess() = default;
    ~ChildProcess() { close(); }
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;
    ChildProcess(ChildProcess &&o) noexcept
        : process(o.process), thread(o.thread), stdin_write(o.stdin_write),
          stdout_read(o.stdout_read), stderr_share(o.stderr_share) {
        o.process = o.thread = o.stdin_write = o.stdout_read = o.stderr_share = nullptr;
    }
    ChildProcess &operator=(ChildProcess &&o) noexcept {
        if (this != &o) {
            close();
            process = o.process; thread = o.thread; stdin_write = o.stdin_write;
            stdout_read = o.stdout_read; stderr_share = o.stderr_share;
            o.process = o.thread = o.stdin_write = o.stdout_read = o.stderr_share = nullptr;
        }
        return *this;
    }

    void close() {
        if (stdin_write) { CloseHandle(stdin_write); stdin_write = nullptr; }
        if (stdout_read) { CloseHandle(stdout_read); stdout_read = nullptr; }
        if (stderr_share) { CloseHandle(stderr_share); stderr_share = nullptr; }
        if (thread) { CloseHandle(thread); thread = nullptr; }
        if (process) { CloseHandle(process); process = nullptr; }
    }
};

bool spawn(const std::wstring &command, Pipe *feed /*child stdin*/, Pipe *collect /*child stdout*/,
           ChildProcess &out, bool redirect_stderr = false) {
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = (feed && feed->read_end) ? feed->read_end : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = (collect && collect->write_end) ? collect->write_end : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = (redirect_stderr && collect && collect->write_end) ? collect->write_end : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION process{};
    std::wstring mutable_command = command;
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    out.process = process.hProcess;
    out.thread = process.hThread;
    // The halves handed to the child are no longer ours.
    if (feed && feed->read_end) { CloseHandle(feed->read_end); feed->read_end = nullptr; }
    if (collect && collect->write_end) { CloseHandle(collect->write_end); collect->write_end = nullptr; }
    return true;
}

bool spawn(const std::wstring &command, Pipe &feed /*child stdin*/, Pipe &collect /*child stdout*/,
           ChildProcess &out, bool redirect_stderr = false) {
    return spawn(command, &feed, &collect, out, redirect_stderr);
}

bool wait_exit(HANDLE process, DWORD timeout_ms, DWORD &code) {
    if (!process) { code = 1; return false; }
    DWORD wr = WaitForSingleObject(process, timeout_ms);
    if (wr != WAIT_OBJECT_0) {
        if (wr == WAIT_TIMEOUT) {
            TerminateProcess(process, 1);
        }
        code = 1;
        return false;
    }
    code = 1;
    if (!GetExitCodeProcess(process, &code)) return false;
    return true;
}

std::wstring widen(const char *narrow) {
    if (!narrow || !*narrow) return L"";
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, narrow, -1, nullptr, 0);
    if (len > 0) {
        std::wstring out((size_t)len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, narrow, -1, out.data(), len);
        return out;
    }
    len = MultiByteToWideChar(CP_ACP, 0, narrow, -1, nullptr, 0);
    if (len > 0) {
        std::wstring out((size_t)len - 1, L'\0');
        MultiByteToWideChar(CP_ACP, 0, narrow, -1, out.data(), len);
        return out;
    }
    return L"";
}

// True ffmpeg/ffprobe invocation used by the helpers below.
std::wstring tool_cmd(bool probe) {
    wchar_t from_env[4096] = {};
    if (GetEnvironmentVariableW(L"FFMPEG_PATH", from_env, 4096) && from_env[0]) {
        std::wstring dir = from_env;
        // A trailing backslash would make "dir\ffmpeg.exe" if we just joined.
        if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
        std::wstring exe = dir + (probe ? L"ffprobe.exe" : L"ffmpeg.exe");
        if (exe.find(L' ') != std::wstring::npos && exe.front() != L'"') {
            exe = L"\"" + exe + L"\"";
        }
        return exe;
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
    double duration = 0.0;
    std::string audio_codec;
};

// ffprobe -> "width,height,r_frame_rate" csv line ("1920,1080,30000/1001").
bool probe_video(const std::wstring &input, VideoParams &params, std::string &error) {
    Pipe pipe;
    if (!pipe.make(false, true)) { error = "pipe creation failed"; return false; }
    ChildProcess child;
    std::wstring command = tool_cmd(true) + L" -v error -select_streams v:0 "
                           L"-show_entries stream=width,height,r_frame_rate:format=duration -of csv=p=0 \"" +
                           input + L"\"";
    if (!spawn(command, Pipe{}, pipe, child, true)) {
        error = "ffprobe could not be started (is ffmpeg on PATH, or $FFMPEG_PATH set?)";
        pipe.close();
        return false;
    }
    std::string text;
    char buffer[512];
    DWORD read = 0;
    const auto probe_start = std::chrono::steady_clock::now();
    while (true) {
        DWORD avail = 0;
        if (PeekNamedPipe(pipe.read_end, nullptr, 0, nullptr, &avail, nullptr)) {
            if (avail > 0) {
                if (ReadFile(pipe.read_end, buffer, sizeof buffer, &read, nullptr) && read) {
                    text.append(buffer, read);
                    continue;
                }
            }
        }
        DWORD exit_code = STILL_ACTIVE;
        if (GetExitCodeProcess(child.process, &exit_code) && exit_code != STILL_ACTIVE) {
            while (ReadFile(pipe.read_end, buffer, sizeof buffer, &read, nullptr) && read) {
                text.append(buffer, read);
            }
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - probe_start).count() > 15000) {
            TerminateProcess(child.process, 1);
            pipe.close();
            child.close();
            error = "ffprobe timed out after 15s (process terminated)";
            return false;
        }
        Sleep(10);
    }
    pipe.close();
    DWORD code = 1;
    if (!wait_exit(child.process, 2000, code) || code != 0) {
        error = "ffprobe failed (code " + std::to_string(code) + "): " + text;
        child.close();
        return false;
    }
    child.close();

    unsigned w = 0, h = 0;
    char rate[64] = {};
    double dur = 0.0;
    if (sscanf(text.c_str(), "%u,%u,%63s\n%lf", &w, &h, rate, &dur) >= 3 && w && h) {
        params.width = w;
        params.height = h;
        params.duration = dur;
        unsigned num = 0, den = 1;
        if (sscanf(rate, "%u/%u", &num, &den) == 2 && num && den) params.fps = (double)num / den;
        else if (sscanf(rate, "%u", &num) == 1 && num) params.fps = (double)num;
    } else if (sscanf(text.c_str(), "%u,%u,%63s", &w, &h, rate) == 3 && w && h) {
        params.width = w;
        params.height = h;
        unsigned num = 0, den = 1;
        if (sscanf(rate, "%u/%u", &num, &den) == 2 && num && den) params.fps = (double)num / den;
        else if (sscanf(rate, "%u", &num) == 1 && num) params.fps = (double)num;
    } else {
        error = "could not parse ffprobe output: " + text;
        return false;
    }

    // Probe primary audio stream codec
    Pipe apipe;
    if (apipe.make(false, true)) {
        ChildProcess achild;
        std::wstring acommand = tool_cmd(true) + L" -v error -select_streams a:0 "
                                L"-show_entries stream=codec_name -of csv=p=0 \"" +
                                input + L"\"";
        if (spawn(acommand, Pipe{}, apipe, achild, true)) {
            std::string atext;
            char abuf[128];
            DWORD aread = 0;
            while (ReadFile(apipe.read_end, abuf, sizeof abuf, &aread, nullptr) && aread) {
                atext.append(abuf, aread);
            }
            apipe.close();
            DWORD acode = 1;
            wait_exit(achild.process, 5000, acode);
            achild.close();
            while (!atext.empty() && (atext.back() == '\r' || atext.back() == '\n' || atext.back() == ' ')) {
                atext.pop_back();
            }
            params.audio_codec = atext;
        } else {
            apipe.close();
        }
    }

    return true;
}

// Decoder: ffmpeg -i in -an -f rawvideo -pix_fmt rgb48le -
bool start_decoder(const std::wstring &input, Pipe &collect, ChildProcess &child,
                   unsigned decode_w = 0, unsigned decode_h = 0,
                   double start_sec = 0.0, double duration_sec = 0.0, int ffmpeg_threads = 0,
                   std::string *error = nullptr) {
    Pipe empty;
    std::wstring scale_filter;
    if (decode_w > 0 && decode_h > 0) {
        scale_filter = L"-vf scale=" + std::to_wstring(decode_w) + L":" + std::to_wstring(decode_h) + L":flags=bicubic ";
    }
    std::wstring time_args;
    if (start_sec > 0.001) {
        time_args += L"-ss " + std::to_wstring(start_sec) + L" ";
    }
    if (duration_sec > 0.001) {
        time_args += L"-t " + std::to_wstring(duration_sec) + L" ";
    }
    std::wstring thread_args;
    if (ffmpeg_threads > 0) {
        thread_args = L"-threads " + std::to_wstring(ffmpeg_threads) + L" ";
    }
    std::wstring command = tool_cmd(false) + L" -nostdin -v error " + thread_args + time_args + L"-i \"" + input +
                           L"\" -an " + scale_filter + L"-f rawvideo -pix_fmt rgb48le -";
    if (!spawn(command, empty, collect, child)) {
        if (error) *error = "failed to spawn ffmpeg decoder process";
        return false;
    }
    return true;
}

bool detect_amf_support(unsigned width, unsigned height) {
    if (width < 128 || height < 128) return false;
    static int cached_amf = -1;
    if (cached_amf != -1) return cached_amf == 1;

    Pipe pipe;
    if (!pipe.make(false, true, 4096)) {
        cached_amf = 0;
        return false;
    }
    Pipe empty_feed;
    ChildProcess child;
    std::wstring command = tool_cmd(false) + L" -nostdin -v error -f lavfi -i testsrc=duration=0.1:size=320x240:rate=1 -c:v h264_amf -f null -";
    if (!spawn(command, empty_feed, pipe, child)) {
        pipe.close();
        cached_amf = 0;
        return false;
    }
    pipe.close();
    DWORD code = 1;
    wait_exit(child.process, 5000, code);
    child.close();
    cached_amf = (code == 0) ? 1 : 0;
    return cached_amf == 1;
}

// Encoder: audio by copy from the source, video from the raw pipe.
bool start_encoder(const std::wstring &input, const std::wstring &output,
                   const VideoParams &params, int crf, bool audio, Pipe &feed,
                   ChildProcess &child, unsigned input_width = 0, unsigned input_height = 0,
                   unsigned output_width = 0, unsigned output_height = 0,
                   const std::string &encoder_choice = "auto",
                   int ffmpeg_threads = 0, bool video_only = false) {
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
    if (!input_width) input_width = params.width;
    if (!input_height) input_height = params.height;
    if (!output_width) output_width = input_width;
    if (!output_height) output_height = input_height;

    std::string actual_encoder = encoder_choice;
    if (actual_encoder == "auto") {
        if (detect_amf_support(output_width, output_height)) {
            actual_encoder = "h264_amf";
        } else {
            actual_encoder = "x264";
        }
    }

    auto get_codec_args = [&](const std::string &enc) -> std::wstring {
        const std::wstring color_flags = L"-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv ";
        if (enc == "hevc_amf") {
            return L"-c:v hevc_amf -rc cqp -qp_i " + std::to_wstring(crf) +
                   L" -qp_p " + std::to_wstring(crf) +
                   L" -quality quality -pix_fmt yuv420p " + color_flags;
        } else if (enc == "h264_amf" || enc == "amf") {
            return L"-c:v h264_amf -rc cqp -qp_i " + std::to_wstring(crf) +
                   L" -qp_p " + std::to_wstring(crf) +
                   L" -quality quality -pix_fmt yuv420p " + color_flags;
        } else {
            return L"-c:v libx264 -crf " + std::to_wstring(crf) +
                   L" -preset veryfast -pix_fmt yuv420p " + color_flags;
        }
    };

    const bool is_mkv = output.size() >= 4 &&
        (_wcsicmp(output.c_str() + output.size() - 4, L".mkv") == 0);
    const std::wstring sub_args = is_mkv ? L"-map 0:s? -c:s copy " : L"";

    std::wstring audio_args;
    if (video_only) {
        audio_args = L"-an ";
    } else if (audio) {
        const std::string &ac = params.audio_codec;
        const bool can_copy_in_mp4 = (ac == "aac" || ac == "mp3" || ac == "ac3" || ac == "eac3");
        if (is_mkv || can_copy_in_mp4 || ac.empty()) {
            audio_args = L"-map 0:a? -c:a copy ";
        } else {
            fprintf(stderr, "[audio] Input audio '%s' cannot be copied into MP4; transcoding to AAC (192 kbps)\n", ac.c_str());
            audio_args = L"-map 0:a? -c:a aac -b:a 192k ";
        }
    } else {
        audio_args = L"-an ";
    }

    auto build_command = [&](const std::wstring &codec_args) -> std::wstring {
        const std::wstring vf_args = (output_width != input_width || output_height != input_height)
            ? (L"-vf scale=" + std::to_wstring(output_width) + L":" + std::to_wstring(output_height) +
               L":flags=lanczos:in_color_matrix=bt709:out_color_matrix=bt709:in_range=full:out_range=limited ")
            : L"-vf scale=in_color_matrix=bt709:out_color_matrix=bt709:in_range=full:out_range=limited ";
        const std::wstring thread_args = (ffmpeg_threads > 0)
            ? (L"-threads " + std::to_wstring(ffmpeg_threads) + L" ")
            : L"";

        return tool_cmd(false) + L" -nostdin -v error -y -i \"" + input + L"\" " +
               L"-f rawvideo -pix_fmt rgb48le -s " +
               std::to_wstring(input_width) + L"x" + std::to_wstring(input_height) +
               L" -r " + widen(rate) + L" -i - " +
               L"-map 1:v " +
               audio_args +
               sub_args +
               vf_args +
               thread_args +
               codec_args +
               L"\"" + output + L"\"";
    };

    if (actual_encoder == "hevc_amf") {
        fprintf(stderr, "[encoder] Using AMD AMF HEVC (hardware accelerated, CQP=%d)\n", crf);
    } else if (actual_encoder == "h264_amf" || actual_encoder == "amf") {
        fprintf(stderr, "[encoder] Using AMD AMF H.264 (hardware accelerated, CQP=%d)\n", crf);
    } else {
        fprintf(stderr, "[encoder] Using CPU libx264 (software, CRF=%d)\n", crf);
    }

    std::wstring command = build_command(get_codec_args(actual_encoder));
    if (!spawn(command, feed, empty, child)) {
        if (actual_encoder != "x264" && encoder_choice == "auto") {
            fprintf(stderr, "[warn] Failed to start AMF encoder; falling back to CPU libx264\n");
            actual_encoder = "x264";
            command = build_command(get_codec_args("x264"));
            return spawn(command, feed, empty, child);
        }
        return false;
    }
    return true;
}

static inline double srgb_to_linear(double c) {
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 1.0;
    return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

static inline double linear_to_srgb(double c) {
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 1.0;
    return (c <= 0.0031308) ? (c * 12.92) : (1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
}

const std::array<uint16_t, 65536> &rgb48_half_lut() {
    static const std::array<uint16_t, 65536> lut = [] {
        std::array<uint16_t, 65536> values{};
        for (unsigned i = 0; i < values.size(); ++i) {
            double s = (double)i / 65535.0;
            values[i] = enhancer::float_to_half((float)srgb_to_linear(s));
        }
        return values;
    }();
    return lut;
}

void rgb48_to_half_rgba(const unsigned char *rgb48, enhancer::Image &image) {
    const uint16_t *src = (const uint16_t *)rgb48;
    uint16_t *dst = image.pixels.data();
    const auto &lut = rgb48_half_lut();
    const size_t count = (size_t)image.width * image.height;
    WorkerPool::instance().parallel_for(count, [&](size_t start, size_t end, size_t /*tid*/) {
        for (size_t i = start; i < end; ++i) {
            const uint16_t r = src[i * 3 + 0];
            const uint16_t g = src[i * 3 + 1];
            const uint16_t b = src[i * 3 + 2];
            dst[i * 4 + 0] = lut[r];
            dst[i * 4 + 1] = lut[g];
            dst[i * 4 + 2] = lut[b];
            dst[i * 4 + 3] = 0x3C00; // 1.0
        }
    });
}

// --------------------------------------------------------------------------
// Output tone handling. The network operates in physically LINEAR (HDR) half floats.
// Input is converted from sRGB to linear, and output is converted back from
// linear to sRGB via IEC 61966-2-1.
// Default gamma is 1.0 (standard physical sRGB pass-through);
// --gamma F allows optional custom power curve scaling.
// --------------------------------------------------------------------------
double g_gamma = 1.0;

static inline double to_sdr(double value) {
    if (value <= 0.0) return 0.0;
    if (value >= 1.0) return 1.0;
    if (g_gamma != 1.0) {
        value = std::pow(value, 1.0 / g_gamma);
        if (value <= 0.0) return 0.0;
        if (value >= 1.0) return 1.0;
    }
    return linear_to_srgb(value);
}

uint16_t clamp_half_to_u16(uint16_t half) {
    float value = enhancer::half_to_float(half);
    if (value <= 0.0f) return 0;
    if (value >= 1.0f) return 65535;
    return (uint16_t)(to_sdr(value) * 65535.0 + 0.5);
}

const std::array<uint16_t, 65536> &half_to_rgb48_lut() {
    static std::array<uint16_t, 65536> lut{};
    static double cached_gamma = -1.0;
    if (cached_gamma != g_gamma) {
        for (unsigned i = 0; i < lut.size(); ++i)
            lut[i] = clamp_half_to_u16((uint16_t)i);
        cached_gamma = g_gamma;
    }
    return lut;
}

// --------------------------------------------------------------------------
// Unified post-composite processor (Schemes 3, 4, 5)
// Combines Magpie Rec.709 directional residual gating (Scheme 3),
// frequency-domain high-frequency detail boost (Scheme 4),
// and linear output mix (Scheme 5).
// --------------------------------------------------------------------------
struct CompositeOptions {
    float output_mix = 1.0f;       // --output-mix (0.0..1.0)
    float detail_boost = 1.0f;     // --detail-boost (0.0..2.0)
    float shadow_protect = 1.0f;   // --shadow-protect (0.0..2.0)
    float glow_control = 1.0f;     // --glow-control (0.0..2.0)

    bool is_active() const {
        return std::abs(output_mix - 1.0f) > 0.001f ||
               std::abs(detail_boost - 1.0f) > 0.001f ||
               std::abs(shadow_protect - 1.0f) > 0.001f ||
               std::abs(glow_control - 1.0f) > 0.001f;
    }
};

void apply_post_composite(const enhancer::Image &in, enhancer::Image &out, const CompositeOptions &opts) {
    if (!opts.is_active()) return;
    if (in.empty() || out.empty()) return;

    const unsigned width = out.width;
    const unsigned height = out.height;
    const size_t total_pixels = (size_t)width * height;
    const bool same_dim = (in.width == width && in.height == height);
    const uint16_t *src_in = in.pixels.data();
    uint16_t *src_out = out.pixels.data();

    const bool need_high_freq = (std::abs(opts.detail_boost - 1.0f) > 0.001f);
    std::vector<float> blur_luma;

    if (need_high_freq) {
        // Extract Luma from src_out
        std::vector<float> luma_out(total_pixels);
        WorkerPool::instance().parallel_for(total_pixels, [&](size_t start, size_t end, size_t) {
            for (size_t i = start; i < end; ++i) {
                const size_t idx4 = i * 4;
                const float r = enhancer::half_to_float(src_out[idx4 + 0]);
                const float g = enhancer::half_to_float(src_out[idx4 + 1]);
                const float b = enhancer::half_to_float(src_out[idx4 + 2]);
                luma_out[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            }
        });

        // Separable Gaussian blur (ksize=9, sigma=2.0) on single-channel Luminance
        static const float weights[5] = {0.204164f, 0.180174f, 0.123832f, 0.066282f, 0.027631f};
        std::vector<float> temp_h(total_pixels);
        blur_luma.resize(total_pixels);

        // Pass 1: Horizontal blur
        WorkerPool::instance().parallel_for(height, [&](size_t y_start, size_t y_end, size_t) {
            for (size_t y = y_start; y < y_end; ++y) {
                const size_t row_offset = y * width;
                for (size_t x = 0; x < width; ++x) {
                    float sum = 0.0f;
                    for (int dx = -4; dx <= 4; ++dx) {
                        int sx = std::clamp((int)x + dx, 0, (int)width - 1);
                        sum += luma_out[row_offset + sx] * weights[std::abs(dx)];
                    }
                    temp_h[row_offset + x] = sum;
                }
            }
        });

        // Pass 2: Vertical blur
        WorkerPool::instance().parallel_for(height, [&](size_t y_start, size_t y_end, size_t) {
            for (size_t y = y_start; y < y_end; ++y) {
                const size_t row_offset = y * width;
                for (size_t x = 0; x < width; ++x) {
                    float sum = 0.0f;
                    for (int dy = -4; dy <= 4; ++dy) {
                        int sy = std::clamp((int)y + dy, 0, (int)height - 1);
                        sum += temp_h[(size_t)sy * width + x] * weights[std::abs(dy)];
                    }
                    blur_luma[row_offset + x] = sum;
                }
            }
        });
    }

    // Pass 3: Composite modulation loop
    const float out_mix = std::clamp(opts.output_mix, 0.0f, 1.0f);
    const float one_minus_mix = 1.0f - out_mix;
    const float detail_delta = opts.detail_boost - 1.0f;
    const float shadow_prot = std::clamp(opts.shadow_protect, 0.0f, 2.0f);
    const float glow_ctrl = std::clamp(opts.glow_control, 0.0f, 2.0f);

    const float scale_x = same_dim ? 1.0f : ((float)in.width / (float)width);
    const float scale_y = same_dim ? 1.0f : ((float)in.height / (float)height);

    WorkerPool::instance().parallel_for(total_pixels, [&](size_t p_start, size_t p_end, size_t /*tid*/) {
        for (size_t i = p_start; i < p_end; ++i) {
            const size_t idx4 = i * 4;
            float r_orig, g_orig, b_orig;
            if (same_dim) {
                r_orig = enhancer::half_to_float(src_in[idx4 + 0]);
                g_orig = enhancer::half_to_float(src_in[idx4 + 1]);
                b_orig = enhancer::half_to_float(src_in[idx4 + 2]);
            } else {
                const size_t x = i % width;
                const size_t y = i / width;
                const float sx = (float)x * scale_x;
                const float sy = (float)y * scale_y;
                const int x0 = (int)sx;
                const int y0 = (int)sy;
                const int x1 = std::min(x0 + 1, (int)in.width - 1);
                const int y1 = std::min(y0 + 1, (int)in.height - 1);
                const float fx = sx - x0;
                const float fy = sy - y0;
                const float w00 = (1.0f - fx) * (1.0f - fy);
                const float w10 = fx * (1.0f - fy);
                const float w01 = (1.0f - fx) * fy;
                const float w11 = fx * fy;

                const uint16_t *p00 = &src_in[((size_t)y0 * in.width + x0) * 4];
                const uint16_t *p10 = &src_in[((size_t)y0 * in.width + x1) * 4];
                const uint16_t *p01 = &src_in[((size_t)y1 * in.width + x0) * 4];
                const uint16_t *p11 = &src_in[((size_t)y1 * in.width + x1) * 4];

                r_orig = w00 * enhancer::half_to_float(p00[0]) + w10 * enhancer::half_to_float(p10[0]) +
                         w01 * enhancer::half_to_float(p01[0]) + w11 * enhancer::half_to_float(p11[0]);
                g_orig = w00 * enhancer::half_to_float(p00[1]) + w10 * enhancer::half_to_float(p10[1]) +
                         w01 * enhancer::half_to_float(p01[1]) + w11 * enhancer::half_to_float(p11[1]);
                b_orig = w00 * enhancer::half_to_float(p00[2]) + w10 * enhancer::half_to_float(p10[2]) +
                         w01 * enhancer::half_to_float(p01[2]) + w11 * enhancer::half_to_float(p11[2]);
            }

            const float r_dlss = enhancer::half_to_float(src_out[idx4 + 0]);
            const float g_dlss = enhancer::half_to_float(src_out[idx4 + 1]);
            const float b_dlss = enhancer::half_to_float(src_out[idx4 + 2]);

            // Linear Rec.709 luminance
            const float y_orig = 0.2126f * r_orig + 0.7152f * g_orig + 0.0722f * b_orig;
            const float y_dlss = 0.2126f * r_dlss + 0.7152f * g_dlss + 0.0722f * b_dlss;
            const float delta_y = y_dlss - y_orig;

            // Continuous, seamless shadow & highlight gating (zero threshold artifacts, zero posterization/color blocks):
            // 1. Deep shadow fog protection:
            //    Only deep blacks (y_orig < y_knee) lifted by DLSS (delta_y > 0) are suppressed.
            //    Midtones (human skin, face, clothing at y >= y_knee) have dark_weight = 0.0, completely untouched!
            constexpr float y_knee = 0.06f; // threshold where deep black transitions to midtone
            float eff_mult = 1.0f;
            if (delta_y > 0.0f) {
                if (y_orig < y_knee) {
                    const float t = y_orig / y_knee;
                    const float dark_weight = (1.0f - t) * (1.0f - t); // quadratic smooth falloff: 1.0 at 0, 0.0 at y_knee
                    eff_mult = 1.0f - dark_weight * (1.0f - shadow_prot);
                } else if (y_orig > 0.50f) {
                    const float t = std::clamp((y_orig - 0.50f) / 0.50f, 0.0f, 1.0f);
                    const float glow_weight = t * t * (3.0f - 2.0f * t); // smoothstep
                    eff_mult = 1.0f + glow_weight * (glow_ctrl - 1.0f);
                }
            }

            // Apply smooth multiplier to the delta
            float r_cand = r_orig + (r_dlss - r_orig) * eff_mult;
            float g_cand = g_orig + (g_dlss - g_orig) * eff_mult;
            float b_cand = b_orig + (b_dlss - b_orig) * eff_mult;

            // High frequency detail modulation (pure luminance, 100% chromaticity preserving)
            if (need_high_freq) {
                const float hf_luma = y_dlss - blur_luma[i];
                const float hf_boost = std::clamp(detail_delta * hf_luma, -0.15f, 0.15f);
                r_cand += hf_boost;
                g_cand += hf_boost;
                b_cand += hf_boost;
            }

            if (r_cand < 0.0f) r_cand = 0.0f;
            if (g_cand < 0.0f) g_cand = 0.0f;
            if (b_cand < 0.0f) b_cand = 0.0f;

            // Final output mix with original
            float r_final = one_minus_mix * r_orig + out_mix * r_cand;
            float g_final = one_minus_mix * g_orig + out_mix * g_cand;
            float b_final = one_minus_mix * b_orig + out_mix * b_cand;

            if (r_final < 0.0f) r_final = 0.0f;
            if (g_final < 0.0f) g_final = 0.0f;
            if (b_final < 0.0f) b_final = 0.0f;

            src_out[idx4 + 0] = enhancer::float_to_half(r_final);
            src_out[idx4 + 1] = enhancer::float_to_half(g_final);
            src_out[idx4 + 2] = enhancer::float_to_half(b_final);
            // alpha channel is preserved
        }
    });
}

// half RGBA -> rgb48, and while we are in there, a cheap blank-frame check.
void half_rgba_to_rgb48(const enhancer::Image &image, unsigned char *rgb48, bool &blank) {
    const uint16_t *src = image.pixels.data();
    uint16_t *dst = (uint16_t *)rgb48;
    const auto &lut = half_to_rgb48_lut();
    const size_t count = (size_t)image.width * image.height;

    struct PartialStat {
        double sum = 0.0;
        double sum_sq = 0.0;
        size_t samples = 0;
    };
    const size_t n_threads = WorkerPool::instance().thread_count();
    std::vector<PartialStat> stats(n_threads);

    WorkerPool::instance().parallel_for(count, [&](size_t start, size_t end, size_t tid) {
        double l_sum = 0.0;
        double l_sum_sq = 0.0;
        size_t l_samples = 0;
        for (size_t i = start; i < end; ++i) {
            const uint16_t r = lut[src[i * 4 + 0]];
            const uint16_t g = lut[src[i * 4 + 1]];
            const uint16_t b = lut[src[i * 4 + 2]];
            dst[i * 3 + 0] = r;
            dst[i * 3 + 1] = g;
            dst[i * 3 + 2] = b;
            if ((i & 7u) == 0) {
                const double luma = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 65535.0;
                l_sum += luma;
                l_sum_sq += luma * luma;
                ++l_samples;
            }
        }
        if (tid < stats.size()) {
            stats[tid].sum = l_sum;
            stats[tid].sum_sq = l_sum_sq;
            stats[tid].samples = l_samples;
        }
    });

    double sum = 0.0, sum_sq = 0.0;
    size_t samples = 0;
    for (const auto &s : stats) {
        sum += s.sum;
        sum_sq += s.sum_sq;
        samples += s.samples;
    }
    const double mean = samples ? sum / (double)samples : 0.0;
    const double variance = samples ? sum_sq / (double)samples - mean * mean : 0.0;
    blank = (mean < 0.005 && variance < 0.0001);
}

void resize_rgb48(const unsigned char *src, unsigned sw, unsigned sh,
                  unsigned char *dst, unsigned dw, unsigned dh) {
    const uint16_t *in = (const uint16_t *)src; uint16_t *out = (uint16_t *)dst;
    WorkerPool::instance().parallel_for(dh, [&](size_t y_start, size_t y_end, size_t /*tid*/) {
        for (size_t y = y_start; y < y_end; ++y) {
            double fy = ((double)y + 0.5) * sh / dh - 0.5; int y0 = (int)floor(fy); double ty = fy - y0;
            if (y0 < 0) { y0 = 0; ty = 0; } if (y0 >= (int)sh - 1) { y0 = (int)sh - 1; ty = 0; } int y1 = (y0 + 1 < (int)sh) ? y0 + 1 : y0;
            for (unsigned x = 0; x < dw; ++x) {
                double fx = ((double)x + 0.5) * sw / dw - 0.5; int x0 = (int)floor(fx); double tx = fx - x0;
                if (x0 < 0) { x0 = 0; tx = 0; } if (x0 >= (int)sw - 1) { x0 = (int)sw - 1; tx = 0; } int x1 = (x0 + 1 < (int)sw) ? x0 + 1 : x0;
                for (int c = 0; c < 3; ++c) {
                    double a = in[((size_t)y0 * sw + x0) * 3 + c] * (1 - tx) + in[((size_t)y0 * sw + x1) * 3 + c] * tx;
                    double b = in[((size_t)y1 * sw + x0) * 3 + c] * (1 - tx) + in[((size_t)y1 * sw + x1) * 3 + c] * tx;
                    out[((size_t)y * dw + x) * 3 + c] = (uint16_t)(a * (1 - ty) + b * ty + 0.5);
                }
            }
        }
    });
}

// --------------------------------------------------------------------------
// Scene-cut detection (reset trigger)
// --------------------------------------------------------------------------

struct SceneDetector {
    double threshold = 0.30; // mean luma difference in 0..1 that counts as a cut
    std::vector<uint16_t> previous_luma; // strided grid, null until the first frame
    std::vector<uint16_t> current_luma;  // scratch buffer reused for each frame
    unsigned step_x = 1, step_y = 1;

    void configure(unsigned width, unsigned height, double threshold_value) {
        threshold = threshold_value;
        // A grid of roughly 64x36 samples; striding reads the raw 16-bit
        // planes directly, no resampling needed for a cut test.
        step_x = width / 64; if (step_x < 1) step_x = 1;
        step_y = height / 36; if (step_y < 1) step_y = 1;
        const size_t samples = (size_t)((width + step_x - 1) / step_x) *
                               ((height + step_y - 1) / step_y);
        previous_luma.clear();
        current_luma.clear();
        previous_luma.reserve(samples);
        current_luma.reserve(samples);
    }

    // Returns true when the incoming frame should reset the accumulation.
    bool consider(const unsigned char *rgb48, unsigned width, unsigned height) {
        const uint16_t *src = (const uint16_t *)rgb48;
        current_luma.clear();
        for (unsigned y = 0; y < height; y += step_y) {
            for (unsigned x = 0; x < width; x += step_x) {
                const uint16_t r = src[((size_t)y * width + x) * 3 + 0];
                const uint16_t g = src[((size_t)y * width + x) * 3 + 1];
                const uint16_t b = src[((size_t)y * width + x) * 3 + 2];
                current_luma.push_back((uint16_t)(((uint32_t)r * 19595 + (uint32_t)g * 38470 +
                                                   (uint32_t)b * 7471) >> 16));
            }
        }
        if (previous_luma.empty()) {
            previous_luma.swap(current_luma);
            return true; // first frame always resets
        }
        if (current_luma.size() != previous_luma.size()) {
            previous_luma.swap(current_luma);
            return true;
        }
        uint64_t total = 0;
        for (size_t i = 0; i < current_luma.size(); ++i) {
            uint64_t d = current_luma[i] > previous_luma[i] ? current_luma[i] - previous_luma[i]
                                                            : previous_luma[i] - current_luma[i];
            total += d;
        }
        const size_t sample_count = current_luma.size();
        previous_luma.swap(current_luma);
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
    double gamma = 1.0;      // output gamma, 1.0 = standard IEC 61966-2-1 sRGB
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
    std::string encoder = "auto";
    int yield_ms = 1; // ms to yield per frame to prevent TDR and keep DWM responsive
    CompositeOptions comp_opts;
    std::string parallel_mode = "auto";
    bool is_child_chunk = false;
    double chunk_start_sec = 0.0;
    double chunk_duration_sec = 0.0;
    double warmup_sec = 0.0;
    unsigned warmup_discard_frames = 0;
    unsigned frame_index_offset = 0;
    int ffmpeg_threads = 0;
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
        "  --gamma F             output gamma adjustment (1.0 default = standard sRGB)\n"
        "  --flow 0|1           motion-vector guidance for video (0 default; 1 = on)\n"
        "  --upscale-mode P     native, quality, balanced, performance, ultra\n"
        "  --model-scale F      internal DLSS render scale, 0.25..1.0 (1.0 default)\n"
        "  --dlss-model-preset P DLSS model preset: default, J, K, L, M\n"
        "  --intensity F --global-tone F --local-tone F --local-structure F\n"
        "  --skin-structure F --style N --preset N --no-auto-mask\n"
        "  --output-mix F        AI output blend mix, 0.0..1.0 (1.0 default = 100%% AI)\n"
        "  --detail-boost F      high-frequency detail boost, 0.0..2.0 (1.0 default)\n"
        "  --shadow-protect F    shadow darkening protection multiplier, 0.0..2.0 (1.0 default)\n"
        "  --glow-control F      reflection/highlight glow multiplier, 0.0..2.0 (1.0 default)\n"
        "  --crf N               quality / CQP value (18)\n"
        "  --encoder P           encoder: auto, amf/h264_amf, hevc_amf, x264 (auto default)\n"
        "  --fps N               override output frame rate\n"
        "  --no-audio            don't copy the source audio\n"
        "  --max-frames N        stop after N frames\n"
        "  --parallel auto|2|off multi-process temporal chunk parallelization (auto default, with hardware guard)\n"
        "  --yield-ms N          GPU scheduling yield per frame in ms for TDR prevention (1 default)\n"
        "  --dump-frames DIR     write each output frame as PNG into DIR\n");
}

bool parse_args(int argc, char **argv, Options &options) {
    if (argc < 5) { usage(); return false; }
    options.input = widen(argv[1]);
    options.output = widen(argv[2]);
    options.snippet = widen(argv[3]);
    options.driver = widen(argv[4]);
    options.runtime = L"nvngx.dll";
    options.nvapi = L"nvapi64.dll";

    int arg_start = 5;
    if (argc > 5 && argv[5][0] != '-') {
        options.runtime = widen(argv[5]);
        arg_start = 6;
        if (argc > 6 && argv[6][0] != '-') {
            options.nvapi = widen(argv[6]);
            arg_start = 7;
        }
    }

    for (int i = arg_start; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", name); return nullptr; }
            return argv[++i];
        };
        if (arg == "--passes") {
            const char *v = need("--passes"); if (!v) return false;
            options.settings.passes = atoi(v);
            if (options.settings.passes < 1) options.settings.passes = 1;
            if (options.settings.passes > 10) options.settings.passes = 10;
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
            if (options.cut_threshold < 0.0) options.cut_threshold = 0.0;
            if (options.cut_threshold > 1.0) options.cut_threshold = 1.0;
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
            const char *v = need("--model-scale"); if (!v) return false;
            options.model_scale = atof(v);
            if (options.model_scale < 0.25) options.model_scale = 0.25;
            if (options.model_scale > 1.0) options.model_scale = 1.0;
        } else if (arg == "--dlss-model-preset") {
            const char *v = need("--dlss-model-preset"); if (!v) return false;
            options.dlss_model_preset = v;
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
            if (options.settings.style < 0) options.settings.style = 0;
            if (options.settings.style > 3) options.settings.style = 3;
        } else if (arg == "--preset") {
            const char *v = need("--preset"); if (!v) return false;
            options.settings.preset = atoi(v);
            if (options.settings.preset < 0) options.settings.preset = 0;
            if (options.settings.preset > 3) options.settings.preset = 3;
        } else if (arg == "--no-auto-mask") {
            options.settings.auto_mask = false;
        } else if (arg == "--output-mix") {
            const char *v = need("--output-mix"); if (!v) return false;
            options.comp_opts.output_mix = (float)atof(v);
        } else if (arg == "--detail-boost") {
            const char *v = need("--detail-boost"); if (!v) return false;
            options.comp_opts.detail_boost = (float)atof(v);
        } else if (arg == "--shadow-protect") {
            const char *v = need("--shadow-protect"); if (!v) return false;
            options.comp_opts.shadow_protect = (float)atof(v);
        } else if (arg == "--glow-control") {
            const char *v = need("--glow-control"); if (!v) return false;
            options.comp_opts.glow_control = (float)atof(v);
        } else if (arg == "--crf") {
            const char *v = need("--crf"); if (!v) return false;
            options.crf = atoi(v);
            if (options.crf < 0) options.crf = 0;
            if (options.crf > 51) options.crf = 51;
        } else if (arg == "--encoder") {
            const char *v = need("--encoder"); if (!v) return false;
            options.encoder = v;
        } else if (arg == "--fps") {
            const char *v = need("--fps"); if (!v) return false;
            options.fps_override = atof(v);
        } else if (arg == "--no-audio") {
            options.audio = false;
        } else if (arg == "--max-frames") {
            const char *v = need("--max-frames"); if (!v) return false;
            options.max_frames = atoi(v);
            if (options.max_frames < 0) {
                fprintf(stderr, "[warn] --max-frames must be non-negative; set to 0\n");
                options.max_frames = 0;
            }
        } else if (arg == "--yield-ms") {
            const char *v = need("--yield-ms"); if (!v) return false;
            options.yield_ms = atoi(v);
            if (options.yield_ms < 0) options.yield_ms = 0;
            options.settings.yield_ms = options.yield_ms;
        } else if (arg == "--parallel") {
            const char *v = need("--parallel"); if (!v) return false;
            options.parallel_mode = v;
        } else if (arg == "--child-chunk") {
            const char *v1 = need("--child-chunk (start)"); if (!v1) return false;
            const char *v2 = need("--child-chunk (duration)"); if (!v2) return false;
            const char *v3 = need("--child-chunk (warmup)"); if (!v3) return false;
            options.is_child_chunk = true;
            options.chunk_start_sec = atof(v1);
            options.chunk_duration_sec = atof(v2);
            options.warmup_sec = atof(v3);
        } else if (arg == "--frame-index-offset") {
            const char *v = need("--frame-index-offset"); if (!v) return false;
            options.frame_index_offset = (unsigned)atoi(v);
        } else if (arg == "--ffmpeg-threads") {
            const char *v = need("--ffmpeg-threads"); if (!v) return false;
            options.ffmpeg_threads = atoi(v);
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
        if (!device) { fprintf(stderr, "[flow] no D3D12 device\n"); rel(adapter); factory->Release(); return false; }
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if ((!queue && FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&cmd))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            fprintf(stderr, "[flow] failed to create D3D12 objects (queue=%p)\n", (void*)queue);
            rel(adapter); factory->Release(); stop(); return false;
        }
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
    uint hpx = min(x * 4, FW - 1), hpy = min(y * 4, FH - 1);
    uint old = Hist[hpy*FW+hpx]; float ox=f16tof32(old & 0xffff), oy=f16tof32(old >> 16);
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
        if (FAILED(D3DCompile(src, strlen(src), "gpu_flow", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) {
            if (err) { fprintf(stderr, "[flow] shader compile failed: %.*s\n", (int)err->GetBufferSize(), (const char*)err->GetBufferPointer()); err->Release(); }
            rel(adapter); factory->Release(); stop(); return false;
        }
        D3D12_DESCRIPTOR_RANGE ranges[2]{}; ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors=3; ranges[0].BaseShaderRegister=0; ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors=2; ranges[1].BaseShaderRegister=0;
        D3D12_ROOT_PARAMETER rp[3]{}; rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues=4; rp[0].Constants.ShaderRegister=0; rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges=1; rp[1].DescriptorTable.pDescriptorRanges=&ranges[0]; rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[2].DescriptorTable.NumDescriptorRanges=1; rp[2].DescriptorTable.pDescriptorRanges=&ranges[1];
        D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters=3; rsd.pParameters=rp; rsd.Flags=D3D12_ROOT_SIGNATURE_FLAG_NONE; ID3DBlob *sig=nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rsd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err)) || FAILED(device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root)))) { if(err) err->Release(); if(sig) sig->Release(); cs->Release(); rel(adapter); factory->Release(); stop(); return false; }
        sig->Release(); D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root; pd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
        bool ok=SUCCEEDED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso))); cs->Release(); if(!ok) { fprintf(stderr, "[flow] CreateComputePipelineState failed\n"); rel(adapter); factory->Release(); stop(); return false; }
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=5; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)))) { fprintf(stderr, "[flow] CreateDescriptorHeap failed\n"); rel(adapter); factory->Release(); stop(); return false; }
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
        if (!prev || !cur) return false;
        unsigned *p = nullptr;
        D3D12_RANGE z{0, 0};
        if (FAILED(prev->Map(0, &z, (void**)&p)) || !p) return false;
        memcpy(p, prev_luma.data(), prev_luma.size() * 4);
        prev->Unmap(0, nullptr);
        p = nullptr;
        if (FAILED(cur->Map(0, &z, (void**)&p)) || !p) return false;
        memcpy(p, cur_luma.data(), cur_luma.size() * 4);
        cur->Unmap(0, nullptr);
        const uint32_t constants[4] = {qw, qh, fw, fh};
        auto cpu=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); auto base=heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; sv.Format=DXGI_FORMAT_UNKNOWN; sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Buffer.NumElements=qw*qh; sv.Buffer.StructureByteStride=4;
        device->CreateShaderResourceView(prev,&sv,{base.ptr});
        device->CreateShaderResourceView(cur,&sv,{base.ptr+cpu});
        D3D12_SHADER_RESOURCE_VIEW_DESC hv=sv; hv.Buffer.NumElements=fw*fh;
        device->CreateShaderResourceView(history,&hv,{base.ptr+cpu*2});
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; uv.Format=DXGI_FORMAT_UNKNOWN; uv.Buffer.NumElements=qw*qh; uv.Buffer.StructureByteStride=8;
        device->CreateUnorderedAccessView(out,nullptr,&uv,{base.ptr+cpu*3});
        D3D12_UNORDERED_ACCESS_VIEW_DESC fv{}; fv.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; fv.Format=DXGI_FORMAT_UNKNOWN; fv.Buffer.NumElements=fw*fh; fv.Buffer.StructureByteStride=4;
        device->CreateUnorderedAccessView(full_out,nullptr,&fv,{base.ptr+cpu*4});
        allocator->Reset(); cmd->Reset(allocator,pso); ID3D12DescriptorHeap *hs[]={heap}; cmd->SetDescriptorHeaps(1,hs); cmd->SetComputeRootSignature(root); cmd->SetComputeRoot32BitConstants(0,4,constants,0); auto gpu=heap->GetGPUDescriptorHandleForHeapStart(); cmd->SetComputeRootDescriptorTable(1,gpu); cmd->SetComputeRootDescriptorTable(2,{gpu.ptr+cpu*3}); cmd->Dispatch((qw+7)/8,(qh+7)/8,1); D3D12_RESOURCE_BARRIER b[3]{}; b[0].Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; b[0].UAV.pResource=out; b[1].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[1].Transition.pResource=out; b[1].Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE; b[1].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b[2].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[2].Transition.pResource=full_out; b[2].Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b[2].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE; b[2].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; cmd->ResourceBarrier(3,b); cmd->CopyResource(readback,out); cmd->Close(); ID3D12CommandList *ls[]={cmd}; queue->ExecuteCommandLists(1,ls); queue->Signal(fence,++fence_value); if(fence->GetCompletedValue()<fence_value){fence->SetEventOnCompletion(fence_value,fence_event);WaitForSingleObject(fence_event,5000);}
        allocator->Reset(); cmd->Reset(allocator,nullptr);
        D3D12_RESOURCE_BARRIER hb[3]{};
        hb[0].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        hb[0].Transition.pResource=history;
        hb[0].Transition.StateBefore=D3D12_RESOURCE_STATE_GENERIC_READ;
        hb[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        hb[0].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1,&hb[0]);
        cmd->CopyResource(history,full_out);
        hb[0].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
        hb[0].Transition.StateAfter=D3D12_RESOURCE_STATE_GENERIC_READ;
        cmd->ResourceBarrier(1,&hb[0]);
        hb[2].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        hb[2].Transition.pResource=out;
        hb[2].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
        hb[2].Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        hb[2].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1,&hb[2]);
        hb[2].Transition.pResource=full_out;
        hb[2].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
        hb[2].Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        hb[2].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1,&hb[2]);
        cmd->Close(); ID3D12CommandList *hls[]={cmd}; queue->ExecuteCommandLists(1,hls); queue->Signal(fence,++fence_value); if(fence->GetCompletedValue()<fence_value){fence->SetEventOnCompletion(fence_value,fence_event);WaitForSingleObject(fence_event,5000);}
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
        // Note: WIC's format converter to GUID_WICPixelFormat64bppRGBAHalf already
        // decodes sRGB integer formats into linear radiance half-floats.
        // Applying srgb_to_linear again would cause double linearization and crush the image.
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
        return 1;
    }
    enhancer::Settings settings;
    settings.passes = 3; // stills settle over a few evaluations
    int retries = 3;     // blank-output races are re-evaluated this many times
    double upscale_factor = 1.0;
    double model_scale = 1.0;
    CompositeOptions comp_opts;
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
        if (arg == "--passes") { const char *v = need("--passes"); if (!v) return 1; settings.passes = atoi(v); }
        else if (arg == "--retries") { const char *v = need("--retries"); if (!v) return 1; retries = atoi(v); if (retries < 0) retries = 0; }
        else if (arg == "--gamma") { const char *v = need("--gamma"); if (!v) return 1; g_gamma = atof(v); if (g_gamma < 0.1) g_gamma = 0.1; }
        else if (arg == "--intensity") { const char *v = need("--intensity"); if (!v) return 1; settings.intensity = (float)atof(v); }
        else if (arg == "--global-tone") { const char *v = need("--global-tone"); if (!v) return 1; settings.global_tone = (float)atof(v); }
        else if (arg == "--local-tone") { const char *v = need("--local-tone"); if (!v) return 1; settings.local_tone = (float)atof(v); }
        else if (arg == "--local-structure") { const char *v = need("--local-structure"); if (!v) return 1; settings.local_structure = (float)atof(v); }
        else if (arg == "--skin-structure") { const char *v = need("--skin-structure"); if (!v) return 1; settings.skin_structure = (float)atof(v); }
        else if (arg == "--style") { const char *v = need("--style"); if (!v) return 1; settings.style = atoi(v); }
        else if (arg == "--preset") { const char *v = need("--preset"); if (!v) return 1; settings.preset = atoi(v); }
        else if (arg == "--dlss-model-preset") { const char *v = need("--dlss-model-preset"); if (!v) return 1; SetEnvironmentVariableA("DLSS_PRESET", v); }
        else if (arg == "--no-auto-mask") { settings.auto_mask = false; }
        else if (arg == "--output-mix") { const char *v = need("--output-mix"); if (!v) return 1; comp_opts.output_mix = (float)atof(v); }
        else if (arg == "--detail-boost") { const char *v = need("--detail-boost"); if (!v) return 1; comp_opts.detail_boost = (float)atof(v); }
        else if (arg == "--shadow-protect") { const char *v = need("--shadow-protect"); if (!v) return 1; comp_opts.shadow_protect = (float)atof(v); }
        else if (arg == "--glow-control") { const char *v = need("--glow-control"); if (!v) return 1; comp_opts.glow_control = (float)atof(v); }
        else if (arg == "--precompile-wait") { /* shell flag, ignore */ }
        else if (arg == "--retry-delay") { need("--retry-delay"); }
        else if (arg == "--upscale-mode" || arg == "--upscale") {
            const char *v = need(arg.c_str()); if (!v) return 1;
            if (!_stricmp(v, "quality")) upscale_factor = 1.5;
            else if (!_stricmp(v, "balanced")) upscale_factor = 1.724;
            else if (!_stricmp(v, "performance")) upscale_factor = 2.0;
            else if (!_stricmp(v, "ultra")) upscale_factor = 3.0;
            else { double f = atof(v); if (f >= 1.0) upscale_factor = f; }
        }
        else if (arg == "--model-scale") {
            const char *v = need("--model-scale"); if (!v) return 1;
            model_scale = atof(v);
            if (model_scale < 0.25) model_scale = 0.25;
            if (model_scale > 1.0) model_scale = 1.0;
        }
        else if (arg == "--flow" || arg == "--no-flow" || arg == "--flow-only") { /* ignore */ }
        else if (arg == "--cut-threshold") { need("--cut-threshold"); }
        else if (arg == "--crf") { need("--crf"); }
        else if (arg == "--fps") { need("--fps"); }
        else if (arg == "--no-audio") { /* ignore */ }
        else if (arg == "--max-frames") { need("--max-frames"); }
        else if (arg == "--parallel") { need("--parallel"); }
        else if (arg == "--yield-ms") { need("--yield-ms"); }
        else if (arg == "--dump-frames" || arg == "--dry-run") { /* ignore */ }
        else if (arg == "--ffmpeg-threads") { need("--ffmpeg-threads"); }
        else if (arg == "--encoder") { need("--encoder"); }
        else if (arg == "--warmup-sec") { need("--warmup-sec"); }
        else if (arg == "--chunk-sec") { need("--chunk-sec"); }
        else { fprintf(stderr, "unknown option: %s\n", arg.c_str()); return 1; }
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
    if (upscale_factor > 1.0) {
        settings.output_width = ((unsigned)std::round(in.width * upscale_factor)) & ~1u;
        settings.output_height = ((unsigned)std::round(in.height * upscale_factor)) & ~1u;
        fprintf(stderr, "[info] image %ux%u -> %ux%u (upscale %.2fx), %d passes\n",
                in.width, in.height, settings.output_width, settings.output_height, upscale_factor, settings.passes);
    } else {
        fprintf(stderr, "[info] image %ux%u, %d passes\n", in.width, in.height, settings.passes);
    }
    if (comp_opts.is_active()) {
        fprintf(stderr, "[composite] 画面合成与防起雾已开启: 混合=%.0f%%, 细节=%.0f%%, 暗部保护=%.0f%%, 高光=%.0f%%\n",
                comp_opts.output_mix * 100.0f,
                comp_opts.detail_boost * 100.0f,
                comp_opts.shadow_protect * 100.0f,
                comp_opts.glow_control * 100.0f);
        fflush(stderr);
    }

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
        if (error.find("blank image") != std::string::npos) {
            fprintf(stderr, "[warn] 单帧输出空白（ZLUDA 概率竞态），交由外层重跑\n");
            return 2;
        }
        return 1;
    }
    const bool blank = output_is_blank(out);
    if (blank)
        fprintf(stderr, "[warn] 输出空白：本轮求值竞态，交给外层重跑\n");
    else if (comp_opts.is_active()) {
        apply_post_composite(in, out, comp_opts);
    }
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
    if (image.empty()) return true;
    const uint16_t *src = image.pixels.data();
    const size_t count = (size_t)image.width * image.height;
    const size_t step = 8;
    double sum = 0.0, sum_sq = 0.0;
    size_t samples = 0;
    float max_luma = 0.0f;
    for (size_t i = 0; i < count; i += step) {
        const float r = enhancer::half_to_float(src[i * 4 + 0]);
        const float g = enhancer::half_to_float(src[i * 4 + 1]);
        const float b = enhancer::half_to_float(src[i * 4 + 2]);
        const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        if (luma > max_luma) max_luma = luma;
        sum += luma;
        sum_sq += (double)luma * luma;
        ++samples;
    }
    if (samples == 0) return true;
    const double mean = sum / (double)samples;
    const double variance = sum_sq / (double)samples - mean * mean;
    // A true blank output from a failed GPU launch has max_luma near zero (< 0.005)
    // and almost zero variance (< 0.0001). A valid dark photograph/screenshot has real highlights.
    return (max_luma < 0.005f) || (mean < 0.002 && variance < 0.0001);
}

static int run_parallel_orchestrator(int argc, char **argv, const Options &options,
                                     const VideoParams &params,
                                     const dlssnr_budget::HardwareInfo &budget,
                                     int concurrency) {
    fprintf(stderr, "[parallel] 启动 %d 进程分片并发加速 (时长: %.1fs, 帧率: %.2f fps, 显存/CPU安全分流)\n",
            concurrency, params.duration, params.fps);
    fflush(stderr);

    const double total_dur = params.duration;
    const double split_sec = total_dur / 2.0;
    double warmup_sec = 1.0;
    if (split_sec < 3.0) warmup_sec = 0.5;

    std::wstring self_exe(32768, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, self_exe.data(), (DWORD)self_exe.size());
    self_exe.resize(len);

    std::wstring out_part0 = options.output + L".part0.mp4";
    std::wstring out_part1 = options.output + L".part1.mp4";
    std::wstring concat_list_path = options.output + L".concat.txt";

    _wremove(out_part0.c_str());
    _wremove(out_part1.c_str());
    _wremove(concat_list_path.c_str());

    auto build_worker_cmd = [&](const std::wstring &chunk_out, double start_s, double dur_s,
                                double warmup_s, unsigned frame_offset, int worker_max_frames) -> std::wstring {
        std::wstring cmd = L"\"" + self_exe + L"\"";
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = widen(argv[i]);
            if (i == 2) {
                cmd += L" \"" + chunk_out + L"\"";
            } else if (arg == L"--parallel") {
                cmd += L" --parallel off";
                if (i + 1 < argc && argv[i + 1][0] != '-') ++i; // skip value
            } else if (arg == L"--max-frames") {
                if (i + 1 < argc && argv[i + 1][0] != '-') ++i; // skip value
            } else {
                if (arg.find(L' ') != std::wstring::npos) {
                    cmd += L" \"" + arg + L"\"";
                } else {
                    cmd += L" " + arg;
                }
            }
        }
        cmd += L" --child-chunk " + std::to_wstring(start_s) + L" " + std::to_wstring(dur_s) +
               L" " + std::to_wstring(warmup_s);
        cmd += L" --frame-index-offset " + std::to_wstring(frame_offset);
        if (budget.ffmpeg_threads_per_worker > 0) {
            cmd += L" --ffmpeg-threads " + std::to_wstring(budget.ffmpeg_threads_per_worker);
        }
        if (worker_max_frames > 0) {
            cmd += L" --max-frames " + std::to_wstring(worker_max_frames);
        }
        return cmd;
    };

    int w0_max = 0;
    int w1_max = 0;
    if (options.max_frames > 0) {
        unsigned w0_estimated = (unsigned)std::round(split_sec * params.fps);
        w0_max = (int)std::min((unsigned)options.max_frames, w0_estimated);
        int rem = options.max_frames - w0_max;
        if (rem > 0) {
            unsigned w1_warmup = (unsigned)std::round(warmup_sec * params.fps);
            w1_max = rem + (int)w1_warmup;
        } else {
            w1_max = 1;
        }
    }

    std::wstring cmd0 = build_worker_cmd(out_part0, 0.0, split_sec, 0.0, 0, w0_max);
    std::wstring cmd1 = build_worker_cmd(out_part1, split_sec, total_dur - split_sec, warmup_sec,
                                         (unsigned)std::round(split_sec * params.fps), w1_max);

    STARTUPINFOW si0{}, si1{};
    si0.cb = sizeof(si0);
    si0.dwFlags = STARTF_USESTDHANDLES;
    si0.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si0.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si0.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    si1.cb = sizeof(si1);
    si1.dwFlags = STARTF_USESTDHANDLES;
    si1.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si1.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si1.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi0{}, pi1{};

    // Launch worker 0 with below normal priority (prevents desktop/UI freeze)
    if (!CreateProcessW(nullptr, cmd0.data(), nullptr, nullptr, TRUE,
                        BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si0, &pi0)) {
        fprintf(stderr, "[FAIL] 无法启动分片工作进程 0 (错误码 %lu)\n", GetLastError());
        return 1;
    }

    // Small stagger delay to let worker 0 initialize pipeline without driver race
    Sleep(600);

    // Launch worker 1 with below normal priority
    if (!CreateProcessW(nullptr, cmd1.data(), nullptr, nullptr, TRUE,
                        BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si1, &pi1)) {
        fprintf(stderr, "[FAIL] 无法启动分片工作进程 1 (错误码 %lu)\n", GetLastError());
        TerminateProcess(pi0.hProcess, 1);
        CloseHandle(pi0.hProcess);
        CloseHandle(pi0.hThread);
        return 1;
    }

    HANDLE handles[2] = { pi0.hProcess, pi1.hProcess };
    DWORD code0 = STILL_ACTIVE, code1 = STILL_ACTIVE;
    const auto wait_start = std::chrono::steady_clock::now();
    const double max_wall_sec = std::max(120.0, total_dur * 60.0);
    bool timed_out = false;

    while (true) {
        DWORD wr = WaitForMultipleObjects(2, handles, FALSE, 500);
        if (wr == WAIT_FAILED) {
            fprintf(stderr, "[FAIL] WaitForMultipleObjects 句柄等待异常 (错误码 %lu)，终止分片...\n", GetLastError());
            TerminateProcess(pi0.hProcess, 1);
            TerminateProcess(pi1.hProcess, 1);
            code0 = code1 = 1;
            break;
        }
        GetExitCodeProcess(pi0.hProcess, &code0);
        GetExitCodeProcess(pi1.hProcess, &code1);
        if (code0 != STILL_ACTIVE && code0 != 0) {
            fprintf(stderr, "[FAIL] 分片工作进程 0 异常失败 (退出码 %lu)，终止分片 1...\n", code0);
            TerminateProcess(pi1.hProcess, 1);
            break;
        }
        if (code1 != STILL_ACTIVE && code1 != 0) {
            fprintf(stderr, "[FAIL] 分片工作进程 1 异常失败 (退出码 %lu)，终止分片 0...\n", code1);
            TerminateProcess(pi0.hProcess, 1);
            break;
        }
        if (code0 != STILL_ACTIVE && code1 != STILL_ACTIVE) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s = std::chrono::duration<double>(now - wait_start).count();
        if (elapsed_s > max_wall_sec) {
            fprintf(stderr, "[FAIL] 分片工作进程超时挂死 (已耗时 %.1f 秒 > 限制 %.1f 秒)，强制终止...\n", elapsed_s, max_wall_sec);
            TerminateProcess(pi0.hProcess, 1);
            TerminateProcess(pi1.hProcess, 1);
            timed_out = true;
            code0 = code1 = 1;
            break;
        }
    }

    CloseHandle(pi0.hProcess);
    CloseHandle(pi0.hThread);
    CloseHandle(pi1.hProcess);
    CloseHandle(pi1.hThread);

    if (code0 != 0 || code1 != 0 || timed_out) {
        fprintf(stderr, "[FAIL] 分片处理异常退出 (分片0: %lu, 分片1: %lu%s)\n", code0, code1, timed_out ? ", 超时" : "");
        _wremove(out_part0.c_str());
        _wremove(out_part1.c_str());
        _wremove(concat_list_path.c_str());
        return 1;
    }

    fprintf(stderr, "[parallel] 各分片处理完成，正在极速拼合并封装音频...\n");
    fflush(stderr);

    // Write concat file in UTF-8
    FILE *fconcat = _wfopen(concat_list_path.c_str(), L"wb");
    if (!fconcat) {
        fprintf(stderr, "[FAIL] 无法创建合并列表文件\n");
        _wremove(out_part0.c_str());
        _wremove(out_part1.c_str());
        return 1;
    }
    std::wstring p0_norm = out_part0;
    std::wstring p1_norm = out_part1;
    for (auto &c : p0_norm) if (c == L'\\') c = L'/';
    for (auto &c : p1_norm) if (c == L'\\') c = L'/';

    auto to_u8 = [](const std::wstring &w) -> std::string {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return "";
        std::string s((size_t)n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    };

    std::string line0 = "file '" + to_u8(p0_norm) + "'\r\n";
    std::string line1 = "file '" + to_u8(p1_norm) + "'\r\n";
    fwrite(line0.data(), 1, line0.size(), fconcat);
    fwrite(line1.data(), 1, line1.size(), fconcat);
    fclose(fconcat);

    // Concat & mux audio
    std::wstring concat_cmd;
    if (!options.audio || params.audio_codec.empty() || params.audio_codec == "none") {
        concat_cmd = tool_cmd(false) + L" -y -nostdin -v error -f concat -safe 0 -i \"" +
                     concat_list_path + L"\" -c:v copy -an \"" + options.output + L"\"";
    } else {
        std::wstring audio_args;
        if (params.audio_codec == "wmapro" || params.audio_codec == "wmav2" ||
            params.audio_codec == "wmavoice" || params.audio_codec == "pcm_s16le" ||
            params.audio_codec == "pcm_s24le" || params.audio_codec == "alac") {
            audio_args = L"-c:a aac -b:a 192k ";
        } else {
            audio_args = L"-c:a copy ";
        }
        concat_cmd = tool_cmd(false) + L" -y -nostdin -v error -f concat -safe 0 -i \"" +
                     concat_list_path + L"\" -i \"" + options.input +
                     L"\" -map 0:v -map 1:a? -c:v copy " + audio_args +
                     L"\"" + options.output + L"\"";
    }

    STARTUPINFOW csi{};
    csi.cb = sizeof(csi);
    PROCESS_INFORMATION cpi{};
    if (CreateProcessW(nullptr, concat_cmd.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &csi, &cpi)) {
        WaitForSingleObject(cpi.hProcess, INFINITE);
        DWORD ccode = 1;
        GetExitCodeProcess(cpi.hProcess, &ccode);
        CloseHandle(cpi.hProcess);
        CloseHandle(cpi.hThread);
        if (ccode != 0) {
            fprintf(stderr, "[FAIL] FFmpeg concat 拼合失败 (代码 %lu)\n", ccode);
            _wremove(out_part0.c_str());
            _wremove(out_part1.c_str());
            _wremove(concat_list_path.c_str());
            return 1;
        }
    } else {
        fprintf(stderr, "[FAIL] 无法启动 FFmpeg concat 命令\n");
        _wremove(out_part0.c_str());
        _wremove(out_part1.c_str());
        _wremove(concat_list_path.c_str());
        return 1;
    }

    _wremove(out_part0.c_str());
    _wremove(out_part1.c_str());
    _wremove(concat_list_path.c_str());

    fprintf(stderr, "[parallel] 🚀 分片并行转码全部完成！输出已保存至：%ls\n", options.output.c_str());
    fflush(stderr);
    return 0;
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
    if (!parse_args(argc, argv, options)) return 1;
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
    unsigned model_w = params.width & ~1u;
    unsigned model_h = params.height & ~1u;
    if (options.model_scale > 0.0 && options.model_scale < 1.0) {
        model_w = (unsigned)std::lround(params.width * options.model_scale);
        model_h = (unsigned)std::lround(params.height * options.model_scale);
        model_w = (std::max)(64u, model_w & ~1u);
        model_h = (std::max)(64u, model_h & ~1u);
    }
    unsigned output_w = (std::max)(2u, (unsigned)std::lround(params.width * options.upscale));
    unsigned output_h = (std::max)(2u, (unsigned)std::lround(params.height * options.upscale));
    output_w &= ~1u;
    output_h &= ~1u;
    if (options.model_scale != 1.0)
        fprintf(stderr, "[model-scale] ratio=%.2f model_res=%ux%u\n", options.model_scale, model_w, model_h);
    if (options.upscale != 1.0)
        fprintf(stderr, "[upscale] ratio=%.3f output=%ux%u\n", options.upscale, output_w, output_h);

    // Check parallel processing mode
    const bool requested_parallel = (options.parallel_mode == "auto" || options.parallel_mode == "2");
    if (!options.is_child_chunk && requested_parallel) {
        dlssnr_budget::HardwareInfo budget = dlssnr_budget::detect_hardware_budget(
            params.duration, params.width, params.height);
        fprintf(stderr, "%s\n", budget.summary.c_str());
        fflush(stderr);

        int concurrency = (options.parallel_mode == "2") ? 2 : budget.recommended_concurrency;
        if (concurrency >= 2 && params.duration >= 15.0) {
            return run_parallel_orchestrator(argc, argv, options, params, budget, concurrency);
        }
    }

    double start_s = 0.0, dur_s = 0.0;
    if (options.is_child_chunk) {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        start_s = std::max(0.0, options.chunk_start_sec - options.warmup_sec);
        dur_s = options.chunk_duration_sec + (options.chunk_start_sec - start_s);
        options.warmup_discard_frames = (unsigned)std::round((options.chunk_start_sec - start_s) * params.fps);
    }

    // --- start ffmpeg decode + encode sides ------------------------------
    // Decoder: the child writes its raw frames to the pipe, so the write end
    // is the inherited one; we only read.
    Pipe decoder_stdout;
    if (!decoder_stdout.make(false, true)) { fprintf(stderr, "[FAIL] pipe\n"); return 1; }
    ChildProcess decoder;
    const bool need_decode_scale = (model_w != params.width || model_h != params.height);
    if (!start_decoder(options.input, decoder_stdout, decoder,
                       need_decode_scale ? model_w : 0,
                       need_decode_scale ? model_h : 0,
                       start_s, dur_s, options.ffmpeg_threads,
                       &error)) {
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
            const size_t fbytes = (size_t)model_w * model_h * 6;
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
                fg.compute(frame.data(), model_w, model_h, fi == 0, mv);
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
                       encoder_stdin, encoder, model_w, model_h, output_w, output_h, options.encoder,
                       options.ffmpeg_threads, options.is_child_chunk /* video_only */)) {
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
    if (!options.dlss_model_preset.empty() && options.dlss_model_preset != "default" && options.dlss_model_preset != "默认") {
        SetEnvironmentVariableA("DLSS_PRESET", options.dlss_model_preset.c_str());
        fprintf(stderr, "[model] DLSS model preset: %s\n", options.dlss_model_preset.c_str());
    } else {
        SetEnvironmentVariableA("DLSS_PRESET", nullptr);
    }

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
    detector.configure(model_w, model_h, options.cut_threshold);
    CpuFlow flowgen; // motion-vector estimator for temporal guidance
    GpuFlow gpuflow; // D3D12 compute estimator; CPU remains the fallback
    bool gpu_flow_ready = false;
    const bool flow_stats = [] {
        const char *value = std::getenv("DLSS_FLOW_STATS");
        return value && value[0] && std::strcmp(value, "0") != 0;
    }();
    if (options.flow) {
        // Keep the flow command queue independent until cross-queue fence
        // handoff is fully validated; this is the stable GPU path.
        // Keep the flow queue independent from the DLSS queue. Sharing the
        // queue reuses command execution state owned by the processor and can
        // corrupt the second frame on ZLUDA when both allocators are reset.
        gpu_flow_ready = gpuflow.init(model_w, model_h,
                                      processor.native_device(), nullptr);
        fprintf(stderr, "[flow] %s\n", gpu_flow_ready ?
                "D3D12 GPU motion guide enabled (quarter-resolution compute)" :
                "D3D12 GPU motion guide unavailable; using CPU fallback");
    }

    const size_t frame_bytes = (size_t)model_w * model_h * 6;
    enhancer::Settings settings = options.settings;
    settings.output_width = model_w;
    settings.output_height = model_h;
    settings.is_video = true;
    if (settings.passes >= 2) {
        fprintf(stderr, "[multipass] 2-pass cascaded dual-engine enabled (flicker-free independent temporal features)\n");
    }
    if (options.comp_opts.is_active()) {
        fprintf(stderr, "[composite] 画面合成与防起雾已开启: 混合=%.0f%%, 细节=%.0f%%, 暗部保护=%.0f%%, 高光=%.0f%%\n",
                options.comp_opts.output_mix * 100.0f,
                options.comp_opts.detail_boost * 100.0f,
                options.comp_opts.shadow_protect * 100.0f,
                options.comp_opts.glow_control * 100.0f);
        fflush(stderr);
    }

    std::vector<unsigned> flow_luma;
    std::vector<short> qflow;

    // --- 3-stage asynchronous pipeline (Triple-Buffering) ----------------
    constexpr size_t POOL_SIZE = 3;
    Channel<std::shared_ptr<InputFrame>> free_input_pool(POOL_SIZE);
    Channel<std::shared_ptr<InputFrame>> ready_input_channel(POOL_SIZE);
    Channel<std::shared_ptr<OutputFrame>> free_output_pool(POOL_SIZE);
    Channel<std::shared_ptr<OutputFrame>> ready_output_channel(POOL_SIZE);

    ID3D12Device *d3d_dev = processor.native_device();
    bool zero_copy_in = false;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        auto in_f = std::make_shared<InputFrame>();
        in_f->raw_bytes.resize(frame_bytes);
        if (d3d_dev) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC rdesc{};
            rdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rdesc.Width = frame_bytes;
            rdesc.Height = 1;
            rdesc.DepthOrArraySize = 1;
            rdesc.MipLevels = 1;
            rdesc.SampleDesc.Count = 1;
            rdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(d3d_dev->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &rdesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&in_f->upload_buf))) && in_f->upload_buf) {
                if (SUCCEEDED(in_f->upload_buf->Map(0, nullptr, &in_f->mapped_ptr)) && in_f->mapped_ptr) {
                    zero_copy_in = true;
                    in_f->raw_bytes.clear();
                    in_f->raw_bytes.shrink_to_fit();
                }
            }
        }
        if (!in_f->mapped_ptr || options.comp_opts.is_active()) {
            in_f->in.width = model_w;
            in_f->in.height = model_h;
            in_f->in.pixels.resize((size_t)model_w * model_h * 4);
        }
        if (options.flow) {
            in_f->motion.width = model_w;
            in_f->motion.height = model_h;
            in_f->motion.pixels.resize((size_t)model_w * model_h * 2);
        }
        free_input_pool.push(in_f);

        auto out_f = std::make_shared<OutputFrame>();
        out_f->out.width = model_w;
        out_f->out.height = model_h;
        out_f->out.pixels.resize((size_t)model_w * model_h * 4);
        out_f->model_output.resize(frame_bytes);
        free_output_pool.push(out_f);
    }
    if (zero_copy_in) {
        if (options.comp_opts.is_active()) {
            fprintf(stderr, "[zero-copy] D3D12 mapped upload heap enabled (GPU Direct Pipe + CPU Composite Channel)\n");
        } else {
            fprintf(stderr, "[zero-copy] D3D12 mapped upload heap enabled (direct pipe-to-GPU, CPU LUT bypass)\n");
        }
    }


    std::atomic<bool> abort_pipeline{false};
    std::atomic<int> reset_count{0};
    std::atomic<int> blank_count{0};
    std::atomic<double> elapsed_total{0.0};
    std::atomic<unsigned> frames_written{0};
    std::atomic<unsigned> frames_written_count{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> retryable{false};
    const auto pipeline_began = std::chrono::steady_clock::now();

    // Stage 1: Decoder reader & preprocessor thread
    std::thread decode_thread([&]() {
        unsigned dec_index = 0;
        while (!abort_pipeline.load()) {
            if (options.max_frames && (int)dec_index >= options.max_frames) {
                if (decoder.process) TerminateProcess(decoder.process, 0);
                break;
            }

            std::shared_ptr<InputFrame> frame;
            if (!free_input_pool.pop(frame)) break;
            if (abort_pipeline.load()) break;

            unsigned char *dst_raw = frame->mapped_ptr ? (unsigned char *)frame->mapped_ptr : frame->raw_bytes.data();
            size_t got = 0;
            while (got < frame_bytes && !abort_pipeline.load()) {
                DWORD chunk = 0;
                if (!ReadFile(decoder_stdout.read_end, dst_raw + got,
                              (DWORD)(frame_bytes - got), &chunk, nullptr) || chunk == 0) {
                    break;
                }
                got += chunk;
            }

            if (got == 0) {
                // Clean end of video
                break;
            }
            if (got != frame_bytes) {
                if (!abort_pipeline.load()) {
                    fprintf(stderr, "[FAIL] short read at frame %u (%zu of %zu bytes)\n", dec_index, got, frame_bytes);
                    failed = true;
                    abort_pipeline.store(true);
                }
                break;
            }

            frame->index = dec_index;
            frame->is_eos = false;

            // Reset determination
            bool reset = false;
            switch (options.reset) {
                case ResetMode::Always: reset = true; break;
                case ResetMode::Never: reset = (dec_index == 0); break;
                case ResetMode::Every: reset = (dec_index % options.reset_every) == 0; break;
                case ResetMode::Auto: reset = detector.consider(dst_raw, model_w, model_h); break;
            }
            frame->reset = reset;

            // Format conversion: GPU Compute Shader handles format conversion if upload_buf is mapped.
            // When post-composite is active, CPU also needs in_frame->in in linear half-float RGBA
            // to composite with out_frame->out.
            if (!frame->mapped_ptr || options.comp_opts.is_active()) {
                rgb48_to_half_rgba(dst_raw, frame->in);
            }
            if (frame->mapped_ptr) {
                frame->raw_bytes.clear();
                frame->raw_bytes.shrink_to_fit();
            }

            ready_input_channel.push(frame);
            ++dec_index;
        }

        // Send EOS frame to Stage 2
        auto eos_in = std::make_shared<InputFrame>();
        eos_in->is_eos = true;
        ready_input_channel.push(eos_in);
    });

    // Stage 3: Encoder writer thread
    std::thread encode_thread([&]() {
        IWICImagingFactory *wic = nullptr;
        bool dump_ok = false;
        if (!options.dump_dir.empty()) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            dump_ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                                 CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
        }
        while (!abort_pipeline.load()) {
            std::shared_ptr<OutputFrame> frame;
            if (!ready_output_channel.pop(frame)) break;
            if (frame->is_eos) {
                break;
            }

            if (options.is_child_chunk && frame->index < options.warmup_discard_frames) {
                free_output_pool.push(frame);
                continue;
            }

            bool blank = false;
            half_rgba_to_rgb48(frame->out, frame->model_output.data(), blank);
            if (blank) {
                blank_count.fetch_add(1);
                fprintf(stderr, "[warn] frame %u output looks blank\n", frame->index);
                failed = true;
                retryable = true;
                abort_pipeline.store(true);
                break;
            }

            DWORD written = 0;
            if (!WriteFile(encoder_stdin.write_end, frame->model_output.data(), (DWORD)frame_bytes,
                           &written, nullptr) || written != frame_bytes) {
                if (!abort_pipeline.load()) {
                    fprintf(stderr, "[FAIL] encoder pipe broke at frame %u\n", frame->index);
                    failed = true;
                    abort_pipeline.store(true);
                }
                break;
            }

            unsigned display_idx = frame->index;
            if (options.is_child_chunk) {
                int diff = (int)frame->index - (int)options.warmup_discard_frames;
                display_idx = (diff > 0 ? (unsigned)diff : 0) + options.frame_index_offset;
            }

            if (!options.dump_dir.empty() && dump_ok) {
                wchar_t name[512];
                swprintf(name, 512, L"%s\\frame_%05u.png", options.dump_dir.c_str(), display_idx);
                if (!dump_png(wic, name, frame->out))
                    fprintf(stderr, "[warn] could not dump frame %u\n", display_idx);
            }

            frames_written.store(display_idx + 1);
            frames_written_count.fetch_add(1);
            if ((display_idx % 5) == 0 || (frame->index < 5)) {
                fprintf(stderr, "[%.5u] %.0f ms (avg %.1f, reset=%d, blanks=%d)\n",
                        display_idx, frame->ms,
                        elapsed_total.load() / (frame->index + 1),
                        reset_count.load(), blank_count.load());
                fflush(stderr);
            }

            free_output_pool.push(frame);
        }
        if (wic) wic->Release();
        if (!options.dump_dir.empty()) {
            CoUninitialize();
        }
    });

    // Stage 2: GPU inference (Main thread)
    unsigned eval_index = 0;
    while (!abort_pipeline.load()) {
        std::shared_ptr<InputFrame> in_frame;
        if (!ready_input_channel.pop(in_frame)) break;
        if (in_frame->is_eos) {
            // Forward EOS to Stage 3
            auto eos_out = std::make_shared<OutputFrame>();
            eos_out->is_eos = true;
            ready_output_channel.push(eos_out);
            break;
        }

        std::shared_ptr<OutputFrame> out_frame;
        if (!free_output_pool.pop(out_frame)) break;
        if (abort_pipeline.load()) break;

        if (in_frame->reset) reset_count.fetch_add(1);
        settings.reset_accumulation = in_frame->reset;

        // GPU motion vectors if enabled
        ID3D12Resource *gpu_motion = nullptr;
        unsigned gpu_motion_pitch = 0;
        const bool useFlow = options.flow;
        if (useFlow && gpu_flow_ready) {
            if (in_frame->reset) gpuflow.reset();
            const unsigned qw = (model_w + 3) / 4, qh = (model_h + 3) / 4;
            flow_luma.resize((size_t)qw * qh);
            const unsigned char *data_ptr = in_frame->mapped_ptr ? (const unsigned char *)in_frame->mapped_ptr : in_frame->raw_bytes.data();
            for (unsigned y = 0; y < qh; ++y) for (unsigned x = 0; x < qw; ++x) {
                unsigned px = std::min(model_w - 1, x * 4u);
                unsigned py = std::min(model_h - 1, y * 4u);
                const uint16_t *p = (const uint16_t *)(data_ptr + ((size_t)py * model_w + px) * 6);
                flow_luma[(size_t)y * qw + x] = ((unsigned)p[0] * 19595u + (unsigned)p[1] * 38470u + (unsigned)p[2] * 7471u) >> 16;
            }
            bool generated = gpuflow.compute(flow_luma, qflow);
            if (generated) {
                CpuFlow::median3(qflow, qw, qh);
                CpuFlow::upscale4(qflow, qw, qh, model_w, model_h, in_frame->motion.pixels);
            } else {
                in_frame->motion.pixels.assign((size_t)model_w * model_h * 2, 0);
            }
        }

        const auto began = std::chrono::steady_clock::now();
        bool proc_ok = false;
        if (in_frame->upload_buf) {
            proc_ok = processor.process_raw_rgb48(
                in_frame->upload_buf, model_w, model_h, out_frame->out, settings, error,
                gpu_motion ? nullptr : (useFlow ? &in_frame->motion : nullptr),
                gpu_motion, gpu_motion_pitch);
        } else {
            proc_ok = processor.process(
                in_frame->in, out_frame->out, settings, error,
                gpu_motion ? nullptr : (useFlow ? &in_frame->motion : nullptr),
                gpu_motion, gpu_motion_pitch);
        }
        if (!proc_ok) {

            fprintf(stderr, "[FAIL] frame %u: %s\n", in_frame->index, error.c_str());
            if (error.find("blank image") != std::string::npos) {
                retryable = true;
            }
            failed = true;
            abort_pipeline.store(true);
            break;
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - began)
                              .count();
        elapsed_total.store(elapsed_total.load() + ms);

        // Check if first frame came out blank (ZLUDA sticky race)
        if (in_frame->index == 0 && output_is_blank(out_frame->out)) {
            fprintf(stderr, "[FAIL] 首帧输出空白（本轮竞态），交由外层重跑\n");
            failed = true;
            retryable = true;
            abort_pipeline.store(true);
            break;
        }

        out_frame->index = in_frame->index;
        out_frame->ms = ms;
        out_frame->is_eos = false;

        if (options.comp_opts.is_active()) {
            apply_post_composite(in_frame->in, out_frame->out, options.comp_opts);
        }

        // Return input frame buffer to free pool for reuse
        free_input_pool.push(in_frame);

        // Send output frame to encoder
        ready_output_channel.push(out_frame);
        ++eval_index;

        // TDR Prevention & DWM responsiveness:
        // High resolution (1080p, 1440p, 4K) or multipass keeps the GPU hardware queue heavily loaded.
        // Yielding 1-2 ms gives the Windows Desktop Window Manager (DWM) and display driver
        // a guaranteed scheduling window to present desktop frames, completely eliminating
        // Windows TDR Watchdog timeouts (LiveKernelEvent 0x141).
        if (options.yield_ms > 0) {
            Sleep((DWORD)options.yield_ms);
        } else if (ms > 300.0) {
            // Adaptive yield for heavy frames
            Sleep(2);
        }
    }

    if (abort_pipeline.load()) {
        if (decoder.process) TerminateProcess(decoder.process, 1);
        if (encoder.process) TerminateProcess(encoder.process, 1);
        if (decode_thread.native_handle()) CancelSynchronousIo((HANDLE)decode_thread.native_handle());
        if (encode_thread.native_handle()) CancelSynchronousIo((HANDLE)encode_thread.native_handle());
        free_input_pool.close();
        ready_input_channel.close();
        free_output_pool.close();
        ready_output_channel.close();
    }

    if (decode_thread.joinable()) decode_thread.join();
    if (encode_thread.joinable()) encode_thread.join();

    const double wall_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - pipeline_began)
                               .count();

    // --- finish the pipes safely AFTER threads have joined ---
    decoder_stdout.close();
    encoder_stdin.close();

    DWORD decoder_code = 1, encoder_code = 1;
    wait_exit(decoder.process, 15000, decoder_code);
    wait_exit(encoder.process, 60000, encoder_code);

    processor.stop();
    decoder.close();
    encoder.close();

    if (!failed.load() && decoder_code != 0) {
        fprintf(stderr, "[FAIL] ffmpeg decoder exited with code %u\n", decoder_code);
        failed.store(true);
    }
    if (!failed.load() && encoder_code != 0) {
        fprintf(stderr, "[FAIL] ffmpeg encoder exited with code %u\n", encoder_code);
        failed.store(true);
    }

    const unsigned total_done = frames_written_count.load();
    const double total_eval_ms = elapsed_total.load();
    fprintf(stderr,
            "[done] %u frames in %.2f s (%.2f fps throughput, avg GPU %.1f ms), resets=%d, blanks=%d%s\n",
            total_done, wall_ms / 1000.0,
            wall_ms > 0 ? (1000.0 * total_done / wall_ms) : 0.0,
            total_done ? (total_eval_ms / total_done) : 0.0,
            reset_count.load(), blank_count.load(),
            failed.load() ? " -- FAILED" : "");
    if (blank_count.load() > 0) {
        failed.store(true);
        retryable.store(true);
    }
    // 2 = retryable failure (blank race) -> outer main() re-runs this program.
    return failed.load() ? (retryable.load() ? 2 : 1) : 0;
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
static int real_main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        usage();
        return 0;
    }

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
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
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

int main(int argc, char **argv) {
    // Detect GPU architecture and inject RDNA 4 environment if needed
    dlssnr::auto_configure_gpu_environment();

#if defined(_WIN32)
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv && wargc > 0) {
        std::vector<std::string> utf8_args(wargc);
        std::vector<char *> new_argv(wargc + 1);
        for (int i = 0; i < wargc; ++i) {
            int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                utf8_args[i].resize((size_t)len - 1);
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, utf8_args[i].data(), len, nullptr, nullptr);
            }
            new_argv[i] = utf8_args[i].data();
        }
        new_argv[wargc] = nullptr;
        LocalFree(wargv);
        return real_main(wargc, new_argv.data());
    }
    if (wargv) LocalFree(wargv);
#endif
    return real_main(argc, argv);
}

