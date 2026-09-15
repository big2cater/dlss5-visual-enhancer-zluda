param(
    [string]$TestOverride = "",
    [switch]$IsolatedOnly,
    # Probe mode: load exactly one DLL in this process and report one line.
    # The parent uses this for every probe, because loading two DLLs with the
    # same base name (System32's amdhip64_7.dll and the one under HIP_PATH) into
    # one process is not reliable: the loader keys on the base name, and HIP's
    # runtime caches its device list after the first successful call, so the
    # second probe can be answered by the first module's state. Freeing the
    # library in between is not enough to make that safe -- which is why every
    # probe below runs in its own process instead.
    [string]$Probe = "",
    [string]$ProbeKind = ""
)

# Applied before anything loads a HIP or CUDA module, in every mode. The override
# only has an effect on a runtime that has not initialised yet, so setting it after
# the first hipGetDeviceCount would silently test nothing.
if ($TestOverride) {
    [System.Environment]::SetEnvironmentVariable("HSA_OVERRIDE_GFX_VERSION", $TestOverride, "Process")
    $env:HSA_OVERRIDE_GFX_VERSION = $TestOverride
}

# Defined here, at the top, because every section below needs it. It used to be
# assigned in section [5], several hundred lines after section [3b] started using
# it, so [3b] enumerated a null path and always reported "no local amdhip64*.dll"
# -- the one section whose job is to catch a stray DLL in the application
# directory, which is the cause the FAQ points at for cuInit 100.
$currentDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $currentDir) { $currentDir = (Get-Location).Path }

$code = @"
using System;
using System.Runtime.InteropServices;

public class HipDiag {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibrary(string lpFileName);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    public static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool FreeLibrary(IntPtr hModule);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int hipGetDeviceCountDelegate(out int count);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int hipDeviceGetNameDelegate(byte[] name, int len, int device);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int cuInitDelegate(uint flags);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int cuDeviceGetCountDelegate(out int count);

    public static string TestHip(string path) {
        IntPtr h = LoadLibrary(path);
        if (h == IntPtr.Zero) {
            int err = Marshal.GetLastWin32Error();
            return "LoadLibrary failed (win32 error: " + err + ")";
        }
        try {
            IntPtr pFunc = GetProcAddress(h, "hipGetDeviceCount");
            if (pFunc == IntPtr.Zero) {
                return "hipGetDeviceCount not exported";
            }
            var func = (hipGetDeviceCountDelegate)Marshal.GetDelegateForFunctionPointer(pFunc, typeof(hipGetDeviceCountDelegate));
            int count = -1;
            int res = func(out count);
            string ret = "hipGetDeviceCount -> status=" + res + " (" + (res == 0 ? "hipSuccess" : (res == 100 ? "hipErrorNoDevice" : "Error " + res)) + "), count=" + count;
            if (res == 0 && count > 0) {
                IntPtr pName = GetProcAddress(h, "hipDeviceGetName");
                if (pName != IntPtr.Zero) {
                    var funcName = (hipDeviceGetNameDelegate)Marshal.GetDelegateForFunctionPointer(pName, typeof(hipDeviceGetNameDelegate));
                    byte[] nameBuf = new byte[256];
                    funcName(nameBuf, 256, 0);
                    string devName = System.Text.Encoding.ASCII.GetString(nameBuf).TrimEnd('\0');
                    ret += " [GPU 0: " + devName + "]";
                }
            }
            return ret;
        } finally {
            FreeLibrary(h);
        }
    }

