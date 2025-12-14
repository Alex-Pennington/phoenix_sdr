# Phoenix SDR - CI Build Script
# Simplified build for GitHub Actions environment

param(
    [switch]$Release
)

$ErrorActionPreference = "Stop"

# Configuration
$ProjectName = "phoenix_sdr"
$SrcDir = "src"
$IncludeDir = "include"
$BuildDir = "build"
$BinDir = "bin"

# CI-specific paths (set up by workflow)
$SDL2Include = "$PSScriptRoot\..\include\SDL2"
$SDL2Lib = "$PSScriptRoot\..\lib"

# SDRplay stub for CI
$SDRplayInclude = "$PSScriptRoot\..\include\sdrplay"

# Compiler from PATH (set up by setup-mingw action)
$CC = "gcc"

function Write-Status($msg) {
    Write-Host "[$ProjectName] " -ForegroundColor Cyan -NoNewline
    Write-Host $msg
}

function Build-Object($source, $extraFlags) {
    $objName = [System.IO.Path]::GetFileNameWithoutExtension($source)
    $objPath = "$BuildDir\$objName.o"

    Write-Status "Compiling $source..."
    $cmd = "$CC $($CFLAGS -join ' ') $($extraFlags -join ' ') -c -o `"$objPath`" `"$source`""
    Write-Host $cmd -ForegroundColor DarkGray
    Invoke-Expression $cmd
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $source" }
    return $objPath
}

try {
    Push-Location (Join-Path $PSScriptRoot "..")
    
    Write-Status "CI Build starting..."
    Write-Status "GCC version: $(gcc --version | Select-Object -First 1)"

    # Set compiler flags
    $CFLAGS = @(
        "-std=c17",
        "-Wall",
        "-Wextra",
        "-I`"$IncludeDir`"",
        "-I`"$SDRplayInclude`""
    )

    if ($Release) {
        $CFLAGS += @("-O2", "-DNDEBUG")
        Write-Status "Release build"
    } else {
        $CFLAGS += @("-O0", "-g", "-D_DEBUG")
        Write-Status "Debug build"
    }

    # Create directories
    if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }
    if (-not (Test-Path $BinDir)) { New-Item -ItemType Directory -Path $BinDir | Out-Null }

    # Build kiss_fft (shared)
    $kissObj = Build-Object "src\kiss_fft.c" @()

    # Build waterfall (SDL2 tool)
    Write-Status "Building waterfall..."
    $waterfallObj = Build-Object "tools\waterfall.c" @("-I`"$SDL2Include`"")

    Write-Status "Linking waterfall.exe..."
    $cmd = "$CC -o `"$BinDir\waterfall.exe`" `"$waterfallObj`" `"$kissObj`" -L`"$SDL2Lib`" -lmingw32 -lSDL2main -lSDL2 -lm"
    Write-Host $cmd -ForegroundColor DarkGray
    Invoke-Expression $cmd
    if ($LASTEXITCODE -ne 0) { throw "Linking failed for waterfall" }
    Write-Status "Built: $BinDir\waterfall.exe"

    # Build DSP tests
    Write-Status "Building test_dsp..."
    $testDspObj = Build-Object "test\test_dsp.c" @()
    
    Write-Status "Linking test_test_dsp.exe..."
    $cmd = "$CC -o `"$BinDir\test_test_dsp.exe`" `"$testDspObj`" -lm"
    Write-Host $cmd -ForegroundColor DarkGray
    Invoke-Expression $cmd
    if ($LASTEXITCODE -ne 0) { throw "Linking failed for test_dsp" }
    Write-Status "Built: $BinDir\test_test_dsp.exe"

    Write-Status "CI Build complete."
}
catch {
    Write-Host "BUILD FAILED: $_" -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
