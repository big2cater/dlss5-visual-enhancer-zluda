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

Write-Host ""

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "[Notice] Applying TDR registry changes requires Administrator privileges." -ForegroundColor Yellow
    Write-Host "Restarting script with Administrator elevation..." -ForegroundColor Cyan
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}

Write-Host "[Applying Recommended Settings (TdrDelay = 10s, TdrDdiDelay = 10s)]..." -ForegroundColor Cyan

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
