# Phoenix SDR - Build Script (MinGW/GCC)
# Requires: MinGW-w64 (via winget or other)
# SDRplay API must be installed: https://www.sdrplay.com/api/

param(
    [switch]$Clean,
    [switch]$Release,
    [string]$Target = "all"
)

$ErrorActionPreference = "Stop"

# Configuration
$ProjectName = "phoenix_sdr"
$SrcDir = "src"
$IncludeDir = "include"
$BuildDir = "build"
$BinDir = "bin"
$TestDir = "test"

# SDRplay API paths (default installation)
$SDRplayInclude = "C:\Program Files\SDRplay\API\inc"
$SDRplayLib = "C:\Program Files\SDRplay\API\x64"

# Compiler settings (MinGW/GCC via winget)
$MinGWBin = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
$CC = "$MinGWBin\gcc.exe"
$CFLAGS_COMMON = @(
    "-std=c17",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-I`"$IncludeDir`"",
    "-I`"$SDRplayInclude`""
)

$CFLAGS_DEBUG = @("-O0", "-g", "-D_DEBUG")
$CFLAGS_RELEASE = @("-O2", "-DNDEBUG")

$LDFLAGS = @(
    "-L`"$SDRplayLib`"",
    "-lsdrplay_api"
)

# Select build type
if ($Release) {
    $CFLAGS = $CFLAGS_COMMON + $CFLAGS_RELEASE
    $BuildType = "Release"
} else {
    $CFLAGS = $CFLAGS_COMMON + $CFLAGS_DEBUG
    $BuildType = "Debug"
}

function Write-Status($msg) {
    Write-Host "[$ProjectName] " -ForegroundColor Cyan -NoNewline
    Write-Host $msg
}

function Invoke-Clean {
    Write-Status "Cleaning build artifacts..."
    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    if (Test-Path $BinDir) { Remove-Item -Recurse -Force $BinDir }
    Write-Status "Clean complete."
}

function Initialize-BuildDirs {
    if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }
    if (-not (Test-Path $BinDir)) { New-Item -ItemType Directory -Path $BinDir | Out-Null }
}

function Test-SDRplayAPI {
    if (-not (Test-Path $SDRplayInclude)) {
        Write-Warning "SDRplay API include directory not found: $SDRplayInclude"
        Write-Warning "Please install SDRplay API from https://www.sdrplay.com/api/"
        Write-Warning "Or update paths in build.ps1"
        return $false
    }
    if (-not (Test-Path "$SDRplayLib\sdrplay_api.dll")) {
        Write-Warning "SDRplay API DLL not found: $SDRplayLib\sdrplay_api.dll"
        return $false
    }
    return $true
}

function Test-Compiler {
    if (-not (Test-Path $CC)) {
        Write-Error "Compiler not found at: $CC"
        return $false
    }
    return $true
}

