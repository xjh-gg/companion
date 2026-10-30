# Companion build & verify script
# Usage: powershell -ExecutionPolicy Bypass -File tools/build.ps1 [-target esp32s3] [-NoTarget] [-Clean]

param(
    [string]$Target = "esp32s3",
    [switch]$NoTarget,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$ProfileCandidates = @()

if ($env:IDF_PROFILE) {
    $ProfileCandidates += $env:IDF_PROFILE
}
$ProfileCandidates += "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"

$idfProfile = $ProfileCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $idfProfile) {
    Write-Host "[build.ps1] ERROR: ESP-IDF profile not found." -ForegroundColor Red
    exit 1
}

Write-Output ("[build.ps1] Loading IDF: " + $idfProfile)
. $idfProfile *> $null

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Host "[build.ps1] ERROR: idf.py unavailable." -ForegroundColor Red
    exit 1
}
Write-Output ("[build.ps1] IDF_PATH = " + ${env:IDF_PATH})

$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root
try {
    if ($Clean) {
        Write-Host "[build.ps1] Running fullclean ..." -ForegroundColor Cyan
        idf.py fullclean *> $null
    }
    if (-not $NoTarget) {
        Write-Output ("[build.ps1] set-target: " + $Target)
        idf.py set-target $Target *> $null
    }
    Write-Host "[build.ps1] Running idf.py build ..." -ForegroundColor Cyan
    idf.py build
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        Write-Host "[build.ps1] BUILD OK" -ForegroundColor Green
    } else {
        Write-Output ("[build.ps1] BUILD FAILED exit=" + $code)
    }
    exit $code
}
finally {
    Pop-Location
}