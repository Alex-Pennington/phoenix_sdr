# WWV Test Signal Generator

## Signal Domain Specification for SDR Implementation

**Phoenix Nest LLC – December 2025**

---

## 1. System Parameters

The test signal generator operates within the Phoenix SDR framework with the following constraints:

| Parameter | Value | Notes |
|-----------|-------|-------|
| **IQ Sample Rate** | 2,000,000 sps | 2 Msps |
| **Sample Period** | 500 ns | Timing resolution |
| **SDR Bandwidth** | 10 MHz | Full capture |
| **Samples per Second** | 2,000,000 | Exact |
| **Samples per Minute** | 120,000,000 | 60 × 2M |
| **Samples per 5 ms Tick** | 10,000 | Second pulse |
| **Samples per 800 ms Marker** | 1,600,000 | Minute/hour |
| **Samples per 100 Hz Cycle** | 20,000 | BCD subcarrier |

---

## 2. Signal Domain Catalog

WWV transmits multiple signals simultaneously via amplitude modulation. All signals below are summed into a composite AM envelope applied to the RF carrier.

### 2.1 Complete Signal Inventory

| Signal | Frequency | Amplitude | Duration | Occurrence |
|--------|-----------|-----------|----------|------------|
| **BCD Subcarrier** | 100 Hz | 18% / 3% | Continuous | Every second (PWM) |
| **Second Tick** | 1000 Hz | 100% | 5 ms | Every sec (not 29, 59) |
| **Minute Marker** | 1000 Hz | 100% | 800 ms | Second 0 each minute |
| **Hour Marker** | 1500 Hz | 100% | 800 ms | Second 0, minute 0 |
| **UT1 Double Tick** | 1000 Hz | 100% | 5 ms | Seconds 1-16 (cond.) |
| **500 Hz Tone** | 500 Hz | 50% | ~960 ms/sec | Scheduled minutes |
| **600 Hz Tone** | 600 Hz | 50% | ~960 ms/sec | Scheduled minutes |
| **440 Hz Tone** | 440 Hz | 50% | Full minute | Minute 2 only (WWV) |

### 2.2 Modulation Depth per NIST Specification

| Signal Component | Modulation Depth |
|------------------|------------------|
| Second/Minute/Hour Pulses | 100% (full carrier deviation) |
| Standard Audio Tones (440, 500, 600 Hz) | 50% |
| Voice Announcements | 75% |
| BCD Subcarrier – High (binary mark) | 18% (−15 dBc) |
| BCD Subcarrier – Low (space) | 3% (−30 dBc) |

---

## 3. Temporal Structure of One Second

Each second follows a precise timing structure with guard zones around the on-time marker. The on-time reference is the **leading edge of the 5 ms tone pulse**.

### 3.1 Standard Second Structure (Seconds 1-28, 30-58)

| Start (ms) | End (ms) | Duration | Content |
|------------|----------|----------|---------|
| 0 | 10 | 10 ms | SILENCE – Pre-tick guard zone |
| 10 | 15 | 5 ms | TICK – 1000 Hz pulse at 100% modulation |
| 15 | 40 | 25 ms | SILENCE – Post-tick guard zone |
| 40 | 1000 | 960 ms | TONE – Background audio + BCD subcarrier |

**Sample counts at 2 Msps:** Guard 1 = 20,000 samples, Tick = 10,000 samples, Guard 2 = 50,000 samples, Tone = 1,920,000 samples

### 3.2 Minute Marker (Second 0 of Each Minute)

| Start (ms) | End (ms) | Duration | Content |
|------------|----------|----------|---------|
| 0 | 10 | 10 ms | SILENCE – Pre-marker guard |
| 10 | 810 | 800 ms | MARKER – 1000 Hz pulse at 100% modulation |
| 810 | 835 | 25 ms | SILENCE – Post-marker guard |
| 835 | 1000 | 165 ms | TONE – Background audio + BCD subcarrier |

**Sample counts:** Guard 1 = 20,000, Marker = 1,600,000, Guard 2 = 50,000, Tone = 330,000 samples

### 3.3 Hour Marker (Second 0 of Minute 0)

