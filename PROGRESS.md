# Phoenix SDR - Progress & Status

**Last Updated:** 2025-12-19

## Current Status: 🟢 TELEMETRY LOGGER TOOL COMPLETE

**Version:** v1.10.x — UDP telemetry with standalone logger tool

## What's Working ✅

| Component | Status | Notes |
|-----------|--------|-------|
| SDRplay API | ✅ | Opens device, configures, streams |
| Simple AM Receiver | ✅ | Zero-IF + 450Hz offset, 6kHz RF BW |
| Split-Screen Waterfall | ✅ | Left: FFT spectrum, Right: 7 bucket bars |
| Tick Detection | ✅ | State machine with hysteresis, adaptive threshold |
| Interval Tracking | ✅ | Per-tick interval + 15-second rolling average |
| Purple Flash | ✅ | Visual indicator on 1000Hz bar when tick detected |
| UDP Telemetry | ✅ | All detector data broadcast on port 3005 |
| Telemetry Logger | ✅ | System tray app logs UDP → CSV files |
| Keyboard Controls | ✅ | D=toggle, S=stats, 0-7=params, +/-=adjust |

## Today's Achievement 🎯

**Split-screen waterfall with real-time tick detection:**

```
┌────────────────────────────┬──────┐
│                            │ ▌▌▌▌▌│
│   FFT Waterfall            │ ▌█▌▌▌│  ← Purple flash on tick
│   (carrier + sidebands)    │ ▌▌▌▌▌│
│                            │ ▌▌▌▌▌│
│   1024 x 800               │ 200px│
└────────────────────────────┴──────┘
```

**Console output:**
```
[WARMUP] Complete. Noise=0.000342, Thresh=0.000684
[   1.1s] TICK #1    int=     0ms  avg=     0ms
[   2.1s] TICK #2    int=  1002ms  avg= 1002ms
[   3.1s] TICK #3    int=   998ms  avg= 1000ms
[   4.1s] TICK #4    int=  1001ms  avg= 1000ms
```

## Tick Detector Features

- **Adaptive threshold:** Noise floor tracking during idle
- **Hysteresis:** High threshold to enter, low threshold to exit
- **Duration validation:** 2-50ms (rejects voice, accepts 5ms ticks)
- **Cooldown:** 500ms debounce between detections
- **Warmup:** 1 second adaptation before detection starts
- **Interval averaging:** Rolling 15-second window
- **CSV logging:** Full data for analysis

## Usage

```powershell
# Night (5 MHz)
cmd /c ".\bin\simple_am_receiver.exe -f 5.000450 -g 59 -l 0 -o | .\bin\waterfall.exe"

# Day (10 MHz)
cmd /c ".\bin\simple_am_receiver.exe -f 10.000450 -g 50 -l 2 -o | .\bin\waterfall.exe"
```

## Keyboard Controls

| Key | Function |
|-----|----------|
| D | Toggle tick detection |
| S | Print statistics |
| 0 | Select gain adjustment |
| 1-7 | Select frequency threshold |
| +/- | Adjust selected parameter |
| Q/Esc | Quit |

## Files Changed Today

- `tools/waterfall.c` — Complete rewrite with split-screen, tick detector, interval tracking
- `README.md` — Updated with new features
- `PROGRESS.md` — This file

## What's Next

1. **Beta testing** — Get feedback from MARS operators
2. **Threshold tuning** — Optimize for different signal conditions
3. **Minute marker detection** — Detect the 800ms pulse
4. **GPS integration** — Compare detected ticks to GPS PPS

## Build Commands

```powershell
cd D:\claude_sandbox\phoenix_sdr
.\build.ps1 -Target tools
```

## Hardware

- **SDR:** SDRplay RSP2 Pro
- **Antenna:** HF antenna on Hi-Z port

## Session History

### Dec 19 - Telemetry Logger Tool
**Goal:** Create a standalone UDP listener that writes telemetry to CSV files

**New Tool: `telem_logger.exe`**
- Listens on UDP port 3005 for telemetry broadcasts from waterfall.exe
- Auto-detects channels from message prefixes (CHAN, TICK, MARK, SYNC, etc.)
- Creates separate CSV files per channel: `telem_<CHANNEL>_YYYYMMDD_HHMMSS.csv`
- Runs in Windows system tray for background operation

**System Tray Features:**
- Tooltip shows live message count and active channels
- Right-click menu:
  - Status display (messages, channels)
  - Pause/Resume logging
  - Open logs folder in Explorer
  - Exit

**Command-line options:**
- `-p <port>` — UDP port (default: 3005)
- `-o <dir>` — Output directory for CSV files
- `-c <channels>` — Filter to specific channels (comma-separated)
- `-v` — Verbose mode (print to console)
- `--no-tray` — Disable system tray (console only)

