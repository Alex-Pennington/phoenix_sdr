# Phoenix Nest MARS Suite — WWV Detection Progress

**For Beta Testers** | Last Updated: December 2025 | Build 0.2.5

---

> ⚠️ **PARTIALLY SUPERSEDED** - December 18, 2025
>
> **Needs updating:**
> - Build version is now 0.3.x+
> - Sample rate is now 50kHz for detectors, not 48kHz (avoids sample drops)
> - Tick detection now uses FFT-based approach, not just Goertzel
> - Minute marker detection is now implemented (marker_detector.c)
> - BCD time code detection added (bcd_time_detector.c, bcd_freq_detector.c)
> - Sync state machine added (sync_detector.c)
>
> **Still accurate:** WWV signal description, general detection concepts, MARS motivation

---

## Why WWV Detection?

Before we build the full MIL-STD-188-110A HF modem, we need to prove our hardware and software chain actually works. WWV provides the perfect smoke test.

**The logic is simple:**
1. WWV broadcasts a predictable 1000 Hz tick pulse every second
2. The pulse is exactly 5 milliseconds long
3. It happens at a known time (top of each second)
4. If we can reliably detect these ticks, we know our entire capture chain is solid

This isn't just an academic exercise — it validates:
- ✅ SDRplay hardware interface
- ✅ Signal decimation (2 MHz → 48 kHz)
- ✅ AM demodulation
- ✅ Tone detection (Goertzel algorithm)
- ✅ GPS timing synchronization

If WWV works, we're ready for the real modem work.

---

## The MARS Connection

For those wondering why a MARS project cares about WWV:

The Military Auxiliary Radio System has a vested interest in WWV remaining on the air. Beyond advocacy, there's a practical application here.

**Timing discipline.** The eventual goal is half-second timing accuracy using WWV as an NTP fallback. When you're in the field and internet time servers aren't available, WWV provides a reliable timing reference. Every MARS station with an HF receiver can sync to WWV.

This capability starts with proving we can detect ticks. Then we measure their timing. Then we discipline our local clock. One step at a time.

---

## How WWV Works (Quick Primer)

WWV and WWVH broadcast on 2.5, 5, 10, 15, and 20 MHz with several timing signals:

| Signal | Frequency | Duration | Purpose |
|--------|-----------|----------|---------|
| **Second tick** | 1000 Hz | 5 ms | Time marker (our target) |
| Minute marker | 500 Hz | 800 ms | Start of minute |
| Hour marker | 1500 Hz | 800 ms | Start of hour |
| BCD time code | 100 Hz | continuous | Encoded time data |
| Calibration | 440 Hz | varies | A440 reference |

The second tick is a short 1000 Hz pulse at the start of each second. It's amplitude-modulated onto the carrier. We're looking for the *envelope* of that modulation.

**Note:** Seconds 29 and 59 have no tick — that's normal.

---

## Current Detection Approach

### The Pipeline

```
SDRplay RSP2 Pro
    ↓ (2 MHz IQ samples)
Lowpass Filter (3 kHz cutoff)
    ↓
AM Demodulation (envelope = √(I² + Q²))
    ↓
Decimation (2 MHz → 48 kHz audio)
    ↓
DC Removal (highpass filter)
    ↓
Goertzel Algorithm (1000 Hz detector)
    ↓
Threshold Detection → TICK!
```

### Why Goertzel?

Goertzel is an FFT optimized for detecting a single frequency. Instead of computing the entire spectrum, it gives us just the energy at 1000 Hz. Fast, efficient, and perfect for tick detection.

### Detection Parameters

| Parameter | Value | Why |
|-----------|-------|-----|
| Sample rate | 48 kHz | Standard audio rate |
| Window size | 5 ms | Matches tick duration |
| Step size | 1 ms | Sliding window for edge finding |
| Target frequency | 1000 Hz | WWV tick tone |
| Bandwidth | ±100 Hz | Allows for Doppler, drift |

The 5ms window matches the tick duration exactly. We slide it 1ms at a time, looking for when energy suddenly increases (tick start) and decreases (tick end).

---

## What's Working Today

### ✅ Proven Solid

| Component | Status | Notes |
|-----------|--------|-------|
| SDRplay streaming | Working | Stable at 2 MHz sample rate |
| GPS time sync | Working | NEO-6M provides PPS reference |
| AM demodulation | Working | Zero-IF with DC hole avoidance |
| Waterfall display | Working | Visual confirmation of signals |
| Basic tick detection | Working | Red dots appear at tick times |
| DSP math | Working | All unit tests pass |

### 🔧 Under Development

| Feature | Status | Notes |
|---------|--------|-------|
| Edge timing precision | In progress | Finding exact tick edge for timing |
| Noise immunity | Needs work | False triggers in poor conditions |
| Multi-frequency WWV | Planned | Auto-select best frequency |

---

## What Beta Testers Will See

### Current Tools

**simple_am_receiver.exe** — Tunes to WWV frequency, outputs audio and/or waterfall data

```
simple_am_receiver.exe -f 10 -g 45 -l 3 -o | waterfall.exe
```

**waterfall.exe** — Displays spectrum with tick detection markers

- Center line = carrier
- Red dots = detected ticks (when threshold exceeded)
- You should see dots appearing once per second
- Gap at seconds 29 and 59 (no tick transmitted)

### What Good Reception Looks Like

- Clear carrier line in center
- Visible 1000 Hz sidebands during ticks
- Regular red dots at 1-second intervals
- Occasional gaps at :29 and :59

### What Bad Reception Looks Like

- Fuzzy, noisy display
- Random or missing red dots
- No clear carrier line
- Heavy QRM/QRN interference

---

## Next Steps

### Near Term
1. **Threshold auto-calibration** — Adapt to signal conditions automatically
2. **Edge timing measurement** — Measure tick arrival time vs GPS PPS
3. **Statistics logging** — Hit rate, timing jitter, SNR estimates

### Medium Term
1. **Multi-frequency scanning** — Find best WWV frequency automatically
2. **Clock discipline** — Use tick timing to correct local clock drift
3. **Minute/hour marker detection** — Additional timing references

### Long Term
1. **NTP-like time service** — Provide timing to other applications
2. **Integration with modem** — Timing reference for MIL-STD-188-110A

---

## Testing Notes for Beta Testers

### Best Times to Test

- **Daytime (10, 15 MHz):** Usually good propagation to continental US
- **Night (5 MHz):** Often better than higher frequencies after sunset
- **Avoid gray line:** Rapid changes during sunrise/sunset transitions

### Gain Settings

Start with `-g 45 -l 3` and adjust:
- Too much gain = distortion, false triggers
- Too little gain = weak signal, missed ticks
- S-meter guidance: aim for S5-S7 on peaks

### Reporting Issues

When reporting problems, include:
- Frequency used
- Time of day (UTC)
- Gain settings (`-g` and `-l` values)
- What you observed vs expected
- Screenshot of waterfall if possible

---

## The Bottom Line

We're building a smoke test for the SDR chain, and it's working. The next phase is turning "it detects ticks" into "it measures tick timing precisely enough for clock discipline."

Stay tuned — updates will be posted as we progress.

---

*Phoenix Nest MARS Suite — Building reliable HF digital communications*
