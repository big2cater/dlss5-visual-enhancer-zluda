// One still image through the DLSS 5 neural rendering network.
//
// This is the same path the command line tool uses and the ReShade addon runs
// in a game: it links addon/dlss_cuda.cpp unchanged. Nothing here is a second
// implementation of the DLSS side -- that layer is the one that has been made
// to work, and a rewrite would only be a second thing to get wrong.
//
// Windows only. The network ships as a Windows library; on Linux this runs
// under Wine or Proton like any other Windows program.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace enhancer {

// The controls, with the values the addon starts from.
//
// Global tone and skin structure start at zero. A single run with each at one
// produced a flattened picture, but the evaluation is not reproducible -- the
// same settings give a good frame or a flat one -- so that reading is not
// trustworthy and zero is the cautious choice rather than a measured one.
struct Settings {
    float intensity = 1.0f;
    float global_tone = 0.0f;
    float local_tone = 1.0f;
    float local_structure = 1.0f;
    float skin_structure = 0.0f;
    int style = 0;   // 0 default, 1 natural, 2 cinematic
    int preset = 0;  // 0 leaves the choice to the network
    bool auto_mask = true;
    // Repeats the evaluation on the same picture. The network blends with its
    // own previous output, so for a still image this is the nearest thing to a
    // scene standing still.
    int passes = 1;
    // Optional final output size. Zero keeps the input dimensions.
    unsigned output_width = 0;
    unsigned output_height = 0;

    // Reset the network's accumulation history before the first pass of this
    // call. Keeping this true matches the still-image behaviour: every call
    // starts from a fresh history. Setting it false makes the history chain
    // across calls instead -- a video keeps one feature session for all its
    // frames, and chaining is what stops motion from flickering. Scene cuts
    // then need an explicit reset, which the video tool detects by default.
    bool reset_accumulation = true;
};

// Where the pieces are. All of them belong to someone else; the program never
// guesses at a location.
struct Paths {
    std::wstring snippet;     // nvngx_dlssnr.dll
    std::wstring cuda_driver; // nvcuda.dll, or ZLUDA standing in for it
    std::wstring ngx_runtime; // nvngx.dll
    std::wstring nvapi;       // nvapi64.dll
};

// Four half-float channels a pixel, rows packed tight: what the network reads
// and writes, and what the interface converts to and from.
struct Image {
    std::vector<uint16_t> pixels;
    unsigned width = 0;
    unsigned height = 0;

    bool empty() const { return pixels.empty() || width == 0 || height == 0; }
    size_t row_bytes() const { return (size_t)width * 8; }
};

class Processor {
public:
    Processor();
    ~Processor();
    Processor(const Processor &) = delete;
    Processor &operator=(const Processor &) = delete;

    // Brings up Direct3D, the CUDA driver and the network. On anything but a
    // real NVIDIA driver this first translates the network's code in parallel
    // (see precompile.h) rather than leaving it to happen serially, one module
    // at a time, wherever the network first reaches for it. `log`, if given, is
    // called with a line of progress at a time as that runs; it may be called
    // from this thread only, never concurrently.
    bool start(const Paths &paths, std::string &error,
              const std::function<void(const std::string &)> &log = {});
    bool started() const;
    void stop();

    // Runs `in` through the network into `out`. Rebuilds the network if the
    // image size changed since the last call.
    //
    // `motion` is an optional per-pixel motion-vector map at full resolution:
    // two half-floats (x, y) per pixel, the backward screen-space displacement
    // in pixels (where the current pixel was in the previous frame). Passing
    // it gives the network temporal guidance for reprojection, which stops
    // ghosting/swimming when chaining video frames. Null keeps the current
    // (unguided) behaviour -- zero motion.
    bool process(const Image &in, Image &out, const Settings &settings, std::string &error,
                 const Image *motion = nullptr, ID3D12Resource *motion_gpu = nullptr,
                 unsigned motion_gpu_row_pitch = 0);

    // Native D3D12 objects for zero-copy auxiliary passes (for example a GPU
    // motion guide). Borrowed pointers; Processor retains ownership.
    ID3D12Device *native_device() const;
    ID3D12CommandQueue *native_queue() const;

    // Milliseconds the last call spent, and what the CUDA device calls itself.
    double last_ms() const;
    const std::string &device_name() const;

private:
    struct State;
    State *s;
};

} // namespace enhancer
