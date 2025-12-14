# Phoenix Nest MARS Suite — WWV Receiver Beta

**Purpose:** SDR hardware validation test. Receives WWV/WWVH time signals and detects timing pulses with real-time visualization.

**Version:** v0.2.6 — Split-screen waterfall with tick detection

---

## Requirements

- SDRplay RSP2 Pro
- SDRplay API installed
- Windows 10/11

---

## Quick Start

```powershell
cmd /c ".\bin\simple_am_receiver.exe -f 5.000450 -g 59 -l 0 -o | .\bin\waterfall.exe"
```

This tunes to 5 MHz WWV, pipes audio to the split-screen waterfall display with tick detection.

---

## Display

The waterfall window has two panels:

| Panel | Width | Content |
|-------|-------|---------|
| Left | 1024 px | FFT waterfall (spectrum over time) |
| Right | 200 px | 7 bucket energy bars (WWV frequencies) |

### Visual Indicators

- **Carrier line:** Vertical line at center of waterfall
- **Red dots:** Energy above threshold at monitored frequencies
- **Purple flash:** 1000 Hz bar flashes purple when tick detected
- **Color scale:** Blue (low) → Cyan → Green → Yellow → Red (high)

---

## simple_am_receiver.exe

| Flag | Description | Default |
|------|-------------|---------|
| -f | Frequency in MHz | 15.0 |
| -g | Gain reduction (20-59 dB) | 40 |
| -l | LNA state (0-4) | 0 |
| -v | Volume | 50.0 |
| -o | Output PCM to stdout | off |
| -a | Mute speaker audio | off |

**Tip:** Tune slightly off-center to avoid DC hole (e.g., `-f 5.000450`)

---

## waterfall.exe

Split-screen display with automatic tick detection.

### Keyboard Controls

| Key | Function |
|-----|----------|
| 0-7 | Select parameter (0=gain, 1-7=frequency thresholds) |
| +/- | Adjust selected parameter |
| D | Toggle tick detection on/off |
| S | Print tick statistics |
| Q/Esc | Quit |

### Monitored Frequencies

| Key | Frequency | Bandwidth | Signal |
|-----|-----------|-----------|--------|
| 1 | 100 Hz | ±10 Hz | BCD time code |
| 2 | 440 Hz | ±5 Hz | Calibration tone |
| 3 | 500 Hz | ±5 Hz | Minute marker |
| 4 | 600 Hz | ±5 Hz | Station ID |
| 5 | 1000 Hz | ±100 Hz | **WWV tick** |
| 6 | 1200 Hz | ±100 Hz | WWVH tick |
| 7 | 1500 Hz | ±20 Hz | Hour marker |

### Tick Detection Output

When ticks are detected, the console shows:
```
[  23.5s] TICK #12   int= 1002ms  avg=  999ms  
[  24.5s] TICK #13   int=  998ms  avg= 1000ms  
```

- **int** = Interval since last tick (should be ~1000ms)
- **avg** = Average interval over last 15 seconds
- **!** = Marker if interval outside 950-1050ms range

### CSV Logging

Tick data is logged to `wwv_ticks.csv`:
```
timestamp_ms,tick_num,energy_peak,duration_ms,interval_ms,avg_interval_ms,noise_floor
```

---

## WWV Frequencies

| Frequency | Notes |
|-----------|-------|
| 2.5 MHz | Low power, night propagation |
| 5 MHz | Good night reception |
| 10 MHz | Most reliable daytime |
| 15 MHz | Daytime, good distance |
| 20 MHz | Daytime only, WWV only |

---

## Expected Results

- Purple flash on 5th bar (1000 Hz) once per second
- Interval display ~1000ms (±50ms normal variance)
- Average converges to ~1000ms over 15 seconds
- Gap at seconds 29 and 59 (no tick transmitted)
- Carrier visible as center vertical line in waterfall

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| No signal | Try different frequency, check antenna |
| Intervals erratic | Signal too weak, try 5 MHz at night or 10 MHz daytime |
| Many rejected ticks | Voice interference, wait for quiet window |
| No purple flash | Press D to enable detection, check signal strength |
| Too much noise | Reduce LNA (-l 0), increase gain reduction (-g 59) |

---

## Signal Reference

### WWV (Fort Collins, CO)
- Second tick: 1000 Hz, 5 ms pulse
- Minute marker: 1000 Hz, 800 ms pulse
- Hour marker: 1500 Hz, 800 ms pulse
- Voice: Male, 7.5 sec before minute
- Silent: Seconds 29 and 59

### WWVH (Kauai, HI)
- Second tick: 1200 Hz, 5 ms pulse
- Minute marker: 1200 Hz, 800 ms pulse
- Hour marker: 1500 Hz, 800 ms pulse
- Voice: Female, 15 sec before minute
- Silent: Seconds 29 and 59

---

## Build

```powershell
.\build.ps1 -Clean
.\build.ps1 -Target tools
```

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| v0.2.6 | 2024-12-14 | Split-screen display, tick detector with interval averaging |
| v0.2.5 | 2024-12-14 | Waterfall with 7-band monitoring |
| v0.2.0 | 2024-12-13 | Simple AM receiver, basic waterfall |

---

**Phoenix Nest MARS Suite**  
KY4OLB