function Build-Object($source) {
    $objName = [System.IO.Path]::GetFileNameWithoutExtension($source)
    $objPath = "$BuildDir\$objName.o"
    
    Write-Status "Compiling $source..."
    $allArgs = $CFLAGS + @("-c", "-o", "`"$objPath`"", "`"$source`"")
    $argString = $allArgs -join " "
    
    $process = Start-Process -FilePath "`"$CC`"" -ArgumentList $argString -NoNewWindow -Wait -PassThru
    
    if ($process.ExitCode -ne 0) {
        throw "Compilation failed for $source"
    }
    return $objPath
}

function Build-Executable($objects, $outName) {
    $exePath = "$BinDir\$outName.exe"
    
    Write-Status "Linking $outName..."
    $allArgs = @("-o", "`"$exePath`"") + $objects + $LDFLAGS
    $argString = $allArgs -join " "
    
    $process = Start-Process -FilePath "`"$CC`"" -ArgumentList $argString -NoNewWindow -Wait -PassThru
    
    if ($process.ExitCode -ne 0) {
        throw "Linking failed for $outName"
    }
    Write-Status "Built: $exePath"
    return $exePath
}

function Build-All {
    Write-Status "Building $ProjectName ($BuildType)..."
    
    if (-not (Test-Compiler)) { exit 1 }
    
    Initialize-BuildDirs
    
    if (-not (Test-SDRplayAPI)) {
        Write-Warning "Proceeding without SDRplay API validation..."
    }
    
    # Gather source files
    $sources = Get-ChildItem -Path $SrcDir -Filter "*.c" -ErrorAction SilentlyContinue
    
    if ($sources.Count -eq 0) {
        Write-Status "No source files found in $SrcDir"
        return
    }
    
    # Compile all sources
    $objects = @()
    foreach ($src in $sources) {
        $objects += Build-Object $src.FullName
    }
    
    # Link
    Build-Executable $objects $ProjectName
    
    # Copy DLL to bin directory for runtime
    $dllSrc = "$SDRplayLib\sdrplay_api.dll"
    $dllDst = "$BinDir\sdrplay_api.dll"
    if ((Test-Path $dllSrc) -and -not (Test-Path $dllDst)) {
        Write-Status "Copying sdrplay_api.dll to bin..."
        Copy-Item $dllSrc $dllDst
    }
    
    Write-Status "Build complete."
}

function Build-Test {
    Write-Status "Building tests..."
    
    if (-not (Test-Compiler)) { exit 1 }
    
    Initialize-BuildDirs
    
    $testSources = Get-ChildItem -Path $TestDir -Filter "*.c" -ErrorAction SilentlyContinue
    
    foreach ($src in $testSources) {
        $testName = [System.IO.Path]::GetFileNameWithoutExtension($src.Name)
        $obj = Build-Object $src.FullName
        Build-Executable @($obj) "test_$testName"
    }
    
    Write-Status "Test build complete."
}

function Build-Tools {
    Write-Status "Building tools..."
    
    if (-not (Test-Compiler)) { exit 1 }
    
    Initialize-BuildDirs
    
    $ToolsDir = "tools"
    $toolSources = Get-ChildItem -Path $ToolsDir -Filter "*.c" -ErrorAction SilentlyContinue
    
    # Build shared library objects
    $iqrObj = Build-Object "$SrcDir\iq_recorder.c"
    $decimatorObj = Build-Object "$SrcDir\decimator.c"
    $gpsObj = Build-Object "$SrcDir\gps_serial.c"
    $sdrDeviceObj = Build-Object "$SrcDir\sdr_device.c"
    $sdrStreamObj = Build-Object "$SrcDir\sdr_stream.c"
    
    # SDR-dependent tools (need full library)
    $sdrTools = @("wwv_scan")
    
    foreach ($src in $toolSources) {
        $toolName = [System.IO.Path]::GetFileNameWithoutExtension($src.Name)
        Write-Status "Building tool: $toolName"
        
        # Compile tool source
        $toolObj = Build-Object $src.FullName
        
        # Check if this tool needs full SDR library
        if ($sdrTools -contains $toolName) {
            # Link with SDR library + all dependencies
            $allArgs = @(
                "-o", "`"$BinDir\$toolName.exe`"",
                $toolObj,
                $iqrObj,
                $decimatorObj,
                $gpsObj,
                $sdrDeviceObj,
                $sdrStreamObj,
                "-lm"
            ) + $LDFLAGS
        } else {
            # Simple tool - just iq_recorder
            $allArgs = @("-o", "`"$BinDir\$toolName.exe`"", $toolObj, $iqrObj, "-lm", "-lws2_32")
        }
        
        $argString = $allArgs -join " "
        
        $process = Start-Process -FilePath "`"$CC`"" -ArgumentList $argString -NoNewWindow -Wait -PassThru
        
        if ($process.ExitCode -ne 0) {
            throw "Linking failed for $toolName"
        }
        Write-Status "Built: $BinDir\$toolName.exe"
    }
    
    Write-Status "Tools build complete."
}

# Main execution
try {
    Push-Location $PSScriptRoot
    
    # Show compiler version
    Write-Status "Using compiler: $CC"
    
    if ($Clean) {
        Invoke-Clean
        if ($Target -eq "clean") { exit 0 }
    }
    
    switch ($Target) {
        "all"   { Build-All }
        "test"  { Build-Test }
        "tools" { Build-Tools }
        "clean" { Invoke-Clean }
        default { 
            Write-Host "Usage: .\build.ps1 [-Clean] [-Release] [-Target <all|test|tools|clean>]"
        }
    }
}
catch {
    Write-Host "BUILD FAILED: $_" -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
