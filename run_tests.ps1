# Phoenix SDR - Test Runner Script
# Builds and runs all unit tests

param(
    [switch]$Verbose,
    [switch]$BuildOnly,
    [string]$Filter = "*"  # Filter test names (e.g., "tick" for tick detector tests only)
)

$ErrorActionPreference = "Stop"

# Configuration
$ProjectRoot = $PSScriptRoot
$TestDir = "$ProjectRoot\test"
$SrcDir = "$ProjectRoot\src"
$ToolsDir = "$ProjectRoot\tools"
$IncludeDir = "$ProjectRoot\include"
$BuildDir = "$ProjectRoot\build\test"
$BinDir = "$ProjectRoot\bin\test"

# SDRplay API paths
$SDRplayInclude = "C:\Program Files\SDRplay\API\inc"
$SDRplayLib = "C:\Program Files\SDRplay\API\x64"

# Compiler (MinGW/GCC via winget)
$MinGWBin = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
$CC = "$MinGWBin\gcc.exe"

function Write-Status($msg, $color = "Cyan") {
    Write-Host "[TEST] " -ForegroundColor Cyan -NoNewline
    Write-Host $msg
}

function Write-Pass($msg) {
    Write-Host "[PASS] " -ForegroundColor Green -NoNewline
    Write-Host $msg
}

function Write-Fail($msg) {
    Write-Host "[FAIL] " -ForegroundColor Red -NoNewline
    Write-Host $msg
}

function Write-Skip($msg) {
    Write-Host "[SKIP] " -ForegroundColor Yellow -NoNewline
    Write-Host $msg
}

#============================================================================
# Test Definitions
#============================================================================

$Tests = @(
    # Existing tests
    @{
        Name = "test_tcp_commands"
        Sources = @(
            "$TestDir\test_tcp_commands.c",
            "$SrcDir\tcp_commands.c",
            "$TestDir\sdr_stubs.c"
        )
        Libs = @("-lm")
        Description = "TCP command parser tests"
    },
    @{
        Name = "test_telemetry"
        Sources = @(
            "$TestDir\test_telemetry.c",
            "$ToolsDir\waterfall_telemetry.c"
        )
        Libs = @("-lws2_32", "-lm")
        Description = "UDP telemetry module tests"
    },
    @{
        Name = "test_dsp"
        Sources = @(
            "$TestDir\test_dsp.c"
        )
        Libs = @("-lm")
        Description = "DSP math tests (biquad, DC block)"
    },
    @{
        Name = "test_mixer"
        Sources = @(
            "$TestDir\test_mixer.c"
        )
        Libs = @("-lm")
        Description = "450 kHz mixer math tests"
    },
    # New tests from scaffolding
    @{
        Name = "test_tick_detector"
        Sources = @(
            "$TestDir\test_tick_detector.c",
            "$ToolsDir\tick_detector.c",
            "$ToolsDir\wwv_clock.c",
            "$SrcDir\kiss_fft.c"
        )
        Libs = @("-lm")
        Description = "WWV tick pulse detector tests"
    },
    @{
        Name = "test_marker_detector"
        Sources = @(
            "$TestDir\test_marker_detector.c",
            "$ToolsDir\marker_detector.c",
            "$ToolsDir\wwv_clock.c",
            "$ToolsDir\waterfall_telemetry.c",
            "$SrcDir\kiss_fft.c"
        )
        Libs = @("-lws2_32", "-lm")
        Description = "WWV minute marker detector tests"
    },
    @{
        Name = "test_iq_recorder"
        Sources = @(
            "$TestDir\test_iq_recorder.c",
            "$SrcDir\iq_recorder.c"
        )
        Libs = @("-lm")
        Description = "I/Q recording module tests"
    }
)

#============================================================================
# Build Function
#============================================================================

function Build-Test($test) {
    $name = $test.Name
    $sources = $test.Sources
    $libs = $test.Libs
    $exe = "$BinDir\$name.exe"

    Write-Status "Building $name..."

    $CFLAGS = @(
        "-std=c17",
        "-Wall",
        "-Wextra",
        "-O0",
        "-g",
        "-D_DEBUG",
        "-I`"$IncludeDir`"",
        "-I`"$TestDir`"",
        "-I`"$ToolsDir`"",
        "-I`"$SDRplayInclude`""
    )

    $allArgs = $CFLAGS + @("-o", "`"$exe`"")
    foreach ($src in $sources) {
        $allArgs += "`"$src`""
    }
    $allArgs += $libs

    if ($Verbose) {
        Write-Host "  Command: $CC $($allArgs -join ' ')"
    }

    $argString = $allArgs -join " "

    # Create temp file for stderr
    $stderrFile = "$BuildDir\$name.stderr.txt"

    $pinfo = New-Object System.Diagnostics.ProcessStartInfo
    $pinfo.FileName = $CC
    $pinfo.Arguments = $argString
    $pinfo.RedirectStandardError = $true
    $pinfo.RedirectStandardOutput = $true
    $pinfo.UseShellExecute = $false
    $pinfo.CreateNoWindow = $true

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $pinfo
    $p.Start() | Out-Null

    $stdout = $p.StandardOutput.ReadToEnd()
    $stderr = $p.StandardError.ReadToEnd()
    $p.WaitForExit()

    if ($p.ExitCode -ne 0) {
        Write-Fail "Build failed for $name"
        if ($stdout) { Write-Host $stdout -ForegroundColor Yellow }
        if ($stderr) { Write-Host $stderr -ForegroundColor Red }
        return $false
    }

    if ($Verbose -and $stdout) {
        Write-Host $stdout
    }

    return $true
}

