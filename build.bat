@echo off
REM Configures and builds with MSVC. Qt's MSVC kit is required: see the note in
REM CMakeLists.txt for why MinGW is refused rather than merely discouraged.
setlocal
if "%QT_DIR%"=="" (
    if exist "C:\Qt\6.10.3\msvc2022_64\lib\cmake\Qt6" (
        set "QT_DIR=C:\Qt\6.10.3\msvc2022_64"
    ) else if exist "C:\Qt\6.11.2\msvc2022_64\lib\cmake\Qt6" (
        set "QT_DIR=C:\Qt\6.11.2\msvc2022_64"
    ) else (
        set "QT_DIR=C:\Qt\6.10.3\msvc2022_64"
    )
)
if "%VS%"=="" (
    if exist "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VS=D:\Program Files\Microsoft Visual Studio\18\Community"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
    ) else if exist "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VS=D:\Program Files\Microsoft Visual Studio\2022\Community"
    ) else (
        set "VS=D:\Program Files\Microsoft Visual Studio\18\Community"
    )
)
if not exist "%QT_DIR%\lib\cmake\Qt6" (
    echo Qt for MSVC was not found at "%QT_DIR%".
    echo Install the "MSVC 2022 64-bit" component with the Qt Maintenance Tool,
    echo or set QT_DIR to where it is.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%"
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1
"%QT_DIR%\bin\windeployqt.exe" --release --compiler-runtime --no-translations --no-opengl-sw --no-system-dxc-compiler --no-network --exclude-plugins qsvg build\dlssnr_gui.exe
if errorlevel 1 exit /b 1

REM A clean copy of just what running the program needs, separate from
REM build\, which keeps CMake's and Ninja's own bookkeeping (CMakeFiles,
REM build.ninja, the *_autogen staging directories Qt's MOC step uses) so
REM later builds stay incremental. Deleting any of that from build\ itself
REM would work for one clean folder, but the next build would have to start
REM over from nothing -- full reconfigure, every source recompiled, Qt's
REM generated sources rebuilt from scratch -- for every change from then on.
REM
REM /MIR mirrors, so a file windeployqt drops on one run (say, before a flag
REM above was added) does not linger in dist\ after a later run stops
REM producing it.
REM
REM The mirror is also the reason dist\ had accumulated development material:
REM build\ holds rebuild logs, ELF and module dumps, profiler output and the
REM smoke test binary from working in this tree, and /MIR carried all of it into
REM what both this script and tools/package_release.py treat as the shipped
REM folder. It is excluded by category -- the dump directories and the dev-only
REM extensions -- rather than by filename, so a new rebuild_log17.txt or a new
REM isa\ dump does not have to be added here by hand.
REM
REM One consequence worth knowing: /XD and /XF exclude on both sides, so an item
REM already sitting in dist\ is neither copied nor deleted. Changing this list
REM means cleaning dist\ once by hand; from then on the mirror holds it clean.
robocopy build dist /MIR /NFL /NDL /NJH /NJS ^
    /XD CMakeFiles nvngx_autogen dlssnr_gui_autogen video_filter_autogen processor_smoke_autogen .qt zluda ^
        isa trace zluda-modules cache-override-test bin ^
    /XF CMakeCache.txt build.ninja cmake_install.cmake *.pdb *.lib *.exp .ninja_log .ninja_deps ^
        *.txt *.png Makefile *.bat processor_smoke.exe
REM robocopy's own exit codes are a bitmask where 0-7 all mean success (0 =
REM nothing needed copying); only 8 and above is a real failure.
if errorlevel 8 exit /b 1

REM The three build products also live in run\, which is scratch space for test
REM inputs and hand-staged DLLs and is not managed by anything. A stale copy of
REM video_filter.exe there is easy to run by accident and easy to mistake for a
REM fresh build -- it happened: a fix was built, verified and packaged while
REM run\ still held the previous day's exe. Refresh only the products this build
REM owns; nvcuda.dll, nvapi64.dll and nvngx_dlssnr.dll in that folder are staged
REM by hand and must not be touched. /Y because a plain copy prompts, and in a
REM non-interactive shell that prompt is answered by defaulting to "no".
if exist run\ (
    copy /Y build\video_filter.exe run\video_filter.exe >nul
    if errorlevel 1 exit /b 1
    copy /Y build\dlssnr_gui.exe run\dlssnr_gui.exe >nul
    if errorlevel 1 exit /b 1
    copy /Y build\nvngx.dll run\nvngx.dll >nul
    if errorlevel 1 exit /b 1
)
exit /b 0