    public static string TestZluda(string path) {
        IntPtr h = LoadLibrary(path);
        if (h == IntPtr.Zero) {
            int err = Marshal.GetLastWin32Error();
            return "LoadLibrary failed (win32 error: " + err + ")";
        }
        try {
            IntPtr pInit = GetProcAddress(h, "cuInit");
            if (pInit == IntPtr.Zero) {
                return "cuInit not exported";
            }
            var cuInit = (cuInitDelegate)Marshal.GetDelegateForFunctionPointer(pInit, typeof(cuInitDelegate));
            int res = cuInit(0);
            string ret = "cuInit(0) -> status=" + res + " (" + (res == 0 ? "CUDA_SUCCESS" : (res == 100 ? "CUDA_ERROR_NO_DEVICE" : "Error " + res)) + ")";
            IntPtr pCount = GetProcAddress(h, "cuDeviceGetCount");
            if (pCount != IntPtr.Zero) {
                var cuCount = (cuDeviceGetCountDelegate)Marshal.GetDelegateForFunctionPointer(pCount, typeof(cuDeviceGetCountDelegate));
                int count = 0;
                // The count alone is not the answer: report the call's status too,
                // or a failing cuDeviceGetCount reads as a plain zero.
                int cRes = cuCount(out count);
                ret += ", cuDeviceGetCount -> status=" + cRes + ", count=" + count;
            }
            return ret;
        } finally {
            FreeLibrary(h);
        }
    }
}
"@

Add-Type -TypeDefinition $code -ErrorAction Stop

# ---------------------------------------------------------------- probe mode
if ($Probe) {
    $kind = if ($ProbeKind) { $ProbeKind } else { "hip" }
    $res = if ($kind -eq "zluda") { [HipDiag]::TestZluda($Probe) } else { [HipDiag]::TestHip($Probe) }
    Write-Output "$kind|$Probe|$res"
    exit 0
}

function Invoke-Probe([string]$path, [string]$kind, [string]$override) {
    $childArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $scriptPath, "-Probe", $path, "-ProbeKind", $kind)
    if ($override) { $childArgs += @("-TestOverride", $override) }
    $out = & powershell.exe @childArgs 2>&1
    if (-not $out) { return "$kind|$path|(no output)" }
    return ($out | Where-Object { $_ -like "$kind|*" } | Select-Object -First 1)
}

function Show-Probe([string]$path, [string]$kind, [string]$label, [string]$override) {
    $line = Invoke-Probe $path $kind $override
    $res = ($line -split '\|', 3)[2]
    $colour = "Gray"
    if ($kind -eq "zluda") { $colour = if ($res -like "*CUDA_SUCCESS*" -and $res -notlike "*count=0*") { "Green" } else { "Red" } }
    # Same rule as the zluda branch above. hipSuccess with count=0 is exactly what a
    # broken HSA_OVERRIDE looks like on this card, and painting that green told the
    # reader the HIP side was fine when the whole point of the sweep below is to find
    # that case. The probe reports count= for both kinds.
    elseif ($kind -eq "hip") { $colour = if ($res -like "*status=0*" -and $res -notlike "*count=0*") { "Green" } else { "Red" } }
    Write-Host ("    {0}: {1}" -f $label, $res) -ForegroundColor $colour
    return $res
}

if ($IsolatedOnly) {
    # One fresh process that loads the system HIP runtime and the local ZLUDA
    # library, with whatever override was passed in already in place.
    $p = "C:\Windows\System32\amdhip64_7.dll"
    if (-not (Test-Path $p)) { $p = "C:\Windows\System32\amdhip64_6.dll" }
    $hipRes = if (Test-Path $p) { [HipDiag]::TestHip($p) } else { "HIP DLL not found" }
    $zludaDll = Join-Path $currentDir "run\nvcuda.dll"
    if (-not (Test-Path $zludaDll)) { $zludaDll = Join-Path $currentDir "nvcuda.dll" }
    $zRes = if (Test-Path $zludaDll) { [HipDiag]::TestZluda($zludaDll) } else { "" }
    Write-Output "$hipRes | ZLUDA: $zRes"
    exit 0
}

$scriptPath = $PSCommandPath
if (-not $scriptPath) { $scriptPath = Join-Path $currentDir "diagnose_gpu.ps1" }

Write-Host "===============================================================" -ForegroundColor Cyan
Write-Host "         AMD GPU / HIP / ZLUDA Diagnostic Tool (RDNA 3 / 4)    " -ForegroundColor Cyan
Write-Host "===============================================================" -ForegroundColor Cyan

# 1. System GPUs
Write-Host "`n[1] Detected Video Controllers:" -ForegroundColor Yellow
Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host ("    * " + $_.Name + " | Driver: " + $_.DriverVersion + " (" + $_.DriverDate + ")")
}

