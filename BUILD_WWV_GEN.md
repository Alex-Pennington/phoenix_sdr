# Building WWV Test Signal Generator

## Quick Start

```powershell
# Build wwv_gen.exe
powershell -ExecutionPolicy Bypass -File build_wwv_gen.ps1

# Generate a test signal
.\bin\wwv_gen.exe -t 12:00 -d 001 -y 25 -o test.iqr
```

## Requirements

- **MSYS2 MinGW64** toolchain installed at `C:\msys64`
- Install with: `pacman -S mingw-w64-x86_64-toolchain`

## Build Script Usage

```powershell
# Debug build (default)
.\build_wwv_gen.ps1

# Release build (optimized)
.\build_wwv_gen.ps1 -Release

# Clean build artifacts
.\build_wwv_gen.ps1 -Clean
```

## What Gets Built

The minimal build script compiles only these files:
- `src/iqr_meta.c` - IQR file metadata
- `src/iq_recorder.c` - IQR file writer
- `tools/oscillator.c` - Phase-coherent oscillator
- `tools/bcd_encoder.c` - WWV BCD time code encoder
- `tools/wwv_signal.c` - WWV signal composer
- `tools/wwv_gen.c` - Main CLI tool

**Output:** `bin/wwv_gen.exe` (~150 KB)

## WWV Generator Usage

```
wwv_gen [options]

Options:
  -t HH:MM        Start time (default: 00:00)
  -d DDD          Day of year 001-366 (default: 001)
  -y YY           Year, last 2 digits (default: 25)
  -s wwv|wwvh     Station type (default: wwv)
  -o FILE         Output .iqr file (default: wwv_test.iqr)
  -p PORT         Stream via TCP (default: 4536)
  -c              Continuous streaming (requires -p)
  -h              Show help

File mode: Generates fixed 2-minute signal
TCP mode:  Streams continuously or 2-minute loop
```

### File Output Examples

```powershell
# Generate midnight signal for January 1, 2025
.\bin\wwv_gen.exe -o wwv_jan1.iqr

# Generate signal for noon on December 19, 2025 (day 353)
.\bin\wwv_gen.exe -t 12:00 -d 353 -y 25 -o wwv_dec19.iqr

# Generate WWVH (Hawaii) signal
.\bin\wwv_gen.exe -s wwvh -o wwvh_test.iqr
```

### TCP Streaming Examples

**Stream to waterfall.exe (replaces live SDR):**
```powershell
# Terminal 1: Start WWV generator as TCP server
.\bin\wwv_gen.exe -p 4536

# Terminal 2: Connect waterfall to TCP stream
.\bin\waterfall.exe -t localhost:4536
```

**Continuous 24/7 streaming:**
```powershell
.\bin\wwv_gen.exe -p 4536 -c
# Streams forever, loops through 2-minute signal
```

**Custom time signal:**
```powershell
.\bin\wwv_gen.exe -p 4536 -t 18:30 -d 100 -y 25
# Streams 2-minute signal starting at 18:30, day 100, 2025
```

**Test with custom port:**
```powershell
.\bin\wwv_gen.exe -p 5000 -c
```

## Output Formats

### File Mode (.iqr files)

- **File format:** .iqr (IQ Recording) with 64-byte header
- **Sample rate:** 2,000,000 samples/second
- **Duration:** Exactly 2 minutes (120,000,000 samples)
- **Data type:** int16 interleaved I/Q pairs
- **File size:** ~458 MB per file
- **Center frequency:** 5.000 MHz (metadata)
- **Actual content:** DC-centered baseband (450 Hz offset for WWV/WWVH tones)

### TCP Streaming Mode

**Protocol:** Compatible with Phoenix SDR I/Q Streaming Interface (port 4536)

**Stream format:**
- **Header:** 32-byte binary header (magic: "PHXI", version, sample rate, format)
- **Frames:** 8192 samples per frame with 16-byte frame header (magic: "IQDQ", sequence, count, flags)
- **Data:** int16 interleaved I/Q pairs (native SDRplay format)
- **Sample rate:** 2,000,000 Hz
- **Center frequency:** 5,000,000 Hz (metadata)

**Compatibility:**
- Drop-in replacement for sdr_server.exe
- Works with waterfall.exe: `waterfall.exe -t localhost:4536`
- Same binary protocol as live SDR hardware
- Frame size matches sdr_server (8192 samples)

## Troubleshooting

**Build fails with "gcc not found":**
- Install MSYS2 from https://www.msys2.org
- Run: `pacman -S mingw-w64-x86_64-toolchain`
- Verify gcc at: `C:\msys64\mingw64\bin\gcc.exe`

**Script execution disabled:**
- Use: `powershell -ExecutionPolicy Bypass -File build_wwv_gen.ps1`

**Warnings during compilation:**
- Warnings are normal and do not affect functionality
- `-Wunused-variable` warnings in wwv_signal.c are harmless

## Testing Generated Signals

### File Playback
```powershell
# Play IQR file through waterfall
.\bin\iqr_play.exe test.iqr | .\bin\waterfall.exe
```

### TCP Streaming (Recommended)
```powershell
# Terminal 1: Start TCP server
.\bin\wwv_gen.exe -p 4536

# Terminal 2: View in waterfall (live streaming)
.\bin\waterfall.exe -t localhost:4536
```

**What to look for:**
- **Tick marks:** 5ms pulses every second (except seconds 29 and 59)
- **Minute marker:** 800ms pulse at second 0
- **1000 Hz carrier:** WWV station identification tone
- **500/600 Hz tones:** Station tones per schedule
- **BCD time code:** Visible as amplitude modulation depth changes

### Advantages of TCP Mode

1. **No disk space** - streams in real-time without generating large files
2. **Live testing** - test detector algorithms with controllable signal
3. **Repeatable** - same signal every loop for debugging
4. **Fast iteration** - no file generation wait time
5. **Drop-in SDR replacement** - test without hardware
