# Phoenix SDR - Build Script (MinGW/GCC)
# Simplified: Only builds wwv_listen for now

param(
    [switch]$Clean,
    [switch]$Release
)

$ErrorActionPreference = "Stop"

# Configuration
$ProjectName = "phoenix_sdr"
$SrcDir = "src"
$IncludeDir = "include"
$BuildDir = "build"
$BinDir = "bin"

# SDRplay API paths
$SDRplayInclude = "C:\Program Files\SDRplay\API\inc"
$SDRplayLib = "C:\Program Files\SDRplay\API\x64"

# Compiler (MinGW/GCC via winget)
$MinGWBin = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
$CC = "$MinGWBin\gcc.exe"

$CFLAGS = @(
    "-std=c17",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-I`"$IncludeDir`"",
    "-I`"$SDRplayInclude`"",
    "-O0", "-g", "-D_DEBUG"
)

if ($Release) {
    $CFLAGS = @(
        "-std=c17",
        "-Wall",
        "-Wextra", 
        "-pedantic",
        "-I`"$IncludeDir`"",
        "-I`"$SDRplayInclude`"",
        "-O2", "-DNDEBUG"
    )
}

$LDFLAGS = @(
    "-L`"$SDRplayLib`"",
    "-lsdrplay_api",
    "-lm",
    "-lwinmm"
)

function Write-Status($msg) {
    Write-Host "[$ProjectName] " -ForegroundColor Cyan -NoNewline
    Write-Host $msg
}

function Build-Object($source) {
    $objName = [System.IO.Path]::GetFileNameWithoutExtension($source)
    $objPath = "$BuildDir\$objName.o"
    
    Write-Status "Compiling $source..."
    $allArgs = $CFLAGS + @("-c", "-o", "`"$objPath`"", "`"$source`"")
    $argString = $allArgs -join " "
    
    $process = Start-Process -FilePath "`"$CC`"" -ArgumentList $argString -NoNewWindow -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Compilation failed for $source" }
    return $objPath
}

# Main
try {
    Push-Location $PSScriptRoot
    Write-Status "Using compiler: $CC"
    
    if ($Clean) {
        Write-Status "Cleaning..."
        if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
        if (Test-Path $BinDir) { Remove-Item -Recurse -Force $BinDir }
        exit 0
    }
    
    # Create directories
    if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }
    if (-not (Test-Path $BinDir)) { New-Item -ItemType Directory -Path $BinDir | Out-Null }
    
    # Build wwv_listen
    Write-Status "Building wwv_listen..."
    
    $objects = @(
        (Build-Object "tools\wwv_listen.c"),
        (Build-Object "$SrcDir\decimator.c"),
        (Build-Object "$SrcDir\sdr_device.c"),
        (Build-Object "$SrcDir\sdr_stream.c")
    )
    
    # Link
    Write-Status "Linking wwv_listen.exe..."
    $allArgs = @("-o", "`"$BinDir\wwv_listen.exe`"") + $objects + $LDFLAGS
    $argString = $allArgs -join " "
    
    $process = Start-Process -FilePath "`"$CC`"" -ArgumentList $argString -NoNewWindow -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Linking failed" }
    
    # Copy DLL
    $dllSrc = "$SDRplayLib\sdrplay_api.dll"
    $dllDst = "$BinDir\sdrplay_api.dll"
    if ((Test-Path $dllSrc) -and -not (Test-Path $dllDst)) {
        Copy-Item $dllSrc $dllDst
    }
    
    Write-Status "Built: $BinDir\wwv_listen.exe"
    Write-Status "Done."
}
catch {
    Write-Host "BUILD FAILED: $_" -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
