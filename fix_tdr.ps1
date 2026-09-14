# Windows GPU TDR (Timeout Detection & Recovery) Fix Utility
# Recommended for heavy AI workloads (DLSS-NR, Stable Diffusion, ComfyUI, etc.)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

Write-Host "===============================================================" -ForegroundColor Cyan
Write-Host "       Windows GPU TDR (Timeout Detection & Recovery) Fix      " -ForegroundColor Cyan
Write-Host "===============================================================" -ForegroundColor Cyan
Write-Host ""

$regPath = "HKLM:\System\CurrentControlSet\Control\GraphicsDrivers"

$currentTdrDelay = (Get-ItemProperty -Path $regPath -Name "TdrDelay" -ErrorAction SilentlyContinue).TdrDelay
$currentTdrDdiDelay = (Get-ItemProperty -Path $regPath -Name "TdrDdiDelay" -ErrorAction SilentlyContinue).TdrDdiDelay
$currentTdrLevel = (Get-ItemProperty -Path $regPath -Name "TdrLevel" -ErrorAction SilentlyContinue).TdrLevel

Write-Host "[Current Status]" -ForegroundColor Yellow
if ($null -eq $currentTdrDelay) {
    Write-Host "  * TdrDelay: <Not Set> (Windows Default: 2 seconds)" -ForegroundColor Red
    Write-Host "    [WARNING] 2 seconds is too short for heavy DLSS-NR / 4K multi-pass rendering." -ForegroundColor Red
    Write-Host "              This easily causes AMD/NVIDIA driver timeouts and crashes (0x141)!" -ForegroundColor Red
} else {
    Write-Host "  * TdrDelay: $currentTdrDelay seconds" -ForegroundColor $(if ($currentTdrDelay -ge 8) { "Green" } else { "Yellow" })
}

if ($null -eq $currentTdrDdiDelay) {
    Write-Host "  * TdrDdiDelay: <Not Set> (Windows Default: 2 seconds)" -ForegroundColor Yellow
} else {
    Write-Host "  * TdrDdiDelay: $currentTdrDdiDelay seconds" -ForegroundColor $(if ($currentTdrDdiDelay -ge 8) { "Green" } else { "Yellow" })
}

# TdrLevel was read but never shown, and then overwritten unconditionally below.
# TdrLevel = 0 means TDR detection is switched off on purpose, which is a common
# thing to do on a machine used for long compute jobs; re-enabling it silently
# would change that machine's behaviour with no warning and no record.
if ($null -eq $currentTdrLevel) {
    Write-Host "  * TdrLevel: <Not Set> (Windows Default: 3 = enabled, auto-recovery)" -ForegroundColor Gray
} else {
    Write-Host "  * TdrLevel: $currentTdrLevel" -ForegroundColor $(if ($currentTdrLevel -eq 0) { "Red" } else { "White" })
    if ($currentTdrLevel -eq 0) {
        Write-Host "    [WARNING] TDR detection is currently DISABLED on this machine (TdrLevel = 0)." -ForegroundColor Red
        Write-Host "              This script will re-enable it (TdrLevel = 3). If that was deliberate," -ForegroundColor Red
        Write-Host "              stop now and set it back with the backup written below." -ForegroundColor Red
    }
}

Write-Host ""

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "[Notice] Applying TDR registry changes requires Administrator privileges." -ForegroundColor Yellow
    Write-Host "Restarting script with Administrator elevation..." -ForegroundColor Cyan
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}

Write-Host "[Applying Recommended Settings (TdrDelay = 10s, TdrDdiDelay = 10s)]..." -ForegroundColor Cyan

# Record what is being replaced before replacing it. The previous values were
# only ever overwritten, so a machine that had been tuned (a longer TdrDelay, or
# TdrLevel = 0) had no way back to it.
$backup = Join-Path (Split-Path -Parent $PSCommandPath) "tdr_backup_before_fix.txt"
try {
    @(
        "# Windows GPU TDR settings before fix_tdr.ps1 ran, $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
        "# Registry: $regPath"
        "# Restore by hand (Administrator), or use the reg commands below."
        "TdrDelay    = $currentTdrDelay"
        "TdrDdiDelay = $currentTdrDdiDelay"
        "TdrLevel    = $currentTdrLevel"
        ""
        "# reg add `"HKLM\System\CurrentControlSet\Control\GraphicsDrivers`" /v TdrDelay /t REG_DWORD /d <value> /f"
    ) | Set-Content -LiteralPath $backup -Encoding UTF8
    Write-Host ("  previous values backed up to: {0}" -f $backup) -ForegroundColor Gray
} catch {
    Write-Host ("  [WARNING] could not write the backup file: {0}" -f $_) -ForegroundColor Yellow
}

try {
    Set-ItemProperty -Path $regPath -Name "TdrDelay" -Value 10 -Type DWord -Force
    Set-ItemProperty -Path $regPath -Name "TdrDdiDelay" -Value 10 -Type DWord -Force
    Set-ItemProperty -Path $regPath -Name "TdrLevel" -Value 3 -Type DWord -Force

    Write-Host ""
    Write-Host "[SUCCESS] Windows GPU TDR configuration updated successfully!" -ForegroundColor Green
    Write-Host "  * TdrDelay    = 10 (GPU commands now have up to 10s to complete without false-positive driver reset)" -ForegroundColor Green
    Write-Host "  * TdrDdiDelay = 10" -ForegroundColor Green
    Write-Host "  * TdrLevel    = 3 (Auto-recovery)" -ForegroundColor Green
    Write-Host ""
    Write-Host "[NOTE] Changes take effect after restarting Windows (or restarting the display driver)." -ForegroundColor Yellow
} catch {
    Write-Host "[ERROR] Failed to write registry: $_" -ForegroundColor Red
}

Write-Host "Press any key to exit..."
[Console]::ReadKey() | Out-Null