#============================================================================
# Run Function
#============================================================================

function Run-Test($test) {
    $name = $test.Name
    $exe = "$BinDir\$name.exe"
    $description = $test.Description

    Write-Status "Running $name ($description)..."

    if (-not (Test-Path $exe)) {
        Write-Fail "Executable not found: $exe"
        return $false
    }

    $output = & $exe 2>&1 | Out-String
    $exitCode = $LASTEXITCODE

    if ($Verbose -or $exitCode -ne 0) {
        Write-Host $output
    }

    if ($exitCode -eq 0) {
        Write-Pass "$name"
        return $true
    } else {
        Write-Fail "$name (exit code: $exitCode)"
        if (-not $Verbose) {
            Write-Host $output
        }
        return $false
    }
}

#============================================================================
# Main
#============================================================================

try {
    Push-Location $ProjectRoot

    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  Phoenix SDR - Unit Test Runner" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""

    # Create directories
    if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }
    if (-not (Test-Path $BinDir)) { New-Item -ItemType Directory -Path $BinDir | Out-Null }

    # Filter tests
    $filteredTests = $Tests | Where-Object { $_.Name -like "*$Filter*" }

    if ($filteredTests.Count -eq 0) {
        Write-Warning "No tests match filter: $Filter"
        exit 1
    }

    Write-Status "Found $($filteredTests.Count) test suite(s)..."
    Write-Host ""

    # Build all tests
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  Building Tests" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""

    $buildFailed = @()
    foreach ($test in $filteredTests) {
        $success = Build-Test $test
        if (-not $success) {
            $buildFailed += $test.Name
        }
    }

    if ($buildFailed.Count -gt 0) {
        Write-Host ""
        Write-Fail "Build failed for: $($buildFailed -join ', ')"
        Write-Host ""
        Write-Host "Note: Some tests may fail to build if dependencies are missing."
        Write-Host "Continuing with remaining tests..."
        Write-Host ""
    }

    if ($BuildOnly) {
        Write-Host ""
        $builtCount = $filteredTests.Count - $buildFailed.Count
        if ($builtCount -gt 0) {
            Write-Pass "$builtCount test(s) built successfully"
        }
        if ($buildFailed.Count -gt 0) {
            Write-Fail "$($buildFailed.Count) test(s) failed to build"
        }
        exit $(if ($buildFailed.Count -gt 0) { 1 } else { 0 })
    }

    # Run all tests that built successfully
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  Running Tests" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""

    $passed = @()
    $failed = @()
    $skipped = @()

    foreach ($test in $filteredTests) {
        if ($buildFailed -contains $test.Name) {
            Write-Skip "$($test.Name) (build failed)"
            $skipped += $test.Name
            continue
        }

        $success = Run-Test $test
        if ($success) {
            $passed += $test.Name
        } else {
            $failed += $test.Name
        }
        Write-Host ""
    }

    # Summary
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  Summary" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  Total:   $($filteredTests.Count)" -ForegroundColor White
    Write-Host "  Passed:  $($passed.Count)" -ForegroundColor Green
    Write-Host "  Failed:  $($failed.Count)" -ForegroundColor Red
    Write-Host "  Skipped: $($skipped.Count)" -ForegroundColor Yellow
    Write-Host "========================================" -ForegroundColor Cyan

    if ($failed.Count -gt 0) {
        Write-Host ""
        Write-Host "Failed tests:" -ForegroundColor Red
        foreach ($name in $failed) {
            Write-Host "  - $name" -ForegroundColor Red
        }
    }

    if ($skipped.Count -gt 0) {
        Write-Host ""
        Write-Host "Skipped tests (build failed):" -ForegroundColor Yellow
        foreach ($name in $skipped) {
            Write-Host "  - $name" -ForegroundColor Yellow
        }
    }

    if ($failed.Count -eq 0 -and $skipped.Count -eq 0) {
        Write-Host ""
        Write-Pass "All tests passed!"
        exit 0
    } elseif ($failed.Count -eq 0) {
        Write-Host ""
        Write-Host "All built tests passed (some skipped due to build failures)" -ForegroundColor Yellow
        exit 1
    } else {
        exit 1
    }
}
catch {
    Write-Host "ERROR: $_" -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
