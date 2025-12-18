# Phoenix SDR — Beta Testing Guide

**Version:** v0.2.6
**Date:** December 14, 2025
**Contact:** KY4OLB

---

> ⚠️ **PARTIALLY SUPERSEDED** - December 18, 2025
>
> **Needs updating:**
> - Version is now 0.3.x+
> - Display now has 7 bucket bars (100, 440, 500, 600, 1000, 1200, 1500 Hz)
> - Minute marker detection added (800ms pulses flash differently)
> - BCD time code detection added
> - UDP telemetry output added (port 3005)
> - Signal path now uses 50kHz detector path + 12kHz display path
>
> **Still accurate:** Hardware requirements, basic command line usage, general concepts

---

## Overview

Thank you for testing Phoenix SDR! This beta focuses on **WWV tick detection** — receiving the 1000 Hz timing pulses broadcast by WWV/WWVH and measuring their intervals.

This is a smoke test for our SDR capture chain. If we can reliably detect WWV ticks, we know our signal processing works correctly.

---

## What You Need

### Hardware
- SDRplay RSP2 Pro (other SDRplay devices may work)
- HF antenna (any antenna that can receive 5-15 MHz)
- Windows 10/11 PC

### Software
- SDRplay API v3.x installed
- Phoenix SDR binaries (provided)

---

## Installation

1. Extract the Phoenix SDR zip to any folder
2. Ensure SDRplay API is installed
3. Connect your SDRplay device
4. Connect your antenna

---

## Running the Test

Open PowerShell in the Phoenix SDR folder and run:

```powershell
# Night reception (5 MHz) - try this first if after sunset
cmd /c ".\bin\simple_am_receiver.exe -f 5.000450 -g 59 -l 0 -o | .\bin\waterfall.exe"

# Daytime reception (10 MHz) - most reliable during day
cmd /c ".\bin\simple_am_receiver.exe -f 10.000450 -g 50 -l 2 -o | .\bin\waterfall.exe"

# Alternative frequencies
# 15 MHz (daytime, long distance)
cmd /c ".\bin\simple_am_receiver.exe -f 15.000450 -g 45 -l 3 -o | .\bin\waterfall.exe"
```

---

## Understanding the Display

### Split-Screen Layout

```
┌─────────────────────────────────┬──────────┐
│                                 │ 1 2 3 4 5│
│     FFT Waterfall               │ ▌ ▌ ▌ ▌ █│  ← Bucket bars
│     (spectrum scrolling down)   │ ▌ ▌ ▌ ▌ ▌│
│                                 │ 6 7      │
│     1024 pixels wide            │ ▌ ▌      │
│                                 │          │
└─────────────────────────────────┴──────────┘
```

**Left Panel (Waterfall):**
- Spectrum display scrolling downward
- Carrier visible as vertical line at center
- Sidebands visible on both sides
- Red dots mark detected energy peaks

**Right Panel (Bucket Bars):**
- 7 bars showing energy at WWV frequencies
- Bar 5 (1000 Hz) flashes **purple** when tick detected
- Bars numbered left to right: 100, 440, 500, 600, 1000, 1200, 1500 Hz

### Console Output

When the tick detector finds a tick, you'll see:

```
[WARMUP] Complete. Noise=0.000342, Thresh=0.000684
[   1.1s] TICK #1    int=     0ms  avg=     0ms
[   2.1s] TICK #2    int=  1002ms  avg= 1002ms
[   3.1s] TICK #3    int=   998ms  avg= 1000ms
```

- **[time]** — Elapsed time in seconds
- **TICK #n** — Sequential tick number
- **int=** — Interval since previous tick (should be ~1000ms)
- **avg=** — Average interval over last 15 seconds
- **!** — Warning if interval is outside 950-1050ms range

---

## Keyboard Controls

| Key | Function |
|-----|----------|
| **D** | Toggle tick detection on/off |
| **S** | Print statistics summary |
| **0** | Select gain adjustment |
| **1-7** | Select frequency threshold |
| **+/-** | Adjust selected parameter |
| **Q/Esc** | Quit |

---

## What We're Looking For

### Good Results ✅
- Ticks detected every ~1 second
- Interval values 950-1050ms consistently
- Average converging to ~1000ms
- Purple flash on bar 5 with each tick
- Clear carrier line in waterfall

### Problems to Report 🔴
- No ticks detected for extended periods
- Intervals wildly off (>200ms from 1000ms)
- Many rejected ticks (check console)
- Application crashes
- Display glitches

---

## Tuning Tips

### If No Signal
1. Try a different WWV frequency (5, 10, 15 MHz)
2. Check antenna connection
3. Increase LNA: `-l 3` or `-l 4`
4. Reduce gain reduction: `-g 30`

### If Too Much Noise
1. Reduce LNA: `-l 0`
2. Increase gain reduction: `-g 59`
3. Try a quieter frequency

### If Erratic Detection
1. Wait for voice announcements to pass (they interfere)
2. Seconds 29 and 59 have NO tick (normal gap)
3. Try pressing **S** to see statistics

---

## Data Collection

The program automatically logs all detected ticks to `wwv_ticks.csv`:

```csv
timestamp_ms,tick_num,energy_peak,duration_ms,interval_ms,avg_interval_ms,noise_floor
1065.3,1,0.002341,21.3,0,0,0.000342
2067.2,2,0.002156,21.3,1001.9,1001.9,0.000345
```

**Please save this file** and include it with your bug report if you encounter issues.

---

## Reporting Issues

Please report:
1. **What happened** — Describe the problem
2. **What you expected** — What should have happened
3. **Steps to reproduce** — Command line used, time of day
4. **WWV frequency** — Which frequency were you receiving
5. **Signal conditions** — Strong/weak, noisy/clean
6. **wwv_ticks.csv** — Attach the log file if relevant

---

## Known Limitations

- Detection works best during clean windows (avoid voice segments)
- Seconds 29 and 59 have no tick (intentional by WWV)
- Frame timing is ~21ms, so some jitter is expected
- Multipath fading can cause missed detections

---

## WWV Schedule Reference

| Seconds | Content |
|---------|---------|
| 0 | Tick + possible minute/hour tone |
| 1-28 | Ticks (100 Hz BCD subcarrier present) |
| 29 | **NO TICK** |
| 30-51 | Ticks |
| 52-58 | Voice announcement |
| 59 | **NO TICK** |

---

## Version History

| Version | Changes |
|---------|---------|
| v0.2.6 | Split-screen display, tick detector, interval averaging |
| v0.2.5 | Basic waterfall with 7-band monitoring |
| v0.2.0 | Initial AM receiver and waterfall |

---

**Thank you for testing!**

Phoenix Nest MARS Suite
KY4OLB

*"MARS funds WWV"*
