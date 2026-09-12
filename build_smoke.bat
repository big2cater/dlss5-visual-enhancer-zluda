@echo off
REM Builds the processor smoke test into .\bin. No Qt: this exercises the DLSS
REM side on its own, which is the half that can actually fail.
setlocal
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
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (
    echo Visual Studio vcvars64.bat was not found at "%VS%".
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

set ROOT=%~dp0
set OUT=%ROOT%bin
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DUNICODE /D_UNICODE /utf-8 ^
   /I "%ROOT%core" ^
   /I "%ROOT%dlss_layer" ^
   /I "%ROOT%gui" ^
   "%ROOT%tests\processor_smoke.cpp" "%ROOT%core\image_processor.cpp" ^
   "%ROOT%core\precompile.cpp" ^
   "%ROOT%dlss_layer\dlss_cuda.cpp" "%ROOT%dlss_layer\frame_blit.cpp" ^
   wintrust.lib d3d12.lib dxgi.lib d3dcompiler.lib windowscodecs.lib ole32.lib shell32.lib user32.lib ^
   /Fo:"%OUT%\\" /Fe:"%OUT%\processor_smoke.exe"
if errorlevel 1 exit /b 1
del "%OUT%\*.obj" >nul 2>&1
echo Built %OUT%\processor_smoke.exe
exit /b 0
