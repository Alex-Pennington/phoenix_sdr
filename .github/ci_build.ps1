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
$VersionFile = "$IncludeDir\version.h"

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

function Update-VersionHeader {
    # Read current version.h
    $content = Get-Content $VersionFile -Raw
    
    # Extract current values
    $major = 0; $minor = 2; $patch = 5; $build = 0
    if ($content -match 'PHOENIX_VERSION_MAJOR\s+(\d+)') { $major = [int]$matches[1] }
    if ($content -match 'PHOENIX_VERSION_MINOR\s+(\d+)') { $minor = [int]$matches[1] }
    if ($content -match 'PHOENIX_VERSION_PATCH\s+(\d+)') { $patch = [int]$matches[1] }
    if ($content -match 'PHOENIX_VERSION_BUILD\s+(\d+)') { $build = [int]$matches[1] }
    
    # Increment build number
    $build++
    
    # Get git info
    $commit = "unknown"
    try {
        $commit = (git rev-parse --short HEAD 2>$null).Trim()
    } catch { }
    
    $dirty = $false
    try {
        $status = git status --porcelain 2>$null
        if ($status) { $dirty = $true }
    } catch { }
    
    $dirtyFlag = if ($dirty) { "-dirty" } else { "" }
    $versionString = "$major.$minor.$patch"
    $fullVersion = "$versionString+$build.$commit$dirtyFlag"
    
    $newContent = @"
/**
 * @file version.h
 * @brief Phoenix SDR version information
 * 
 * Version format: MAJOR.MINOR.PATCH+BUILD.COMMIT[-dirty]
 * Example: 0.2.5+9.abc1234 or 0.2.5+9.abc1234-dirty
 * 
 * Build number increments every build. Commit hash from git.
 */

#ifndef PHOENIX_VERSION_H
#define PHOENIX_VERSION_H

#define PHOENIX_VERSION_MAJOR   $major
#define PHOENIX_VERSION_MINOR   $minor
#define PHOENIX_VERSION_PATCH   $patch
#define PHOENIX_VERSION_BUILD   $build
#define PHOENIX_VERSION_STRING  "$versionString"
#define PHOENIX_VERSION_FULL    "$fullVersion"
#define PHOENIX_GIT_COMMIT      "$commit"
#define PHOENIX_GIT_DIRTY       $($dirty.ToString().ToLower())

/* Build timestamp - set by compiler */
#define PHOENIX_BUILD_DATE      __DATE__
#define PHOENIX_BUILD_TIME      __TIME__

#include <stdio.h>

static inline void print_version(const char *tool_name) {
    printf("%s v%s (built %s %s)\n", 
           tool_name, PHOENIX_VERSION_FULL, 
           PHOENIX_BUILD_DATE, PHOENIX_BUILD_TIME);
}

#endif /* PHOENIX_VERSION_H */
"@
    
    Set-Content -Path $VersionFile -Value $newContent -NoNewline
    Write-Status "Version: $fullVersion"
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

    # Generate version.h (not in repo, auto-generated)
    Update-VersionHeader

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