**Usage:**
```powershell
# Start logger in system tray
.\bin\telem_logger.exe -o logs/

# Start waterfall
cmd /c ".\bin\simple_am_receiver.exe -f 5.000450 -g 59 -l 0 -o | .\bin\waterfall.exe"

# Right-click tray icon to manage/exit
```

**Files Created:**
- `tools/telem_logger.c` — UDP listener with system tray support
- `docs/TELEMETRY_LOGGER.md` — Tool documentation

**Files Modified:**
- `build.ps1` — Added telem_logger build target

### Dec 19 - UDP Telemetry Consolidation
**Goal:** Eliminate CSV file duplication, consolidate all logging to UDP telemetry (port 3005)

**Changes Implemented:**
1. **Added TELEM_CONSOLE channel** with 8KB ring buffer for console output
   - `telem_console()` helper with auto-flush on newline
   - `telem_console_flush()` for periodic draining (every 12 frames)
   - Captures epoch timing, sync state, connection status messages

2. **Added UDP telemetry to 6 detector modules:**
   - `tick_detector.c` - Tick events via TELEM_TICKS
   - `tick_correlator.c` - Correlation chains via TELEM_CORR
   - `marker_correlator.c` - Marker correlation via TELEM_MARKERS
   - `bcd_time_detector.c` - Time-domain BCD pulses via TELEM_BCDS
   - `bcd_freq_detector.c` - Freq-domain BCD pulses via TELEM_BCDS
   - `bcd_correlator.c` - BCD correlation stats via TELEM_BCDS

3. **Disabled CSV logging by default:**
   - All detector `_create()` calls now pass NULL (no CSV files)
   - Added `--log-csv` / `-l` flag to waterfall.exe to re-enable CSV
   - Removed `g_channel_csv` and `g_subcarrier_csv` (replaced by TELEM_CHANNEL, TELEM_SUBCAR)

4. **Converted console output to telemetry:**
   - Epoch timing messages `[EPOCH]` → TELEM_CONSOLE
   - Sync state messages `[SYNC]` → TELEM_CONSOLE

**Result:**
- ✅ Build successful (all tests pass)
- ✅ No CSV files created by default (UDP-only)
- ✅ Optional CSV re-enable with `--log-csv` flag
- ✅ All detector data flows through UDP port 3005
- ✅ Console messages captured via TELEM_CONSOLE channel

**Files Modified:**
- `tools/waterfall_telemetry.h` - Added TELEM_CONSOLE enum
- `tools/waterfall_telemetry.c` - Implemented console buffer and flush
- `tools/waterfall.c` - Added -l flag, NULL csv_path, console telemetry
- `tools/tick_detector.c` - Added TELEM_TICKS output
- `tools/tick_correlator.c` - Added TELEM_CORR output
- `tools/marker_correlator.c` - Added TELEM_MARKERS output
- `tools/bcd_time_detector.c` - Added TELEM_BCDS output
- `tools/bcd_freq_detector.c` - Added TELEM_BCDS output
- `tools/bcd_correlator.c` - Added TELEM_BCDS output

**Usage:**
```powershell
# Standard TCP mode (UDP telemetry only, no CSV files)
.\bin\waterfall.exe --tcp localhost:4536

# Optional: Enable CSV logging for debugging
.\bin\waterfall.exe --tcp localhost:4536 --log-csv

# Test pattern mode (no SDR hardware needed)
.\bin\waterfall.exe --test-pattern

# Custom window size
.\bin\waterfall.exe --tcp localhost:4536 -w 800 -H 400
```

**New command-line options:**
- `--test-pattern` — Generate synthetic 1000Hz tone for GUI testing
- `-w, --width WIDTH` — Set waterfall width in pixels (default: 1024, min: 400)
- `-H, --height HEIGHT` — Set window height in pixels (default: 600, min: 300)
- Console window automatically minimizes after SDL window opens

### Dec 14 - Morning/Afternoon
- Fixed waterfall display (was broken by sliding window changes)
- Discovered working code was in commit 2c99002
- Branched from working commit
- Added split-screen display (waterfall left, buckets right)
- Added tick detector state machine
- Added purple flash on detection
- Added interval tracking with 15-second rolling average
- Added D/S keyboard controls

### Dec 14 - Early Morning
- Implemented sliding window FFT (broke display)
- Multiple restoration attempts
- Eventually reverted to commit 2c99002

### Dec 13 - Evening
- Created simple_am_receiver.c
- Created waterfall.c with tick detection
- Fixed Zero-IF mode, DC offset, gain bugs

---

**Phoenix Nest MARS Suite**
KY4OLB