Identical structure to minute marker, except the tone frequency is **1500 Hz** instead of 1000 Hz.

### 3.4 Silent Seconds (29 and 59)

No tick pulse. The 40 ms guard zone is entirely silent. Background tone and BCD subcarrier continue for the full 1000 ms.

---

## 4. BCD Time Code (100 Hz Subcarrier)

The 100 Hz subcarrier uses pulse-width modulation to encode binary data. One bit is transmitted per second using modified IRIG-H format.

### 4.1 Bit Encoding

| Symbol | High Duration | Low Duration | Meaning |
|--------|---------------|--------------|---------|
| **Binary 0** | 200 ms | 800 ms | Data bit = 0 |
| **Binary 1** | 500 ms | 500 ms | Data bit = 1 |
| **Position Marker** | 800 ms | 200 ms | Frame sync |

**Important:** WWV transmits LSB first with 1-2-4-8 weighting (opposite of WWVB). High = 18% modulation (−15 dBc), Low = 3% modulation (−30 dBc).

### 4.2 Frame Structure (Per-Second Bit Assignments)

Each minute transmits 60 bits. The following table shows the complete bit map:

| Second | Weight | Content |
|--------|--------|---------|
| 0 | — | Frame Reference Marker (FRM) – always marker |
| 1 | — | Unused (always 0) |
| 2 | — | Unused (always 0) – Note: Also DST status at 00:00Z |
| 3 | — | Unused (always 0) – Note: Leap second warning |
| 4 | — | Unused (always 0) |
| 5 | 1 | Minutes – units digit bit 0 (LSB) |
| 6 | 2 | Minutes – units digit bit 1 |
| 7 | 4 | Minutes – units digit bit 2 |
| 8 | 8 | Minutes – units digit bit 3 |
| 9 | — | Position Marker P1 |
| 10 | 10 | Minutes – tens digit bit 0 |
| 11 | 20 | Minutes – tens digit bit 1 |
| 12 | 40 | Minutes – tens digit bit 2 |
| 13-14 | — | Unused (always 0) |
| 15 | 1 | Hours – units digit bit 0 (LSB) |
| 16 | 2 | Hours – units digit bit 1 |
| 17 | 4 | Hours – units digit bit 2 |
| 18 | 8 | Hours – units digit bit 3 |
| 19 | — | Position Marker P2 |
| 20 | 10 | Hours – tens digit bit 0 |
| 21 | 20 | Hours – tens digit bit 1 |
| 22-24 | — | Unused (always 0) |
| 25 | 1 | Day of Year – units digit bit 0 (LSB) |
| 26 | 2 | Day of Year – units digit bit 1 |
| 27 | 4 | Day of Year – units digit bit 2 |
| 28 | 8 | Day of Year – units digit bit 3 |
| 29 | — | Position Marker P3 – Note: No tick pulse this second |
| 30 | 10 | Day of Year – tens digit bit 0 |
| 31 | 20 | Day of Year – tens digit bit 1 |
| 32 | 40 | Day of Year – tens digit bit 2 |
| 33 | 80 | Day of Year – tens digit bit 3 |
| 34 | — | Unused (always 0) |
| 35 | 100 | Day of Year – hundreds digit bit 0 |
| 36 | 200 | Day of Year – hundreds digit bit 1 |
| 37-38 | — | DUT1 sign – positive indicator bits |
| 39 | — | Position Marker P4 |
| 40-42 | — | Unused (always 0) |
| 43 | — | DUT1 sign – negative indicator bit |
| 44 | — | Unused (always 0) |
| 45 | 1 | Year – units digit bit 0 (LSB) |
| 46 | 2 | Year – units digit bit 1 |
| 47 | 4 | Year – units digit bit 2 |
| 48 | 8 | Year – units digit bit 3 |
| 49 | — | Position Marker P5 |
| 50 | 0.1s | DUT1 magnitude – 0.1 second weight |
| 51 | 0.2s | DUT1 magnitude – 0.2 second weight |
| 52 | 0.4s | DUT1 magnitude – 0.4 second weight |
| 53 | 10 | Year – tens digit bit 0 |
| 54 | 20 | Year – tens digit bit 1 |
| 55 | — | DST status at 24:00Z today |
| 56 | 40 | Year – tens digit bit 2 |
| 57 | 80 | Year – tens digit bit 3 |
| 58 | — | Unused (always 0) |
| 59 | — | Position Marker P0 – Note: No tick pulse this second |

