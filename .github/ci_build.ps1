# Phoenix SDR - CI Build Script
# Complete build for GitHub Actions environment - ALL TOOLS

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
$SDRplayLib = "$PSScriptRoot\..\lib"  # Stub location

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
    $allFlags = $CFLAGS + $extraFlags
    $cmd = @($CC) + $allFlags + @("-c", "-o", "`"$objPath`"", "`"$source`"")
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $cmd[0] $cmd[1..($cmd.Length-1)] 2>&1 | Out-Null
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $prevEAP
    if ($exitCode -ne 0) { throw "Compilation failed for $source" }
    return $objPath
}

try {
    Push-Location (Join-Path $PSScriptRoot "..")

    Write-Status "CI Build starting (FULL SUITE)..."
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

    # Note: -lsdrplay_api omitted for CI (stub headers only, no lib file)
    # Executables will compile but require real sdrplay_api.dll at runtime
    $LDFLAGS = @(
        "-lm",
        "-lwinmm"
    )

    # Create directories
    if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }
    if (-not (Test-Path $BinDir)) { New-Item -ItemType Directory -Path $BinDir | Out-Null }
    if (-not (Test-Path "include\sdrplay")) { New-Item -ItemType Directory -Path "include\sdrplay" | Out-Null }

    # Copy SDRplay stub header for CI builds
    Write-Status "Setting up SDRplay stub header..."
    Copy-Item -Path ".github\sdrplay_api_stub.h" -Destination "include\sdrplay\sdrplay_api.h" -Force

    # Build SDRplay stub library for CI
    Write-Status "Building SDRplay stub..."
    $sdrplayStubObj = Build-Object ".github\sdrplay_api_stub.c" @()

    #==========================================================================
    # 1. simple_am_receiver.exe
    #==========================================================================
    Write-Status "Building simple_am_receiver..."
    $simpleAmObj = Build-Object "tools\simple_am_receiver.c" @()

    Write-Status "Linking simple_am_receiver.exe..."
    $cmd = @($CC, "-o", "`"$BinDir\simple_am_receiver.exe`"", "`"$simpleAmObj`"", "`"$sdrplayStubObj`"") + $LDFLAGS
    & $cmd[0] $cmd[1..($cmd.Length-1)]
    if ($LASTEXITCODE -ne 0) { throw "Linking failed for simple_am_receiver" }
    Write-Status "Built: $BinDir\simple_am_receiver.exe"

    #==========================================================================
    # 2. waterfall.exe (22 object files)
    #==========================================================================
    Write-Status "Building waterfall..."
    $kissObj = Build-Object "src\kiss_fft.c" @()
    $wwvClockObj = Build-Object "tools\wwv_clock.c" @()
    $channelFiltersObj = Build-Object "tools\channel_filters.c" @()
    $tickCombFilterObj = Build-Object "tools\tick_comb_filter.c" @()
    $tickDetectorObj = Build-Object "tools\tick_detector.c" @()
    $markerDetectorObj = Build-Object "tools\marker_detector.c" @()
    $slowMarkerDetectorObj = Build-Object "tools\slow_marker_detector.c" @()
    $markerCorrelatorObj = Build-Object "tools\marker_correlator.c" @()
    $syncDetectorObj = Build-Object "tools\sync_detector.c" @()
    $toneTrackerObj = Build-Object "tools\tone_tracker.c" @()
    $tickCorrelatorObj = Build-Object "tools\tick_correlator.c" @()
    $subcarrierDetectorObj = Build-Object "tools\subcarrier_detector.c" @()
    $bcdEnvelopeObj = Build-Object "tools\bcd_envelope.c" @()
    $bcdDecoderObj = Build-Object "tools\bcd_decoder.c" @()
    $bcdTimeDetectorObj = Build-Object "tools\bcd_time_detector.c" @()
    $bcdFreqDetectorObj = Build-Object "tools\bcd_freq_detector.c" @()
    $bcdCorrelatorObj = Build-Object "tools\bcd_correlator.c" @()
    $waterfallFlashObj = Build-Object "tools\waterfall_flash.c" @()
    $waterfallDspObj = Build-Object "tools\waterfall_dsp.c" @()
    $waterfallAudioObj = Build-Object "tools\waterfall_audio.c" @()
    $waterfallTelemObj = Build-Object "tools\waterfall_telemetry.c" @()
    $waterfallObj = Build-Object "tools\waterfall.c" @("-I`"$SDL2Include`"")

    Write-Status "Linking waterfall.exe..."
    $waterfallObjs = @(
        "`"$waterfallObj`"",
        "`"$channelFiltersObj`"",
        "`"$tickCombFilterObj`"",
        "`"$tickDetectorObj`"",
        "`"$markerDetectorObj`"",
        "`"$slowMarkerDetectorObj`"",
        "`"$markerCorrelatorObj`"",
        "`"$syncDetectorObj`"",
        "`"$toneTrackerObj`"",
        "`"$tickCorrelatorObj`"",
        "`"$subcarrierDetectorObj`"",
        "`"$bcdEnvelopeObj`"",
        "`"$bcdDecoderObj`"",
        "`"$bcdTimeDetectorObj`"",
        "`"$bcdFreqDetectorObj`"",
        "`"$bcdCorrelatorObj`"",
        "`"$waterfallFlashObj`"",
        "`"$wwvClockObj`"",
        "`"$waterfallDspObj`"",
        "`"$waterfallAudioObj`"",
        "`"$waterfallTelemObj`"",
        "`"$kissObj`""
    )
    $waterfallLdflags = @("-L`"$SDL2Lib`"", "-lmingw32", "-lSDL2main", "-lSDL2", "-lm", "-lws2_32", "-lwinmm")
    $cmd = @($CC, "-o", "`"$BinDir\waterfall.exe`"") + $waterfallObjs + $waterfallLdflags
    & $cmd[0] $cmd[1..($cmd.Length-1)]
    if ($LASTEXITCODE -ne 0) { throw "Linking failed for waterfall" }
    Write-Status "Built: $BinDir\waterfall.exe"

    #==========================================================================
    # 3. wormhole.exe
    #==========================================================================
    Write-Status "Building wormhole..."
    $wormholeObj = Build-Object "tools\wormhole.c" @("-I`"$SDL2Include`"")

    Write-Status "Linking wormhole.exe..."
    $wormholeLdflags = @("-L`"$SDL2Lib`"", "-lmingw32", "-lSDL2main", "-lSDL2", "-lm")
    $cmd = @($CC, "-o", "`"$BinDir\wormhole.exe`"", "`"$wormholeObj`"") + $wormholeLdflags
    & $cmd[0] $cmd[1..($cmd.Length-1)]
    if ($LASTEXITCODE -ne 0) { throw "Linking failed for wormhole" }
    Write-Status "Built: $BinDir\wormhole.exe"

    #==========================================================================
    # 4. signal_splitter.exe
    #==========================================================================
    Write-Status "Building signal_splitter..."
    $signalSplitterObj = Build-Object "tools\signal_splitter.c" @()

    Write-Status "Linking signal_splitter.exe..."
    $cmd = @($CC, "-o", "`"$BinDir\signal_splitter.exe`"", "`"$signalSplitterObj`"", "`"$waterfallDspObj`"", "-lm", "-lws2_32")
    & $cmd[0] $cmd[1..($cmd.Length-1)]
    if ($LASTEXITCODE -ne 0) { throw "Linking failed for signal_splitter" }
    Write-Status "Built: $BinDir\signal_splitter.exe"

    #==========================================================================
    # 5. test_tcp_commands.exe
    #==========================================================================
    Write-Status "Building test_tcp_commands..."
    $tcpCmdObj = Build-Object "src\tcp_commands.c" @()
    $testTcpObj = Build-Object "test\test_tcp_commands.c" @()
    $sdrStubsObj = Build-Object "test\sdr_stubs.c" @()

    Write-Status "Linking test_tcp_commands.exe..."
    $cmd = @($CC, "-o", "`"$BinDir\test_tcp_commands.exe`"", "`"$testTcpObj`"", "`"$tcpCmdObj`"", "`"$sdrStubsObj`"", "-lm")
    & $cmd[0] $cmd[1..($cmd.Length-1)]
    if ($LASTEXITCODE -ne 0) { throw "Linking failed for test_tcp_commands" }
    Write-Status "Built: $BinDir\test_tcp_commands.exe"

    #==========================================================================
    # 6. test_telemetry.exe
    #==========================================================================
    Write-Status "Building test_telemetry..."
    $testTelemObj = Build-Object "test\test_telemetry.c" @()

    Write-Status "Linking test_telemetry.exe..."
    $cmd = @($CC, "-o", "`"$BinDir\test_telemetry.exe`"", "`"$testTelemObj`"", "`"$waterfallTelemObj`"", "-lws2_32", "-lm")
    & $cmd[0] $cmd[1..($cmd.Length-1)]
    if ($LASTEXITCODE -ne 0) { throw "Linking failed for test_telemetry" }
    Write-Status "Built: $BinDir\test_telemetry.exe"

    #==========================================================================
    # 7. sdr_server.exe
    #==========================================================================
    Write-Status "Building sdr_server..."
    $sdrStreamObj = Build-Object "src\sdr_stream.c" @()
    $sdrDeviceObj = Build-Object "src\sdr_device.c" @()
    $sdrServerObj = Build-Object "tools\sdr_server.c" @()

    Write-Status "Linking sdr_server.exe..."
    $serverLdflags = @("-lws2_32", "-lm", "-lwinmm")
    $cmd = @($CC, "-o", "`"$BinDir\sdr_server.exe`"", "`"$sdrServerObj`"", "`"$tcpCmdObj`"", "`"$sdrStreamObj`"", "`"$sdrDeviceObj`"", "`"$sdrplayStubObj`"") + $serverLdflags
    & $cmd[0] $cmd[1..($cmd.Length-1)]
    if ($LASTEXITCODE -ne 0) { throw "Linking failed for sdr_server" }
    Write-Status "Built: $BinDir\sdr_server.exe"

    #==========================================================================
    # 8. telem_logger.exe
    #==========================================================================
    Write-Status "Building telem_logger..."
    $telemLoggerObj = Build-Object "tools\telem_logger.c" @()

    Write-Status "Linking telem_logger.exe..."
    $cmd = @($CC, "-o", "`"$BinDir\telem_logger.exe`"", "`"$telemLoggerObj`"", "-lws2_32")
    & $cmd[0] $cmd[1..($cmd.Length-1)]
    if ($LASTEXITCODE -ne 0) { throw "Linking failed for telem_logger" }
    Write-Status "Built: $BinDir\telem_logger.exe"

    Write-Status "CI Build complete (8 tools)."
}
catch {
    Write-Host "BUILD FAILED: $_" -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
