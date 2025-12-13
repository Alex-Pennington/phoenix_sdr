# Phoenix SDR - Progress & Status

**Last Updated:** 2025-12-13 ~17:30 EST

## Current Status: 🟡 IN PROGRESS

**Current Task:** Edge detection diagnostics - need to rebuild wwv_scan.c and run diagnostics to find why edge detection isn't triggering.

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
| DC Offset Fix | ✅ | Removed corruption from decimator |
| Tick Window | ✅ | Narrowed to 0-10ms |
| Voice Window Filter | ✅ | s01-28, s52-58 clean |
| PPS Offset Calibration | ✅ | -440ms default |
| GPS Resync | ✅ | Thread-safe handoff |
| Interactive Mode | ✅ | Live bucket display, key controls |
| WWV-Disciplined Mode | ✅ | Toggle with 'G' key |

## What's In Progress 🟡

| Component | Status | Notes |
|-----------|--------|-------|
| Edge Detection | 🟡 | Implemented but not triggering - diagnostics added |
| Build System | 🟡 | Not rebuilding wwv_scan.c (cache issue) |

## Recent Bug Fixes (This Session)

| Bug | Symptom | Root Cause | Fix |
|-----|---------|------------|-----|
| DC offset corruption | Signal corrupted | Decimator init bug | Fixed in decimator.c |
| Decimator gain | Wrong output levels | Missing gain compensation | Fixed gain calculation |
| Display copy race | `mx0.0000` always | Values reset before display | Added `_display` copy variables |
| Printf format | `mx0.0000` for valid data | `%.4f` rounds small numbers | Changed to `%.2e` |
| Wrong threshold scale | Edge never triggers | Default 0.002 but derivatives ~1e-5 | Changed default to 5e-6 |

## Edge Detection Implementation

Added to `wwv_scan.c`:

1. **Moving Average Filter** (48 samples = 1ms at 48kHz)
   ```c
   float envelope_smoothed = ma_process(fabsf(filtered));
   ```

2. **Derivative-Based Edge Detection**
   ```c
   float derivative = envelope_smoothed - g_prev_envelope;
   if (derivative > g_edge_threshold && envelope_smoothed > g_noise_floor * 3.0f) {
       g_tick_sample_pos = adjusted_pos;
       g_tick_detected = true;
   }
   ```

3. **Noise Floor Estimation** (200-800ms window)
   ```c
   g_noise_floor = 0.999f * g_noise_floor + 0.001f * envelope_smoothed;
   ```

4. **Diagnostic Output**
   ```
   15.0MHz G G40 O-440 T5e-06 ||#------------------| s05   NO-EDGE mx1.23e-05 H 0% C 0%
     [EDGE-DBG] maxD@250ms env=5.00e-04 nf=1.00e-03 nf*3=3.00e-03 thresh=5.00e-06 | deriv:PASS env:FAIL win:FAIL
   ```

5. **New Keyboard Controls**
   - `[` = decrease threshold (more sensitive)
   - `]` = increase threshold (less sensitive)

## Next Action

**Rebuild and run diagnostics:**
```powershell
Remove-Item .\build\wwv_scan.o -ErrorAction SilentlyContinue
.\build.ps1 -Target tools
.\bin\wwv_scan.exe -i
```

The diagnostic output will show which condition blocks edge detection:
- `deriv:FAIL` → derivative below threshold
- `env:FAIL` → envelope below noise_floor * 3
- `win:FAIL` → max derivative outside 0-100ms tick window

## Files Changed Today

- `tools/wwv_scan.c` - Edge detection, moving average, diagnostics
- `src/decimator.c` - DC offset fix, gain bug fix
- `src/gps_serial.c` - Millisecond parsing, thread-safe resync
- `src/sdr_stream.c` - DC/IQ correction enable
- `docs/fldigi_wwv_analysis.md` - FLDIGI research, implementation progress

## Build Commands

```powershell
cd D:\claude_sandbox\phoenix_sdr
.\build.ps1 -Clean
.\build.ps1
.\build.ps1 -Target tools
.\bin\wwv_scan.exe -i          # Interactive mode
.\bin\wwv_scan.exe -scantime 5 # Scan mode
```

## Hardware

- **SDR:** SDRplay RSP2 Pro (Serial: 1717046711)
- **GPS:** NEO-6M on Arduino (COM6, 115200 baud)
- **Antenna:** HF antenna on Hi-Z port

## Interactive Mode Keys

| Key | Function |
|-----|----------|
| 1-6 | Select frequency (2.5, 5, 10, 15, 20, 25 MHz) |
| +/- | Adjust PPS offset ±10ms |
| </> | Adjust gain ±3dB |
| [/] | Adjust edge threshold ×0.8/×1.25 |
| G | Toggle GPS/WWV discipline mode |
| R | Reset offset to -440ms |
| Q | Quit |

## Session History

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

### This Session
- Researched FLDIGI WWV implementation (it's visual only, no algorithm to port)
- Implemented edge detection approach
- Added moving average filter
- Added derivative-based tick detection
- Fixed printf format bug (%.4f → %.2e)
- Fixed display copy race condition
- Added comprehensive diagnostics
- Current blocker: build not rebuilding wwv_scan.c