---

## 5. UT1 Correction Encoding

UT1 corrections are encoded using two methods: audible double ticks and BCD-encoded magnitude/sign.

### 5.1 Double Tick Method (Audible)

Seconds 1-16 may contain an additional 5 ms tick pulse following the standard tick. The count and position encode the UT1-UTC correction:

- **Positive correction:** Doubled ticks in seconds 1-8
- **Negative correction:** Doubled ticks in seconds 9-16
- **Magnitude:** Number of doubled ticks × 0.1 seconds

*Example:* Doubled ticks at seconds 1, 2, 3 → UT1 = UTC + 0.3s

### 5.2 BCD Method

| Seconds | Encoding |
|---------|----------|
| 37-38 (both = 1) | Positive DUT1 sign |
| 43 (= 1) | Negative DUT1 sign |
| 50, 51, 52 | Magnitude in 0.1s units (weights: 0.1, 0.2, 0.4) |

---

## 6. Standard Audio Tone Schedule (WWV)

Background tones alternate between 500 Hz and 600 Hz on a per-minute schedule. Tones are gated during the 40 ms guard zone around each second tick.

| Tone | Minutes |
|------|---------|
| **500 Hz** | 4, 6, 12, 14, 16, 20, 22, 24, 26, 28, 32, 34, 36, 38, 40, 42, 52, 54, 56, 58 |
| **600 Hz** | 1, 3, 5, 7, 11, 13, 15, 17, 19, 21, 23, 25, 27, 31, 33, 35, 37, 39, 41, 53, 55, 57 |
| **440 Hz** | 2 (hourly marker tone – omitted during first hour of UTC day) |
| **None** | 0, 8, 9, 10, 18, 29, 30, 43, 44, 45, 46, 47, 48, 49, 50, 51, 59 |

---

## 7. Signal Composition

The composite baseband signal is the sum of all active components, which then amplitude-modulates the RF carrier:

**IQ(t) = Carrier(t) × [1 + m_pulse(t) + m_tone(t) + m_bcd(t)]**

Where each modulation component is computed per sample based on the current second, minute, and hour.

### 7.1 Per-Sample State Machine

At each sample (every 500 ns), compute:

1. **sample_in_second** = sample_counter mod 2,000,000
2. **second_in_minute** = (sample_counter / 2,000,000) mod 60
3. **minute_in_hour** = (sample_counter / 120,000,000) mod 60
4. **hour** = (sample_counter / 7,200,000,000) mod 24

### 7.2 Layer Activation Logic

**Pulse Layer:** Active during tick zones (samples 20,000–30,000 for normal tick, 20,000–1,620,000 for markers). Frequency = 1000 Hz normally, 1500 Hz for hour marker.

**Tone Layer:** Active outside guard zone (samples 80,000–2,000,000). Frequency selected by minute schedule. Amplitude = 50%.

**BCD Layer:** Always active at 100 Hz. First 30 ms of each second is low (3%). Remaining duration depends on bit value: 170 ms more for 0, 470 ms more for 1, 770 ms more for marker.

---

## 8. Implementation Notes

- **Timing reference:** All edges are sample-accurate. At 2 Msps, timing resolution is 500 ns – approximately 1 million times tighter than half-second discipline.

- **Timestamp source:** For test generation, arbitrary timestamps are acceptable. The decoder tests signal structure, not wall-clock accuracy.

- **Phase continuity:** All tone oscillators should maintain phase across guard zone boundaries to avoid discontinuities.

- **BCD subcarrier phase:** The 100 Hz subcarrier should be phase-coherent across the entire minute frame.

- **Voice omission:** Voice announcements are not required for timing discipline testing. Implement as silent periods or fixed test patterns.

- **Reference implementations:** See jj1bdx/WWV (C) and kuremu/wwv_simulator (Python) on GitHub for encoding examples.

---

*— End of Specification —*
