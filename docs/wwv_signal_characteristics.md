# WWV/WWVH Signal Characteristics Reference

## Overview

WWV (Fort Collins, Colorado) and WWVH (Kauai, Hawaii) are NIST time and frequency standard stations broadcasting continuously. These cesium-referenced signals provide frequency accuracy of approximately 1 part in 10¹².

---

## Carrier Frequencies

| Station | Frequencies (MHz) | Power |
|---------|-------------------|-------|
| WWV | 2.5, 5, 10, 15, 20, 25* | 10 kW (5/10/15 MHz), 2.5 kW (2.5/20 MHz) |
| WWVH | 2.5, 5, 10, 15 | 10 kW (5/10/15 MHz), 5 kW (2.5 MHz) |

*25 MHz is experimental and may be interrupted without notice.

---

## Modulation

WWV and WWVH use **double sideband amplitude modulation (DSB-AM)**.

| Component | Modulation Depth |
|-----------|------------------|
| Steady audio tones | 50% |
| BCD time code (100 Hz subcarrier) | 50% |
| Second pulses and minute/hour markers | 100% |
| Voice announcements | 75% |

---

## Seconds Pulses (Ticks)

### Timing Structure

The on-time marker is synchronized with the **start of the 5 ms tone**.

```
|<-- 10 ms -->|<-- 5 ms -->|<-- 25 ms -->|
|   silence   |    TONE    |   silence   |
              ^
              |
        ON-TIME MARKER
              
Total protected zone: 40 ms
```

### Pulse Characteristics

| Parameter | WWV | WWVH |
|-----------|-----|------|
| Tone frequency | 1000 Hz | 1200 Hz |
| Duration | 5 ms | 5 ms |
| Preceding silence | 10 ms | 10 ms |
| Following silence | 25 ms | 25 ms |

### Omitted Pulses

- **29th second** - no pulse
- **59th second** - no pulse

This allows identification of the approaching minute boundary.

---

## Minute Marker

| Parameter | WWV | WWVH |
|-----------|-----|------|
| Tone frequency | 1000 Hz | 1200 Hz |
| Duration | 800 ms | 800 ms |
| Occurs at | Second 0 of each minute | Second 0 of each minute |

---

## Hour Marker

| Parameter | WWV | WWVH |
|-----------|-----|------|
| Tone frequency | 1500 Hz | 1500 Hz |
| Duration | 800 ms | 800 ms |
| Occurs at | Second 0, Minute 0 of each hour | Second 0, Minute 0 of each hour |

---

## BCD Time Code

### Subcarrier

- **Frequency**: 100 Hz
- **Format**: Modified IRIG-H
- **Data rate**: 1 bit per second
- **Bandwidth**: ~1-2 Hz around subcarrier

### Encoded Information

- Current minute (0-59)
- Current hour (0-23)
- Day of year (1-366)
- Year (2 digits)
- Leap second warning
- Daylight Saving Time indicator
- DUT1 correction

### Frame Structure

One complete time code frame = 60 seconds (one minute).

---

## UT1 Corrections

UT1 corrections are encoded using **doubled ticks** during the first 16 seconds of each minute.

- If you need UT1 within ±0.9 s: Use UTC directly
- If you need UT1 within ±0.1 s: Apply the DUT1 correction from doubled ticks

---

## Audio Tones Schedule

Standard frequency reference tones alternate throughout each hour:

| Tone | Frequency | Schedule |
|------|-----------|----------|
| Primary | 500 Hz | Most minutes (alternating) |
| Primary | 600 Hz | Most minutes (alternating) |
| Musical A | 440 Hz | Minute 2 (WWV), Minute 1 (WWVH) |
| No tone | — | Minutes with voice/special content |

The 440 Hz tone is omitted during the first hour (0000-0100 UTC) of each day.

---

## Voice Announcements

| Station | Timing | Voice |
|---------|--------|-------|
| WWVH | ~15 seconds before the minute | Female |
| WWV | ~7.5 seconds before the minute | Male |

The different voices and timing allow listeners to distinguish stations when both are receivable.

---

## Special Broadcasts

### Geophysical Alerts (NOAA)

