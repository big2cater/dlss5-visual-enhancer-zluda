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
        IntPtr pFunc = GetProcAddress(h, "hipGetDeviceCount");
        if (pFunc == IntPtr.Zero) {
            return "hipGetDeviceCount not exported";
        }
        var func = (hipGetDeviceCountDelegate)Marshal.GetDelegateForFunctionPointer(pFunc, typeof(hipGetDeviceCountDelegate));
        int count = -1;
        int res = func(out count);
        string ret = "hipGetDeviceCount -> status=" + res + " (" + (res == 0 ? "SUCCESS" : (res == 100 ? "hipErrorNoDevice" : "Error " + res)) + "), count=" + count;
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
    }

    public static string TestZluda(string path) {
        IntPtr h = LoadLibrary(path);
        if (h == IntPtr.Zero) {
            int err = Marshal.GetLastWin32Error();
            return "LoadLibrary failed (win32 error: " + err + ")";
        }
        IntPtr pInit = GetProcAddress(h, "cuInit");
        if (pInit == IntPtr.Zero) {
            return "cuInit not exported";
        }
        var cuInit = (cuInitDelegate)Marshal.GetDelegateForFunctionPointer(pInit, typeof(cuInitDelegate));
        int res = cuInit(0);
        string ret = "cuInit(0) -> status=" + res + " (" + (res == 0 ? "CUDA_SUCCESS" : (res == 100 ? "CUDA_ERROR_NO_DEVICE" : "Error " + res)) + ")";
        if (res == 0) {
            IntPtr pCount = GetProcAddress(h, "cuDeviceGetCount");
            if (pCount != IntPtr.Zero) {
                var cuCount = (cuDeviceGetCountDelegate)Marshal.GetDelegateForFunctionPointer(pCount, typeof(cuDeviceGetCountDelegate));
                int count = 0;
                cuCount(out count);
                ret += ", cuDeviceGetCount=" + count;
            }
        }
        return ret;
    }
}
"@

Add-Type -TypeDefinition $code -ErrorAction Stop

Write-Host "===============================================================" -ForegroundColor Cyan
Write-Host "         AMD GPU / HIP / ZLUDA Diagnostic Tool (RX 9070 XT)    " -ForegroundColor Cyan
Write-Host "===============================================================" -ForegroundColor Cyan

# 1. System GPUs
Write-Host "`n[1] Detected Video Controllers:" -ForegroundColor Yellow
Get-CimInstance Win32_VideoController | ForEach-Object {
    Write-Host ("    * " + $_.Name + " | Driver: " + $_.DriverVersion + " (" + $_.DriverDate + ")")
}

# 2. Environment Variables
Write-Host "`n[2] Environment Variables:" -ForegroundColor Yellow
$hipPath = [System.Environment]::GetEnvironmentVariable("HIP_PATH", "Machine")
if (-not $hipPath) { $hipPath = [System.Environment]::GetEnvironmentVariable("HIP_PATH", "User") }
if (-not $hipPath) { $hipPath = $env:HIP_PATH }
if ($hipPath) {
    Write-Host "    HIP_PATH: $hipPath" -ForegroundColor White
} else {
    Write-Host "    HIP_PATH: <Not Set>" -ForegroundColor Gray
}

$hsaOverride = [System.Environment]::GetEnvironmentVariable("HSA_OVERRIDE_GFX_VERSION", "Machine")
if (-not $hsaOverride) { $hsaOverride = [System.Environment]::GetEnvironmentVariable("HSA_OVERRIDE_GFX_VERSION", "User") }
if (-not $hsaOverride) { $hsaOverride = $env:HSA_OVERRIDE_GFX_VERSION }
if ($hsaOverride) {
    Write-Host "    HSA_OVERRIDE_GFX_VERSION: $hsaOverride" -ForegroundColor White
} else {
    Write-Host "    HSA_OVERRIDE_GFX_VERSION: <Not Set>" -ForegroundColor Gray
}

# 3. DLL Files in System32
Write-Host "`n[3] Testing C:\Windows\System32 DLLs:" -ForegroundColor Yellow
@("amdhip64_7.dll", "amdhip64_6.dll") | ForEach-Object {
    $p = "C:\Windows\System32\$_"
    if (Test-Path $p) {
        $item = Get-Item $p
        Write-Host "    [Found] $p (Size: $($item.Length), Modified: $($item.LastWriteTime))" -ForegroundColor Green
        $testRes = [HipDiag]::TestHip($p)
        Write-Host "            $testRes"
    } else {
        Write-Host "    [Missing] $p" -ForegroundColor Red
    }
}

# 4. DLL in HIP_PATH if exists
if ($hipPath) {
    Write-Host "`n[4] Testing HIP_PATH bin DLLs:" -ForegroundColor Yellow
    @("amdhip64_7.dll", "amdhip64_6.dll") | ForEach-Object {
        $p = Join-Path $hipPath "bin\$_"
        if (Test-Path $p) {
            $item = Get-Item $p
            Write-Host "    [Found] $p (Size: $($item.Length), Modified: $($item.LastWriteTime))" -ForegroundColor Green
            $testRes = [HipDiag]::TestHip($p)
            Write-Host "            $testRes"
        } else {
            Write-Host "    [Not Found] $p" -ForegroundColor Gray
        }
    }
}

# 5. Testing local ZLUDA nvcuda.dll
$currentDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $currentDir) { $currentDir = Get-Location }
$zludaDll = Join-Path $currentDir "run\nvcuda.dll"
if (-not (Test-Path $zludaDll)) {
    $zludaDll = Join-Path $currentDir "nvcuda.dll"
}

Write-Host "`n[5] Testing ZLUDA nvcuda.dll:" -ForegroundColor Yellow
if (Test-Path $zludaDll) {
    Write-Host "    Testing: $zludaDll" -ForegroundColor White
    $zRes = [HipDiag]::TestZluda($zludaDll)
    if ($zRes -like "*CUDA_SUCCESS*") {
        Write-Host "    Result: $zRes" -ForegroundColor Green
    } else {
        Write-Host "    Result: $zRes" -ForegroundColor Red
    }
} else {
    Write-Host "    [Not Found] nvcuda.dll in $currentDir or $currentDir\run" -ForegroundColor Yellow
}

# 6. Test with HSA_OVERRIDE_GFX_VERSION if current test failed
Write-Host "`n[6] Overrides Simulation Test:" -ForegroundColor Yellow
@("12.0.0", "12.0.1", "11.0.0") | ForEach-Object {
    $ver = $_
    [System.Environment]::SetEnvironmentVariable("HSA_OVERRIDE_GFX_VERSION", $ver, "Process")
    $p = "C:\Windows\System32\amdhip64_7.dll"
    if (-not (Test-Path $p)) { $p = "C:\Windows\System32\amdhip64_6.dll" }
    if (Test-Path $p) {
        $res = [HipDiag]::TestHip($p)
        Write-Host "    HSA_OVERRIDE_GFX_VERSION=$ver -> $res"
    }
}

Write-Host "`n===============================================================" -ForegroundColor Cyan
Write-Host "Diagnosis complete." -ForegroundColor Cyan
try {
    if ([Environment]::UserInteractive -and -not [Console]::IsInputRedirected) {
        Write-Host "Press any key to exit..."
        [void][System.Console]::ReadKey($true)
    }
} catch {}

