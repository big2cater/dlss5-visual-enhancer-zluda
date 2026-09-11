# DLSS 5 Image Enhancer.
#
#   cmake -B build -DCMAKE_PREFIX_PATH=<Qt>/msvc2022_64
#   cmake --build build --config Release
#
# Qt is optional: without it the graphical front end is skipped and the
# console tools (video_filter) still build.

cmake_minimum_required(VERSION 3.19)
project(dlss5-image-enhancer LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "This program can only be built for Windows: the DLSS network is a Windows ")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 QUIET COMPONENTS Widgets)

if(Qt6_FOUND)
    add_executable(dlss5-image-enhancer WIN32
        gui/main.cpp
        gui/main_window.cpp
        gui/main_window.h
        gui/half_float.h
        core/image_processor.cpp
        core/image_processor.h
        core/precompile.cpp
        core/precompile.h
        dlss_layer/dlss_cuda.cpp
        dlss_layer/frame_blit.cpp
    )

    target_include_directories(dlss5-image-enhancer PRIVATE dlss_layer)
    target_link_libraries(dlss5-image-enhancer PRIVATE Qt6::Widgets d3d12 dxgi d3dcompiler)

    # A console alongside the window: the DLSS layer and the network both report
    # through standard error, and those messages are the only explanation when
    # something is refused.
    target_compile_definitions(dlss5-image-enhancer PRIVATE UNICODE _UNICODE NOMINMAX)

    # The batch front end: Qt 6 modern unified GUI (dlssnr_gui). It
    # holds no processing of its own -- it builds the command line and drives
    # video_filter.exe, which is why it needs none of the D3D or DLSS libraries
    # the other target links.
    add_executable(dlssnr_gui WIN32
        gui/batch_main.cpp
        gui/batch_window.cpp
        gui/batch_window.h
        gui/compare_view.cpp
        gui/compare_view.h
    )
    target_link_libraries(dlssnr_gui PRIVATE Qt6::Widgets)
    target_compile_definitions(dlssnr_gui PRIVATE UNICODE _UNICODE NOMINMAX)
endif()

# Console tools, no Qt. Built whenever the CMake generator can build them;
# nothing here depends on the graphical front end. video_filter is the same
# DLSS path as the GUI (image_processor + dlss_cuda unchanged) with ffmpeg
# subprocesses doing the video I/O.
add_executable(video_filter
    tools/video_filter.cpp
    core/image_processor.cpp
    core/image_processor.h
    core/precompile.cpp
    core/precompile.h
    dlss_layer/dlss_cuda.cpp
    dlss_layer/frame_blit.cpp
)
target_include_directories(video_filter PRIVATE dlss_layer gui core)
target_link_libraries(video_filter PRIVATE d3d12 dxgi d3dcompiler windowscodecs ole32 shell32)
target_compile_definitions(video_filter PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(video_filter PRIVATE /utf-8)
set_target_properties(video_filter PROPERTIES AUTOMOC OFF)

# Our own NGX runtime -- nvngx.dll, the stand-in that lets the snippet believe
# it is being driven by NVIDIA's own NGX loader. See the comment at the top of
# ngx_runtime/ngx_runtime.cpp for what it does and why it exists.
add_library(nvngx SHARED
    ngx_runtime/ngx_runtime.cpp
    ngx_runtime/ngx_cuda.h
)
target_compile_definitions(nvngx PRIVATE UNICODE _UNICODE NOMINMAX)
set_target_properties(nvngx PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:video_filter>
    # No Qt in this DLL at all, so nothing for AUTOMOC to look for -- turned
    # off here rather than left to inherit the directory-wide setting, which
    # would otherwise generate an empty nvngx_autogen/ for it to find nothing
    # in.
    AUTOMOC OFF
)