- **WWV**: 18 minutes after each hour
- **WWVH**: 45 minutes after each hour
- Duration: < 45 seconds
- Updated every 3 hours (0000, 0300, 0600, 0900, 1200, 1500, 1800, 2100 UTC)

### HamSCI Test Signal

Since November 15, 2021:
- **WWV**: Minute 8 of each hour
- **WWVH**: Minute 48 of each hour
- Content: Various tones, chirps, Gaussian noise bursts for ionospheric research

---

## Total Signal Bandwidth

**Occupied bandwidth: ~10 kHz** (±5 kHz from carrier)

| Component | Approximate Bandwidth |
|-----------|----------------------|
| Carrier | ~0 Hz (spectral line) |
| 100 Hz BCD subcarrier | ~1-2 Hz |
| 440/500/600 Hz tones | ~10-20 Hz each |
| 1000/1200 Hz pulses | ~200 Hz (due to 5 ms pulse shape) |
| Voice | ~3 kHz |

For tick detection only, **2 kHz bandwidth** centered on carrier is sufficient.

---

## Using WWV for Frequency Offset Calibration

### Method 1: Carrier Frequency Measurement

1. Tune SDR to WWV (10 MHz typically has best SNR in continental US)
2. Capture signal with narrow bandwidth (~100 Hz)
3. Compute FFT
4. Locate carrier peak - offset from expected position = frequency error

```
offset_ppm = (measured_freq - nominal_freq) / nominal_freq × 1,000,000
```

**Example**: Peak at 10,000,023 Hz instead of 10,000,000 Hz → 2.3 ppm fast

### Method 2: Beat Frequency

1. Mix WWV carrier with local oscillator at nominal frequency
2. Result is DC (perfect alignment) or slow beat tone (offset present)
3. Measure beat frequency directly in Hz

```
offset_ppm = (beat_freq_hz / nominal_freq_hz) × 1,000,000
```

### Method 3: Tick Interval Timing

1. Detect 1000 Hz tick pulses using matched filter
2. Count samples between consecutive ticks
3. Compare to expected sample count

```
expected_samples = sample_rate × 1.0 second
actual_samples = measured count between ticks
offset_ppm = ((actual_samples / expected_samples) - 1.0) × 1,000,000
```

**Advantage**: Self-calibrating over time - average many intervals to reduce noise.

### Method 4: Phase Accumulation

1. Track carrier phase continuously
2. Phase drift accumulates if frequency offset exists
3. 1 Hz offset = 360° phase change per second

Best for long-term integration with low SNR signals.

---

## Practical Considerations

### Propagation Effects

| Effect | Impact | Mitigation |
|--------|--------|------------|
| Path delay | 1-20 ms typical, varies with ionosphere | Don't trust absolute time < 1 ms without modeling |
| Doppler | ~0.1 Hz wander from ionospheric motion | Average over 10+ seconds |
| Multipath | Tick smearing, phase distortion | Matched filter detection |
| Fading | Signal strength variations | Use strongest available frequency |

### Frequency Selection Guidelines

| Condition | Best Frequency |
|-----------|----------------|
| Daytime, normal | 10 MHz or 15 MHz |
| Nighttime | 5 MHz |
| High solar activity | 15 MHz or 20 MHz |
| Low solar activity | 5 MHz or 10 MHz |
| West coast US | May receive both WWV and WWVH |

### Achievable Accuracy

| Parameter | Typical Accuracy |
|-----------|------------------|
| Frequency (long average) | Parts in 10⁹ to 10¹⁰ |
| Absolute time | 1-10 ms (propagation limited) |
| Tick-to-tick interval | Sub-millisecond (local clock limited) |

---

## Station Coordinates

| Station | Latitude | Longitude |
|---------|----------|-----------|
| WWV | 40° 40' 50.5" N | 105° 02' 26.6" W |
| WWVH | 21° 59' 16" N | 159° 45' 56" W |

---

## References

- NIST Special Publication 432: NIST Time and Frequency Services
- NIST WWV/WWVH web pages: https://www.nist.gov/pml/time-and-frequency-division/time-distribution/radio-station-wwv
- NTP WWV/H Driver Documentation: https://www.ntp.org/documentation/drivers/driver36/

---

*Document created for Phoenix Nest MARS Suite development*
*Last updated: December 2024*
