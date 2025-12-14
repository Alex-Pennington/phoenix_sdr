# Phoenix SDR - Progress & Status

**Last Updated:** 2025-12-13 ~21:00 EST

## Current Status: 🟢 TOOLS WORKING

**Current Task:** Simple AM receiver and waterfall display with tick detection are functional. Ready for live WWV testing.

## What We're Trying To Do

### Immediate Goal
1. Detect WWV second ticks reliably (>80% hit rate in clean windows)
2. Use tick position for timing discipline
3. Validate our SDR capture chain works

### Why This Matters
This is a smoke test. If we can detect WWV ticks reliably, we know:
- SDR hardware interface works
- Our signal processing works
- Our timing (GPS) works
- We can proceed to build the actual HF modem

## What's Working ✅

| Component | Status | Notes |
|-----------|--------|-------|
| SDRplay API | ✅ | Opens device, configures, streams |
| GPS Serial | ✅ | Reads time from NEO-6M, parses milliseconds |
| Decimator | ✅ | 2MHz → 48kHz, gain bug fixed |
| DSP Math | ✅ | Unit tests pass 10/10 |
| Build System | ✅ | PowerShell + MinGW |
| Version Header | ✅ | v0.2.0 |
| Simple AM Receiver | ✅ | Zero-IF + 450Hz offset, 6kHz RF BW, audio + waterfall output |
| Waterfall Display | ✅ | 1024x800, auto-gain, tick detection with 7 frequency bands |

## New Tools (This Session)

### simple_am_receiver.exe
Standalone AM receiver for WWV with audio output and waterfall pipe.

| Flag | Description | Default |
|------|-------------|---------|
| `-f` | Frequency in MHz | 15.0 |
| `-g` | Gain reduction 20-59 dB | 40 |
| `-l` | LNA state 0-4 | 0 |
| `-v` | Volume | 50.0 |
| `-o` | Output PCM to stdout (for waterfall) | off |
| `-a` | Mute audio (disable speakers) | off |
| | RF bandwidth | 6 kHz (fixed) |

**DSP Pipeline:**
1. IQ samples from SDRplay (2 MHz)
2. Lowpass filter I/Q at 3 kHz (gives 6 kHz RF bandwidth)
3. Envelope detection: magnitude = sqrt(I² + Q²)
4. Decimation: 2 MHz → 48 kHz
5. DC removal: highpass IIR
6. Output: speakers and/or stdout

### waterfall.exe
FFT waterfall display with WWV tick detection.

| Key | Frequency | Bandwidth | Signal |
|-----|-----------|-----------|--------|
| 0 | - | - | Gain adjustment |
| 1 | 100 Hz | ±10 Hz | BCD subcarrier |
| 2 | 440 Hz | ±5 Hz | Calibration tone |
| 3 | 500 Hz | ±5 Hz | Minute marker |
| 4 | 600 Hz | ±5 Hz | Station ID |
| 5 | 1000 Hz | ±100 Hz | Second tick (5ms pulse) |
| 6 | 1200 Hz | ±100 Hz | WWVH tick (5ms pulse) |
| 7 | 1500 Hz | ±20 Hz | 800ms tone |

**Features:**
- Window: 1024×800
- FFT: 1024 bins (46.9 Hz/bin)
- Auto-gain with attack/decay
- Sideband folding (combines +freq and -freq)
- Red dot markers when energy exceeds threshold
- Keys: `0-7` select parameter, `+/-` adjust, `Q/Esc` quit

**Usage:**
```powershell
cmd /c ".\bin\simple_am_receiver.exe -g 50 -l 2 -f 10.000450 -o | .\bin\waterfall.exe"
```

## What's In Progress 🟡

| Component | Status | Notes |
|-----------|--------|-------|
| Tick Detection Tuning | 🟡 | Thresholds need calibration with live signal |
| Edge Detection (wwv_scan) | 🟡 | Original approach paused; new tools working |

## Recent Changes (This Session)

### Simple AM Receiver Cleanup
- Removed confusing `-b` (SDRplay hardware BW) and `-bp` flags
- Fixed 6 kHz RF bandwidth (3 kHz I/Q lowpass, hardcoded)
- Added `-a` flag to mute audio while keeping waterfall output
- Zero-IF mode with +450 Hz offset to avoid DC hole

### Waterfall Tick Detection
- Added 7 WWV frequency bands with specific bandwidths
- Narrow bands (±5 Hz) for pure tones (440, 500, 600 Hz)
- Wide bands (±100 Hz) for short pulses (1000, 1200 Hz)
- Sideband folding: energy = mag[+freq] + mag[-freq]
- Threshold-based detection with red dot markers
- Keyboard control for threshold adjustment

## Files Changed Today

- `tools/simple_am_receiver.c` - Cleanup: removed -b/-bp flags, fixed 6kHz BW, added -a mute
- `tools/waterfall.c` - Added 7-band tick detection with sideband folding
- `src/decimator.c` - DC offset fix, gain bug fix (earlier)
- `src/gps_serial.c` - Millisecond parsing, thread-safe resync (earlier)

## Build Commands

```powershell
cd D:\claude_sandbox\phoenix_sdr
.\build.ps1 -Clean
.\build.ps1
.\build.ps1 -Target tools

# Run AM receiver with waterfall
cmd /c ".\bin\simple_am_receiver.exe -g 50 -l 2 -f 10.000450 -o | .\bin\waterfall.exe"

# Audio only (no waterfall)
.\bin\simple_am_receiver.exe -g 50 -l 2 -f 10.000450

# Waterfall only (mute audio)
cmd /c ".\bin\simple_am_receiver.exe -g 50 -l 2 -f 10.000450 -o -a | .\bin\waterfall.exe"
```

## Hardware

- **SDR:** SDRplay RSP2 Pro (Serial: 1717046711)
- **GPS:** NEO-6M on Arduino (COM6, 115200 baud)
- **Antenna:** HF antenna on Hi-Z port

## Waterfall Keys

| Key | Function |
|-----|----------|
| 0 | Select gain adjustment |
| 1 | Select 100 Hz (BCD) threshold |
| 2 | Select 440 Hz (Cal) threshold |
| 3 | Select 500 Hz (Min) threshold |
| 4 | Select 600 Hz (ID) threshold |
| 5 | Select 1000 Hz (Tick) threshold |
| 6 | Select 1200 Hz (WWVH) threshold |
| 7 | Select 1500 Hz (Tone) threshold |
| +/- | Adjust selected parameter |
| Q/Esc | Quit |

## Session History

### This Session (Evening)
- Created simple_am_receiver.c - standalone AM receiver
- Created waterfall.c - FFT display with SDL2 + KissFFT
- Fixed LIF mode bug (signal at -450 kHz not +450 kHz) - switched to Zero-IF
- Added auto-gain to waterfall
- Added manual gain control (+/- keys)
- Doubled display to 1024x800
- Removed -b and -bp flags (confusing, not needed)
- Added 7-band tick detection with proper bandwidths
- Added sideband folding for AM signal

### Earlier Today
- Fixed DC offset corruption in decimator
- Fixed decimator gain bug
- Narrowed tick window to 10ms
- Added peak energy tracking
- Added waveform dump mode
- Added interactive mode with live bucket display
- Added voice window filtering (s01-28, s52-58 clean)
- Fixed GPS millisecond parsing
- Fixed thread-safe resync
- Calibrated PPS offset (-440ms)
- Fixed race condition in display
- Researched FLDIGI WWV implementation
- Implemented edge detection approach (paused)
