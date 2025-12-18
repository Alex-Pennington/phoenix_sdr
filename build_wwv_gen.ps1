# Phoenix SDR - WWV Generator Build Script
# Minimal build script for wwv_gen.exe only

param([switch]$Clean, [switch]$Release)

$ErrorActionPreference = "Stop"

# Configuration
$CC = "C:\msys64\mingw64\bin\gcc.exe"
$BuildDir = "build_wwv"
$BinDir = "bin"

# Check compiler
if (-not (Test-Path $CC)) {
    Write-Host "ERROR: GCC not found at $CC" -ForegroundColor Red
    Write-Host "Please install MSYS2 MinGW64 toolchain" -ForegroundColor Yellow
    exit 1
}

# Add MSYS2 to PATH for DLL dependencies
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"

# Clean if requested
if ($Clean) {
    Write-Host "Cleaning..." -ForegroundColor Cyan
    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    if (Test-Path "$BinDir\wwv_gen.exe") { Remove-Item -Force "$BinDir\wwv_gen.exe" }
    Write-Host "Clean complete." -ForegroundColor Green
    exit 0
}

# Create directories
if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }
if (-not (Test-Path $BinDir)) { New-Item -ItemType Directory -Path $BinDir | Out-Null }

# Compiler flags
$CFLAGS = @(
    "-std=c17",
    "-Wall",
    "-Wextra",
    "-Iinclude"
)

if ($Release) {
    $CFLAGS += @("-O2", "-DNDEBUG")
    Write-Host "Building wwv_gen (Release)..." -ForegroundColor Cyan
} else {
    $CFLAGS += @("-O0", "-g", "-D_DEBUG")
    Write-Host "Building wwv_gen (Debug)..." -ForegroundColor Cyan
}

$LDFLAGS = @("-lm", "-lws2_32")

function Compile-Source($source) {
    $objName = [System.IO.Path]::GetFileNameWithoutExtension($source)
    $objPath = "$BuildDir\$objName.o"

    Write-Host "  Compiling $source..." -ForegroundColor Gray

    $allArgs = $CFLAGS + @("-c", "-o", $objPath, $source)

    # Run compiler using Start-Process to properly handle stderr
    $tempOut = [System.IO.Path]::GetTempFileName()
    $tempErr = [System.IO.Path]::GetTempFileName()
    $proc = Start-Process -FilePath $CC -ArgumentList $allArgs -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $tempOut -RedirectStandardError $tempErr
    $stderr = Get-Content $tempErr -ErrorAction SilentlyContinue
    Remove-Item $tempOut,$tempErr -ErrorAction SilentlyContinue

    # Show warnings
    if ($stderr -and $proc.ExitCode -eq 0) {
        $stderr | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
    }

    if ($proc.ExitCode -ne 0) {
        Write-Host "ERROR: Compilation failed for $source" -ForegroundColor Red
        if ($stderr) {
            $stderr | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        }
        exit 1
    }

    return $objPath
}

try {
    Push-Location $PSScriptRoot

    # Compile all source files
    Write-Host "Compiling source files..." -ForegroundColor Cyan
    $objects = @(
        (Compile-Source "src\iqr_meta.c"),
        (Compile-Source "src\iq_recorder.c"),
        (Compile-Source "tools\oscillator.c"),
        (Compile-Source "tools\bcd_encoder.c"),
        (Compile-Source "tools\wwv_signal.c"),
        (Compile-Source "tools\wwv_gen.c")
    )

    # Link executable
    Write-Host "Linking wwv_gen.exe..." -ForegroundColor Cyan
    $linkArgs = @("-o", "$BinDir\wwv_gen.exe") + $objects + $LDFLAGS

    $tempOut = [System.IO.Path]::GetTempFileName()
    $tempErr = [System.IO.Path]::GetTempFileName()
    $proc = Start-Process -FilePath $CC -ArgumentList $linkArgs -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $tempOut -RedirectStandardError $tempErr
    $stderr = Get-Content $tempErr -ErrorAction SilentlyContinue
    Remove-Item $tempOut,$tempErr -ErrorAction SilentlyContinue

    if ($proc.ExitCode -ne 0) {
        Write-Host "ERROR: Linking failed" -ForegroundColor Red
        if ($stderr) {
            $stderr | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        }
        exit 1
    }

    Write-Host ""
    Write-Host "SUCCESS: Built $BinDir\wwv_gen.exe" -ForegroundColor Green
    Write-Host ""
    Write-Host "Usage examples:" -ForegroundColor Cyan
    Write-Host "  File mode:" -ForegroundColor Yellow
    Write-Host "    .\bin\wwv_gen.exe -o test.iqr" -ForegroundColor Gray
    Write-Host "    .\bin\wwv_gen.exe -t 12:00 -d 355 -y 25 -s wwv -o wwv_test.iqr" -ForegroundColor Gray
    Write-Host "  TCP mode (compatible with waterfall.exe):" -ForegroundColor Yellow
    Write-Host "    .\bin\wwv_gen.exe -p 4536" -ForegroundColor Gray
    Write-Host "    .\bin\wwv_gen.exe -p 4536 -c  # continuous" -ForegroundColor Gray
    Write-Host ""
}
catch {
    Write-Host "BUILD FAILED: $_" -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
