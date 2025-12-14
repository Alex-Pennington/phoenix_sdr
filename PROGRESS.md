# Phoenix SDR - Progress & Status

**Last Updated:** 2025-12-14 ~12:00 EST

## Current Status: 🟢 TICK DETECTION WORKING

**Version:** v0.2.6 — Split-screen waterfall with tick detection and interval averaging

## What's Working ✅

| Component | Status | Notes |
|-----------|--------|-------|
| SDRplay API | ✅ | Opens device, configures, streams |
| Simple AM Receiver | ✅ | Zero-IF + 450Hz offset, 6kHz RF BW |
| Split-Screen Waterfall | ✅ | Left: FFT spectrum, Right: 7 bucket bars |
| Tick Detection | ✅ | State machine with hysteresis, adaptive threshold |
| Interval Tracking | ✅ | Per-tick interval + 15-second rolling average |
| Purple Flash | ✅ | Visual indicator on 1000Hz bar when tick detected |
| CSV Logging | ✅ | Full tick data to wwv_ticks.csv |
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