# 2. Environment Variables
Write-Host "`n[2] Environment Variables:" -ForegroundColor Yellow
$hipPath = [System.Environment]::GetEnvironmentVariable("HIP_PATH", "Machine")
if (-not $hipPath) { $hipPath = [System.Environment]::GetEnvironmentVariable("HIP_PATH", "User") }
if (-not $hipPath) { $hipPath = $env:HIP_PATH }
Write-Host ("    HIP_PATH: " + $(if ($hipPath) { $hipPath } else { "<Not Set>" })) -ForegroundColor $(if ($hipPath) { "White" } else { "Gray" })

foreach ($name in @("HSA_OVERRIDE_GFX_VERSION", "AMD_DIRECT_DISPATCH", "ZLUDA_NVAPI_GPU_ARCH")) {
    $v = [System.Environment]::GetEnvironmentVariable($name, "Machine")
    $where = "Machine"
    if (-not $v) { $v = [System.Environment]::GetEnvironmentVariable($name, "User"); $where = "User" }
    if (-not $v) { $v = [System.Environment]::GetEnvironmentVariable($name, "Process"); $where = "Process" }
    if ($v) {
        Write-Host ("    {0}: {1}   (from {2})" -f $name, $v, $where) -ForegroundColor White
    } else {
        Write-Host ("    {0}: <Not Set>" -f $name) -ForegroundColor Gray
    }
}
Write-Host "    Note: the program sets HSA_OVERRIDE_GFX_VERSION itself on RDNA 4 and prints" -ForegroundColor Gray
Write-Host "          what it did through OutputDebugString, which a GUI does not show." -ForegroundColor Gray

# 3. HIP DLLs that could be picked up, each in its own process
Write-Host "`n[3] HIP runtime DLLs in C:\Windows\System32 (each probed in its own process):" -ForegroundColor Yellow
$systemHip = @(Get-ChildItem "C:\Windows\System32" -Filter "amdhip64*.dll" -ErrorAction SilentlyContinue)
if ($systemHip) {
    foreach ($f in $systemHip) {
        Write-Host ("    [Found] {0}  ({1:n0} B, {2:yyyy-MM-dd HH:mm})" -f $f.FullName, $f.Length, $f.LastWriteTime) -ForegroundColor Green
        [void](Show-Probe $f.FullName "hip" "  probe")
    }
} else {
    Write-Host "    [Missing] no amdhip64*.dll in System32" -ForegroundColor Red
}

Write-Host "`n[3b] HIP runtime DLLs beside this script (a local copy overrides System32):" -ForegroundColor Yellow
$localHip = @(Get-ChildItem $currentDir -Filter "amdhip64*.dll" -ErrorAction SilentlyContinue)
if ($localHip) {
    foreach ($f in $localHip) {
        Write-Host ("    [Local Override Found] {0}  ({1:n0} B)" -f $f.FullName, $f.Length) -ForegroundColor Magenta
        [void](Show-Probe $f.FullName "hip" "  probe")
    }
} else {
    Write-Host "    No local amdhip64*.dll in the application directory (normal: the system driver is used)" -ForegroundColor Gray
}

$whereHip = where.exe amdhip64_7.dll 2>$null
if ($whereHip) {
    Write-Host "    [where.exe amdhip64_7.dll]:" -ForegroundColor White
    $whereHip | ForEach-Object { Write-Host "        $_" -ForegroundColor Gray }
}

# 4. HIP_PATH
if ($hipPath) {
    Write-Host "`n[4] HIP runtime DLLs under HIP_PATH\bin:" -ForegroundColor Yellow
    $hipBin = Join-Path $hipPath "bin"
    $hipDlls = @(Get-ChildItem $hipBin -Filter "amdhip64*.dll" -ErrorAction SilentlyContinue)
    if ($hipDlls) {
        foreach ($f in $hipDlls) {
            Write-Host ("    [Found] {0}  ({1:n0} B, {2:yyyy-MM-dd HH:mm})" -f $f.FullName, $f.Length, $f.LastWriteTime) -ForegroundColor Green
            [void](Show-Probe $f.FullName "hip" "  probe")
        }
    } else {
        Write-Host ("    [Not Found] no amdhip64*.dll under {0}" -f $hipBin) -ForegroundColor Gray
    }
}

