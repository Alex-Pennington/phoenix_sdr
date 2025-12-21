#!/usr/bin/env pwsh
# Local CI Environment Simulator
# Mirrors GitHub Actions build exactly to catch issues before pushing

param(
    [switch]$Clean,
    [switch]$Setup,
    [switch]$Build
)

$ErrorActionPreference = "Stop"

Write-Host "=== Phoenix SDR - Local CI Simulator ===" -ForegroundColor Cyan

# Check GCC version
$gccVersion = (gcc --version | Select-String -Pattern "(\d+\.\d+\.\d+)").Matches.Groups[1].Value
Write-Host "Current GCC: $gccVersion" -ForegroundColor Yellow

if ($gccVersion -notlike "12.2.*") {
    Write-Host ""
    Write-Host "WARNING: CI uses GCC 12.2.0, you have $gccVersion" -ForegroundColor Red
    Write-Host "Install matching version:" -ForegroundColor Yellow
    Write-Host "  choco install mingw --version=12.2.0 --force" -ForegroundColor White
    Write-Host "  -OR-" -ForegroundColor Yellow
    Write-Host "  Download from: https://github.com/niXman/mingw-builds-binaries/releases" -ForegroundColor White
    Write-Host ""
    $continue = Read-Host "Continue anyway? (y/n)"
    if ($continue -ne 'y') { exit 1 }
}

# Clean CI artifacts
if ($Clean) {
    Write-Host "Cleaning CI artifacts..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "ci_test"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "include\sdrplay"
    Write-Host "Clean complete" -ForegroundColor Green
    exit 0
}

# Setup CI environment
if ($Setup -or $Build) {
    Write-Host "Setting up CI environment..." -ForegroundColor Yellow
    
    # Create CI test directory
    New-Item -ItemType Directory -Force -Path "ci_test" | Out-Null
    
    # Setup stub SDRplay headers (exactly as CI does)
    Write-Host "  - Creating SDRplay stub headers..." -ForegroundColor Gray
    New-Item -ItemType Directory -Force -Path "include\sdrplay" | Out-Null
    Copy-Item ".github\sdrplay_api_stub.h" -Destination "include\sdrplay\sdrplay_api.h" -Force
    
    Write-Host "Setup complete" -ForegroundColor Green
}

# Build using CI script
if ($Build) {
    Write-Host ""
    Write-Host "Building with CI script..." -ForegroundColor Yellow
    Write-Host "  Script: .github\ci_build.ps1" -ForegroundColor Gray
    Write-Host "  Compiler: GCC $gccVersion" -ForegroundColor Gray
    Write-Host "  Headers: Stub (include\sdrplay\sdrplay_api.h)" -ForegroundColor Gray
    Write-Host ""
    
    # Run the exact CI build script
    & .\.github\ci_build.ps1 -Release
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        Write-Host "=== BUILD SUCCESS ===" -ForegroundColor Green
        Write-Host "All executables built successfully with CI environment" -ForegroundColor Green
        Write-Host ""
        Write-Host "Built files:" -ForegroundColor Cyan
        Get-ChildItem bin\*.exe | ForEach-Object {
            $size = [math]::Round($_.Length / 1KB, 1)
            Write-Host "  $($_.Name) ($size KB)" -ForegroundColor White
        }
    } else {
        Write-Host ""
        Write-Host "=== BUILD FAILED ===" -ForegroundColor Red
        Write-Host "Fix errors above before pushing to GitHub" -ForegroundColor Red
        exit 1
    }
}

if (-not $Setup -and -not $Build -and -not $Clean) {
    Write-Host ""
    Write-Host "Usage:" -ForegroundColor Cyan
    Write-Host "  .\test_ci_local.ps1 -Setup       # Setup CI environment (stub headers)" -ForegroundColor White
    Write-Host "  .\test_ci_local.ps1 -Build       # Setup + build with CI script" -ForegroundColor White
    Write-Host "  .\test_ci_local.ps1 -Clean       # Remove CI artifacts" -ForegroundColor White
    Write-Host ""
    Write-Host "Recommended workflow:" -ForegroundColor Yellow
    Write-Host "  1. .\test_ci_local.ps1 -Build    # Test CI build locally" -ForegroundColor Gray
    Write-Host "  2. git add .; git commit -m '...' # Commit changes" -ForegroundColor Gray
    Write-Host "  3. git push                       # Push to GitHub" -ForegroundColor Gray
    Write-Host "  4. .\test_ci_local.ps1 -Clean    # Restore real SDK" -ForegroundColor Gray
    Write-Host ""
}
