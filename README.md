# Phoenix Nest MARS Suite — WWV Receiver Beta

**Purpose:** SDR hardware validation test. Receives WWV/WWVH time signals and detects timing pulses.

---

## Requirements

- SDRplay RSP2 Pro
- SDRplay API installed
- Windows 10/11

---

## Quick Start

```
simple_am_receiver.exe -f 10 -g 45 -l 3 -o | waterfall.exe
```

This tunes to 10 MHz WWV, pipes audio to the waterfall display.

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

**Tip:** For DC hole avoidance, tune slightly off-center (e.g., `-f 10.000450`)

---

## waterfall.exe

Reads PCM from stdin. Displays spectrum with tick detection.

| Key | Frequency | Bandwidth | Signal |
|-----|-----------|-----------|--------|
| 0 | — | — | Display gain |
| 1 | 100 Hz | ±10 Hz | BCD time code |
| 2 | 440 Hz | ±5 Hz | Calibration tone |
| 3 | 500 Hz | ±5 Hz | Minute marker |
| 4 | 600 Hz | ±5 Hz | Station ID |
| 5 | 1000 Hz | ±100 Hz | WWV tick |
| 6 | 1200 Hz | ±100 Hz | WWVH tick |
| 7 | 1500 Hz | ±20 Hz | Hour marker |

**Controls:** 0-7 select, +/- adjust threshold, Q or Esc quit

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

- Red dots appear at second ticks (once per second)
- Gap at seconds 29 and 59 (no tick transmitted)
- Carrier visible as center vertical line
- Sidebands visible on both sides of carrier

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| No signal | Try different frequency, check antenna |
| Wrong station (music) | Narrow bandwidth, check frequency |
| Whine/tone in audio | Tune ±500 Hz to avoid DC hole |
| Weak signal | Increase LNA (-l 3 or 4), reduce gain reduction (-g 30) |
| Too much noise | Reduce LNA, increase gain reduction |

---

## Signal Reference

### WWV (Fort Collins, CO)
- Second tick: 1000 Hz, 5 ms pulse
- Minute marker: 1000 Hz, 800 ms pulse
- Hour marker: 1500 Hz, 800 ms pulse
- Voice: Male, 7.5 sec before minute

### WWVH (Kauai, HI)
- Second tick: 1200 Hz, 5 ms pulse
- Minute marker: 1200 Hz, 800 ms pulse
- Hour marker: 1500 Hz, 800 ms pulse
- Voice: Female, 15 sec before minute

---

## Version

Beta 0.2.4+8.2c99002  — Hardware validation release

Phoenix Nest MARS Suite
KY4OLB