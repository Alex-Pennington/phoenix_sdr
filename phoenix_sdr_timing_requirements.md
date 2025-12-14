# MIL-188-110A Timing Requirements and Phoenix SDR Waterfall Implementation

**Author:** Rayven Redding (KY4OLB)  
**Date:** December 14, 2025  
**Project:** Phoenix Nest MARS Suite - SDR Component

## Overview

This document summarizes the timing requirements extracted from the MIL-188-110A modem core source code (as used in MS-DMT) and the "HF Serial-Tone Waveform Design" paper by Jorgenson & Moreland. These requirements inform the FFT resolution needed in the Phoenix SDR waterfall display for eventual modem integration.

## MIL-188-110A Core Timing Parameters

Extracted directly from `Cm110s.h` and `txm110a.cpp`:

| Parameter | Value | Source |
|-----------|-------|--------|
| Sample Rate | 9600 Hz | `#define M1_SAMPLE_RATE 9600` |
| Samples per Symbol | 4 | `tx_symbol()` loop: `for( k = 0; k < 4; k++ )` |
| **Symbol Rate** | **2400 baud** | 9600 ÷ 4 |
| **Symbol Duration** | **0.4167 ms** | 1 ÷ 2400 |
| Center Frequency | 1800 Hz | `#define M1_CENTER_FREQUENCY 1800` |
| Block Length | 1920 samples | `#define M1_RX_BLOCK_LENGTH 1920` |
| Block Duration | 200 ms | 1920 ÷ 9600 |

## Why Sub-Millisecond Resolution Matters

From the HF Serial-Tone Waveform Design paper (Section 6.0 - Equalization):

> "Equalization is required for serial tone modulations where the symbol duration, **typically 0.4167 ms**, is small relative to the expected time dispersion, which is often as severe as **several milliseconds**."

The ionosphere causes multipath propagation that smears symbols across 1-4 ms (mid-latitude) or even 10+ ms (high-latitude). The equalizer must resolve individual symbols to compensate for this distortion. This requires timing resolution better than the symbol period.

### Channel Characteristics (from the paper)

- **Typical mid-latitude delay spread:** 1-4 ms
- **Typical Doppler spread:** ≤1 Hz
- **High-latitude worst case:** >10 ms delay spread, >50 Hz Doppler
- **Channel bandwidth:** 3 kHz (military HF allocation)

## Phoenix SDR Waterfall Implementation

### Current State (v0.2.5)

The waterfall display processes audio from `simple_am_receiver.exe` and displays FFT output with frequency bucket monitoring for WWV tick detection.

**Current timing resolution:**
- FFT Size: 1024 samples
- Sample Rate: 48000 Hz
- Frame Duration: **21.3 ms** (1024 ÷ 48000)

This is ~50× too coarse for modem symbol-level work.

### Target Implementation

To support eventual MIL-188-110A integration, we're implementing overlapping FFT with configurable hop size:

| Hop Size | Time Resolution | FFTs/Second | Use Case |
|----------|-----------------|-------------|----------|
| 1024 | 21.3 ms | 47 | Current (display only) |
| 256 | 5.3 ms | 188 | WWV tick detection |
| 48 | 1.0 ms | 1000 | General modem work |
| **24** | **0.5 ms** | **2000** | **MIL-188-110A symbol timing** |

**Recommended default: HOP_SIZE = 24 (0.5 ms resolution)**

This provides better-than-symbol-period resolution while remaining computationally trivial for modern hardware.

### Implementation Notes

The change is straightforward - instead of reading a full FFT frame between each transform, we maintain a sliding window:

```c
#define FFT_SIZE    1024
#define HOP_SIZE    24      /* 0.5ms at 48kHz */

int16_t ring_buffer[FFT_SIZE];

/* Each iteration: */
fread(new_samples, HOP_SIZE);
memmove(ring_buffer, ring_buffer + HOP_SIZE, (FFT_SIZE - HOP_SIZE) * sizeof(int16_t));
memcpy(ring_buffer + FFT_SIZE - HOP_SIZE, new_samples, HOP_SIZE * sizeof(int16_t));
/* FFT the full ring_buffer */
```

Frequency resolution remains unchanged at 46.9 Hz/bin (48000 ÷ 1024).

## Signal Flow Reference

From the MS-DMT documentation, for those implementing compatible systems:

### Transmit Path
```
Message Bytes → Viterbi Encode → Interleave → PSK Symbol Mapping
    → FIR Filter (4 samples/symbol) → Upconvert to 1800 Hz → Audio Out
```

### Receive Path
```
Audio In → Downconvert from 1800 Hz → AGC → Filter/Decimate
    → Preamble Hunt → Equalize → Demodulate → De-interleave
    → Viterbi Decode → Message Bytes
```

## Relationship to WWV Work

The current WWV tick detection work serves as a proving ground for the SDR capture chain. WWV's 5 ms second ticks at 1000 Hz are a convenient intermediate target:

- **WWV tick duration:** 5 ms (requires ~2-3 ms resolution to characterize shape)
- **MIL-188-110A symbol:** 0.4167 ms (requires ~0.5 ms resolution)

By targeting 0.5 ms resolution now, we satisfy both requirements with a single implementation.

## References

1. **MIL-STD-188-110A** - Interoperability and Performance Standards for Data Modems
2. **Jorgenson, M.B. & Moreland, K.W.** - "HF Serial-Tone Waveform Design" - Communications Research Centre, Ottawa, Canada (RTO IST Symposium, 1999)
3. **MS-DMT Modem Core Source** - As provided by N2CKH for Phoenix Nest integration
4. **STANAG 4285 / STANAG 5066 Annex G** - NATO HF waveform standards (referenced in design paper)

## Repository

Phoenix SDR development: https://github.com/Alex-Pennington/phoenix_sdr

---

*This document was prepared as part of the Phoenix Nest MARS Suite development effort. Questions or corrections welcome on the groups.io forum.*