# which one the program would actually use: application directory, then System32, then PATH
Write-Host "`n[4b] Which HIP runtime the program would load (search order):" -ForegroundColor Yellow
$picked = $null
if ($localHip) { $picked = $localHip[0] }
elseif ($systemHip) { $picked = $systemHip[0] }
else { $picked = Get-Command amdhip64_7.dll -ErrorAction SilentlyContinue }
if ($picked) {
    $pickedPath = if ($picked.FullName) { $picked.FullName } else { $picked.Source }
    Write-Host ("    {0}" -f $pickedPath) -ForegroundColor White
    if ($localHip) { Write-Host "    (a copy in the application directory wins over System32 -- check that it matches your driver)" -ForegroundColor Yellow }
} else {
    Write-Host "    none found in the application directory, System32 or PATH" -ForegroundColor Red
}

# 5. ZLUDA
$zludaDll = Join-Path $currentDir "run\nvcuda.dll"
if (-not (Test-Path $zludaDll)) { $zludaDll = Join-Path $currentDir "nvcuda.dll" }
Write-Host "`n[4c] Last GPU auto-configuration lines the program wrote (this is what to send in a report):" -ForegroundColor Yellow
$autoLog = Join-Path $env:TEMP "dlssnr_gpu_autoconfig.log"
if (Test-Path $autoLog) {
    Write-Host ("    {0}" -f $autoLog) -ForegroundColor Gray
    Get-Content $autoLog -ErrorAction SilentlyContinue | Select-Object -Last 12 | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor White }
} else {
    Write-Host ("    (no {0} yet -- the program writes it when it starts; run it once)" -f $autoLog) -ForegroundColor Gray
}

Write-Host "`n[5] Local ZLUDA nvcuda.dll (its own process):" -ForegroundColor Yellow
if (Test-Path $zludaDll) {
    Write-Host ("    Testing: {0}" -f $zludaDll) -ForegroundColor White
    [void](Show-Probe $zludaDll "zluda" "Result")
} else {
    Write-Host ("    [Not Found] nvcuda.dll in {0} or {0}\run" -f $currentDir) -ForegroundColor Yellow
}

# 6. Override simulation
Write-Host "`n[6] Override Simulation (one clean process per value, override set before anything loads):" -ForegroundColor Yellow
foreach ($ver in @("12.0.1", "12.0.0", "11.0.0")) {
    $line = Invoke-Probe $zludaDll "zluda" $ver
    $res = ($line -split '\|', 3)[2]
    $ok = ($res -like "*CUDA_SUCCESS*") -and ($res -notlike "*count=0*")
    $label = if ($ok) { "[OK]  " } else { "[FAIL]" }
    Write-Host ("    {0} HSA_OVERRIDE_GFX_VERSION={1} -> {2}" -f $label, $ver, $res) -ForegroundColor $(if ($ok) { "Green" } else { "Red" })
}
Write-Host "    Note: a green line means the runtime sees a device with that override. It does" -ForegroundColor Gray
Write-Host "          not prove that a network module can be compiled and loaded -- on RDNA 4" -ForegroundColor Gray
Write-Host "          that step is where a precompile failure shows up, and this tool cannot" -ForegroundColor Gray
Write-Host "          reach it. Use the program's own warm-up for that." -ForegroundColor Gray
Write-Host "          A green line also does not mean the value is the right one for the card:" -ForegroundColor Gray
Write-Host "          an unrecognised value can simply be ignored. (Measured: 99.0.0 still" -ForegroundColor Gray
Write-Host "          reported one device.) Treat a green line as 'not obviously broken'." -ForegroundColor Gray

Write-Host "`n===============================================================" -ForegroundColor Cyan
Write-Host "Diagnosis complete." -ForegroundColor Cyan
try {
    if ([Environment]::UserInteractive -and -not [Console]::IsInputRedirected) {
        Write-Host "Press any key to exit..."
        [void][System.Console]::ReadKey($true)
    }
} catch {}
