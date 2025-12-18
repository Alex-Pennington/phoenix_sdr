# Phoenix SDR - Build Instructions

> ✅ **CURRENT** - Build instructions are accurate

This document describes how to set up your development environment and build Phoenix SDR from source.

## Prerequisites

### Required Software

| Software | Version | Purpose |
|----------|---------|---------|
| MinGW-w64 (GCC) | 12.x+ | C compiler |
| Git | Any | Version control |
| PowerShell | 5.1+ | Build scripts |

### Required Libraries

| Library | Version | Purpose | Location |
|---------|---------|---------|----------|
| SDL2 | 2.30.9 | GUI/Graphics | `libs/SDL2/` |
| KissFFT | 1.3.1 | FFT processing | `src/kiss_fft.c` |
| SDRplay API | 3.x | SDR hardware access | System install |

---

## Development Environment Setup

### 1. Install MinGW-w64

Option A - Using winget (recommended):
```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

Option B - Manual download from [winlibs.com](https://winlibs.com/)

The build script expects MinGW at:
```
%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\
```

### 2. Install SDRplay API

1. Download from [sdrplay.com/api](https://www.sdrplay.com/api/)
2. Run installer (requires SDRplay hardware or evaluation mode)
3. Default install location: `C:\Program Files\SDRplay\API\`

The build script expects:
- Headers: `C:\Program Files\SDRplay\API\inc\`
- Libraries: `C:\Program Files\SDRplay\API\x64\`

### 3. SDL2 (Included)

SDL2 is bundled in the repository at `libs/SDL2/SDL2-2.30.9/`.

If missing, download from [libsdl.org](https://github.com/libsdl-org/SDL/releases) and extract to `libs/SDL2/`.

---

## Building

### Quick Build

```powershell
.\build.ps1              # Debug build
.\build.ps1 -Release     # Optimized build
.\build.ps1 -Clean       # Clean all build artifacts
```

### Build Output

All executables are placed in `bin/`:

| Executable | Description |
|------------|-------------|
| `sdr_server.exe` | SDR hardware server (TCP interface) |
| `waterfall.exe` | Spectrum/waterfall display |
| `wormhole.exe` | Constellation display |
| `simple_am_receiver.exe` | Basic AM receiver |
| `test_*.exe` | Test executables |

Required DLLs are copied to `bin/`:
- `SDL2.dll`
- `sdrplay_api.dll`

### Build Artifacts

| Directory | Contents |
|-----------|----------|
| `build/` | Object files (`.o`) |
| `bin/` | Executables and DLLs |

---

## Project Structure

```
phoenix_sdr/
├── build.ps1              # Main build script
├── include/               # Header files
│   ├── version.h          # Auto-generated version info
│   ├── decimator.h
│   ├── gps_serial.h
│   └── ...
├── src/                   # Core library source
│   ├── sdr_stream.c       # SDR streaming
│   ├── sdr_device.c       # SDR device control
│   ├── decimator.c        # Sample rate conversion
│   ├── kiss_fft.c         # FFT implementation
│   └── ...
├── tools/                 # Executable tools
│   ├── sdr_server.c       # SDR server
│   ├── waterfall.c        # Waterfall display
│   ├── waterfall_dsp.c    # DSP module
│   ├── waterfall_audio.c  # Audio output module
│   ├── wormhole.c         # Constellation display
│   └── ...
├── test/                  # Test files
│   ├── test_dsp.c
│   └── ...
├── libs/                  # Third-party libraries
│   └── SDL2/
└── docs/                  # Documentation
```

---

## Compiler Flags

### Debug Build (default)
```
-std=c17 -Wall -Wextra -pedantic -O0 -g -D_DEBUG
```

### Release Build (`-Release`)
```
-std=c17 -Wall -Wextra -pedantic -O2 -DNDEBUG
```

### Linker Flags
```
-lsdrplay_api -lSDL2main -lSDL2 -lm -lws2_32 -lwinmm
```

---

## Running

### Start SDR Server
```powershell
.\bin\sdr_server.exe
```
Opens control port (4535) and I/Q data port (4536).

### Start Waterfall (separate terminal)
```powershell
.\bin\waterfall.exe
```
Connects to SDR server and displays spectrum.

### Keyboard Controls (Waterfall)

| Key | Action |
|-----|--------|
| M | Mute/unmute audio |
| Up/Down | Volume |
| +/- | Adjust gain |
| 0-7 | Select parameter |
| D | Toggle tick detection |
| S | Print statistics |
| Q/Esc | Quit |

---

## Troubleshooting

### "Cannot find sdrplay_api.dll"
Install the SDRplay API from sdrplay.com.

### "gcc not found"
Ensure MinGW is installed and the path in `build.ps1` matches your installation.

### "SDL2.dll not found"
Run `.\build.ps1` - it copies SDL2.dll to bin/ automatically.

### Build fails with "Permission denied"
Close any running executables from bin/ before rebuilding.

### Linker errors for sdrplay_api functions
The SDRplay API must be installed. CI builds use stubs that compile but don't run.

---

## Version Information

Every build auto-increments the build number and embeds the git commit hash in `include/version.h`.

```powershell
.\build.ps1 -Increment patch   # Bump version 0.3.0 → 0.3.1
.\build.ps1 -Increment minor   # Bump version 0.3.0 → 0.4.0
.\build.ps1 -Increment major   # Bump version 0.3.0 → 1.0.0
```

See [RELEASING.md](RELEASING.md) for the full release workflow.

---

## Cross-Platform Notes

Currently Windows-only due to:
- SDRplay API (Windows/Linux/Mac available)
- waveOut audio (Windows-specific)
- Some Win32 socket calls

Linux/Mac support would require:
- ALSA or PulseAudio for audio
- POSIX socket adjustments
- Build script adaptation (Makefile)
