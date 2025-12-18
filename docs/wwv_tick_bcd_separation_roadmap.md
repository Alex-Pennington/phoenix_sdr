# WWV Tick/BCD Separation Roadmap

## Phoenix SDR Modem Enhancement Plan v1.0

**Date:** December 18, 2025  
**Status:** Draft for Review  
**Priority:** Critical - Blocking sync lock on marginal signals

---

## Executive Summary

The tick detector is experiencing false detections of ~497ms duration caused by the 100 Hz BCD subcarrier's 10th harmonic bleeding into the 1000 Hz tick detection band. This prevents reliable sync lock on marginal signals (observed on 25 MHz WWV).

Research into existing WWV receiver implementations (NTP driver36, commercial time receivers, amateur radio designs) reveals a well-documented solution: **NIST designed a 40ms protected zone around each tick where the BCD subcarrier is guaranteed silent.** Our current implementation doesn't exploit this window.

This roadmap outlines changes in priority order, from quick wins to comprehensive architectural improvements.

---

## Problem Statement

### Observed Symptoms
```
[   30.1s] TICK #29    int=  1004ms  avg=  1000ms  corr=13.9   ← Working
[   42.1s] REJECTED: dur=497ms (gap zone 50-600ms)             ← Broken
[   43.1s] REJECTED: dur=497ms (gap zone 50-600ms)
[   44.1s] REJECTED: dur=497ms (gap zone 50-600ms)
...continues indefinitely...
```

### Root Cause
- WWV 100 Hz BCD subcarrier has harmonics at 200, 300, 400... **1000 Hz**
- BCD_ONE symbol has 500ms duration → creates 497ms energy pulse at 1000 Hz
- Tick detector sees this as "1000 Hz energy for 497ms" and rejects it
- Real 5ms ticks are masked by continuous harmonic energy

### Why This Wasn't Obvious
- Works fine on strong signals (tick amplitude dominates harmonics)
- Works during BCD_ZERO symbols (200ms, shorter than gap zone)
- Breaks specifically on BCD_ONE symbols (500ms) and P-markers (800ms)

---

## NIST Signal Timing (Critical Reference)

```
Second boundary
    │
    ├─── 0-10ms ────► Silence (modulation suppressed)
    │
    ├─── 10-15ms ───► *** 5ms TICK PULSE (1000 Hz) ***
    │
    ├─── 15-30ms ───► Silence (modulation suppressed)
    │
    ├─── 30-830ms ──► BCD subcarrier active (100 Hz)
    │                 Duration: 200ms (ZERO), 500ms (ONE), 800ms (MARKER)
    │
    └─── remainder ─► Audio tones (500/600 Hz per schedule)
```

**Key Insight:** The 40ms protected zone (0-30ms after second + 10ms before) is INTERFERENCE-FREE by design. NIST built this specifically so receivers could detect ticks without BCD interference.

---

## Phase 1: Quick Wins (1-2 days)

### 1.1 Tighten Pulse Width Validation

**File:** `tick_detector.c`

**Current:**
```c
#define TICK_MIN_DURATION_MS    2.0f
#define TICK_MAX_DURATION_MS    50.0f   // Too permissive
```

**Proposed:**
```c
#define TICK_MIN_DURATION_MS    2.0f
#define TICK_MAX_DURATION_MS    15.0f   // Real tick is 5ms, allow some margin
```

**Rationale:** Real tick is exactly 5ms. Even with filter ringing, shouldn't exceed 15ms. This alone won't fix detection during BCD pulses but will reject obvious false positives faster.

**Risk:** Low - only tightens existing validation
**Testing:** Verify tick detection still works on known-good signals

---

### 1.2 Add Highpass Filter Before Tick Detector

**File:** `waterfall.c` (detector path)

**Location:** After lowpass filter, before `tick_detector_process_sample()`

**Proposed:**
```c
// 2nd-order Butterworth highpass at 500 Hz
// Passes 1000 Hz tick, rejects 100 Hz BCD fundamental + harmonics 2-4
// 5th harmonic (500 Hz) is at cutoff, ~6dB down
// 10th harmonic (1000 Hz) passes with minimal attenuation

static float hp_x1 = 0, hp_x2 = 0, hp_y1 = 0, hp_y2 = 0;

// Coefficients for fc=500Hz, fs=50000Hz (detector path sample rate)
const float b0_hp =  0.9695f;
const float b1_hp = -1.9391f;
const float b2_hp =  0.9695f;
const float a1_hp = -1.9380f;
const float a2_hp =  0.9401f;

float hp_out = b0_hp * det_sample + b1_hp * hp_x1 + b2_hp * hp_x2
             - a1_hp * hp_y1 - a2_hp * hp_y2;
hp_x2 = hp_x1; hp_x1 = det_sample;
hp_y2 = hp_y1; hp_y1 = hp_out;

tick_detector_process_sample(g_tick_detector, hp_out_i, hp_out_q);
```

**Rationale:** Removes 100 Hz fundamental and low harmonics. The 10th harmonic at 1000 Hz still passes, but with reduced energy since the fundamental is gone.

**Risk:** Low - doesn't change tick detector internals
**Testing:** 
- Verify tick detection unchanged on strong signals
- Verify BCD detection unaffected (separate path)
- Check if 497ms rejections decrease

---

## Phase 2: Timing-Based Gating (3-5 days)

### 2.1 Add Tick Detection Window Gating

**Concept:** Only look for ticks during the 0-30ms protected zone after each second boundary. Outside this window, ignore 1000 Hz energy entirely.

**File:** `tick_detector.c`

**New State:**
```c
typedef struct {
    bool gating_enabled;
    float predicted_second_ms;      // When we expect next second
    float gate_early_ms;            // How early to open gate (default: 5ms)
    float gate_late_ms;             // How late to keep gate open (default: 35ms)
    float last_valid_tick_ms;       // Timestamp of last confirmed tick
    int consecutive_valid_ticks;    // Count of good intervals
    bool epoch_locked;              // True when we trust our timing
} tick_gating_t;
```

**Logic:**
```c
bool tick_detector_is_gate_open(tick_detector_t *td, float current_ms) {
    if (!td->gating.gating_enabled || !td->gating.epoch_locked) {
        return true;  // Gate always open until we have epoch
    }
    
    float ms_into_second = fmodf(current_ms - td->gating.predicted_second_ms, 1000.0f);
    if (ms_into_second < 0) ms_into_second += 1000.0f;
    
    // Gate open from -5ms to +35ms around second boundary
    return (ms_into_second < td->gating.gate_late_ms) || 
           (ms_into_second > (1000.0f - td->gating.gate_early_ms));
}
```

**Epoch Acquisition:**
```c
// After detecting tick with good interval (950-1050ms):
td->gating.consecutive_valid_ticks++;
if (td->gating.consecutive_valid_ticks >= 5) {
    td->gating.epoch_locked = true;
    td->gating.predicted_second_ms = current_tick_ms;
}

// Update prediction on each valid tick:
if (td->gating.epoch_locked) {
    // Weighted average: 90% prediction, 10% measurement
    float error = current_tick_ms - td->gating.predicted_second_ms;
    error = fmodf(error + 500.0f, 1000.0f) - 500.0f;  // Wrap to ±500ms
    td->gating.predicted_second_ms += 0.1f * error;
}
```

**Risk:** Medium - new state machine, but isolated to tick detector
**Testing:**
- Verify epoch acquisition within 10 seconds on good signal
- Verify gating prevents 497ms false detections
- Verify recovery if epoch is lost (bad propagation)

---

### 2.2 Integrate Gating with Sync Detector

**File:** `sync_detector.c`

Once sync detector has confirmed markers at 60s intervals, it has authoritative epoch. Feed this to tick detector:

```c
void sync_detector_update_tick_epoch(sync_detector_t *sd, tick_detector_t *td) {
    if (sd->state >= SYNC_TENTATIVE && sd->last_confirmed_marker_ms > 0) {
        float epoch_ms = fmodf(sd->last_confirmed_marker_ms, 1000.0f);
        tick_detector_set_epoch(td, epoch_ms, true);
    }
}
```

**Rationale:** Markers are reliable even on marginal signals. Use marker-derived epoch to gate tick detection, creating positive feedback loop.

---

## Phase 3: Parallel Filter Architecture (5-7 days)

### 3.1 Split Detector Path into Sync and Data Channels

**Concept:** Process 1000 Hz ticks and 100 Hz BCD through completely separate filter chains immediately after I/Q decimation.

**File:** `waterfall.c`

**Current Architecture:**
```
I/Q (2MHz) → Lowpass (5kHz) → Decimate (40:1) → 50kHz stream
                                                    │
                                                    ├─→ tick_detector
                                                    ├─→ marker_detector  
                                                    ├─→ bcd_time_detector
                                                    └─→ bcd_freq_detector
```

**Proposed Architecture:**
```
I/Q (2MHz) → Lowpass (5kHz) → Decimate (40:1) → 50kHz stream
                                                    │
                              ┌─────────────────────┴─────────────────────┐
                              │                                           │
                              ▼                                           ▼
                    ┌─────────────────┐                         ┌─────────────────┐
                    │  SYNC CHANNEL   │                         │  DATA CHANNEL   │
                    │                 │                         │                 │
                    │  Bandpass       │                         │  Lowpass        │
                    │  800-1400 Hz    │                         │  150 Hz         │
                    │  4th order      │                         │  4th order      │
                    └────────┬────────┘                         └────────┬────────┘
                             │                                           │
                             ▼                                           ▼
                    ┌─────────────────┐                         ┌─────────────────┐
                    │ tick_detector   │                         │ bcd_time_det    │
                    │ marker_detector │                         │ bcd_freq_det    │
                    └─────────────────┘                         └─────────────────┘
```

**Filter Specifications:**

| Filter | Type | Order | Passband | Stopband Atten |
|--------|------|-------|----------|----------------|
| Sync BPF | Butterworth | 4 | 800-1400 Hz | -40 dB @ 500 Hz |
| Data LPF | Butterworth | 4 | 0-150 Hz | -40 dB @ 300 Hz |

**Implementation:**
```c
// Sync channel bandpass (cascade of highpass + lowpass)
typedef struct {
    // Highpass at 800 Hz
    float hp_x1, hp_x2, hp_y1, hp_y2;
    float hp_b0, hp_b1, hp_b2, hp_a1, hp_a2;
    // Lowpass at 1400 Hz  
    float lp_x1, lp_x2, lp_y1, lp_y2;
    float lp_b0, lp_b1, lp_b2, lp_a1, lp_a2;
} sync_filter_t;

// Data channel lowpass
typedef struct {
    float x1, x2, y1, y2;
    float b0, b1, b2, a1, a2;
} data_filter_t;
```

**Risk:** Medium - significant refactor but well-understood DSP
**Testing:**
- Verify tick detection works through sync channel
- Verify BCD detection works through data channel
- Verify no cross-contamination between channels

---

## Phase 4: Enhanced Matched Filtering (3-5 days)

### 4.1 Optimize Tick Matched Filter

**File:** `tick_detector.c`

**Current:** 250-sample matched filter (5ms at 50kHz)

**Enhancement:** Add windowing to reduce sidelobe response

```c
// Current template generation
for (int i = 0; i < TICK_TEMPLATE_SAMPLES; i++) {
    float t = (float)i / TICK_SAMPLE_RATE;
    float window = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (TICK_TEMPLATE_SAMPLES - 1)));
    td->template_i[i] = cosf(2.0f * M_PI * TICK_TARGET_FREQ_HZ * t) * window;
    td->template_q[i] = sinf(2.0f * M_PI * TICK_TARGET_FREQ_HZ * t) * window;
}
```

**Consider:** Blackman-Harris window for better sidelobe suppression (-92 dB vs -43 dB for Hann)

```c
// Blackman-Harris window
float window = 0.35875f 
             - 0.48829f * cosf(2.0f * M_PI * i / (N - 1))
             + 0.14128f * cosf(4.0f * M_PI * i / (N - 1))
             - 0.01168f * cosf(6.0f * M_PI * i / (N - 1));
```

**Rationale:** Better window = less response to nearby frequencies = less BCD harmonic leakage

---

### 4.2 Add Comb Filter for Epoch Tracking

**Concept:** 1-second comb filter that coherently averages tick detections across multiple seconds, providing massive processing gain.

**File:** New `tick_comb_filter.c`

```c
#define COMB_STAGES 50000  // 1 second at 50kHz

typedef struct {
    float *delay_line;
    int write_idx;
    float alpha;           // Averaging coefficient (0.99 = ~100s time constant)
    float output;
} comb_filter_t;

float comb_filter_process(comb_filter_t *cf, float input) {
    float delayed = cf->delay_line[cf->write_idx];
    cf->output = cf->alpha * cf->output + (1.0f - cf->alpha) * (input + delayed) / 2.0f;
    cf->delay_line[cf->write_idx] = input;
    cf->write_idx = (cf->write_idx + 1) % COMB_STAGES;
    return cf->output;
}
```

**Usage:** Feed matched filter output through comb filter. Periodic tick signal adds coherently; non-periodic interference averages to zero.

**Processing Gain:** At α=0.99, approximately 20 dB improvement in SNR for periodic signals.

**Risk:** Low - additional processing, doesn't change existing detection
**Testing:** Verify epoch detection on marginal signals that currently fail

---

## Phase 5: Signal Normalization (2-3 days)

### 5.1 Implement Slow AGC Before Detector Path

**File:** `waterfall.c`

**Concept:** Normalize signal level before tick detection so thresholds work consistently across gain settings.

```c
// Slow envelope tracker (τ ≈ 10 seconds)
static float g_signal_level = 0.01f;
#define LEVEL_ADAPT_RATE 0.0001f

// In detector path processing loop:
float mag = sqrtf(det_i * det_i + det_q * det_q);
g_signal_level += LEVEL_ADAPT_RATE * (mag - g_signal_level);

float norm = 1.0f / (g_signal_level + 0.0001f);
float norm_i = det_i * norm;
float norm_q = det_q * norm;

tick_detector_process_sample(g_tick_detector, norm_i, norm_q);
```

**Rationale:** Addresses the earlier-identified issue where tick detection degrades at high gain (low GR) settings.

---

## Implementation Priority Matrix

| Phase | Change | Impact | Effort | Risk | Priority |
|-------|--------|--------|--------|------|----------|
| 1.1 | Tighten pulse width | Low | 1 hr | Low | P1 |
| 1.2 | Add highpass filter | Medium | 4 hr | Low | P1 |
| 2.1 | Timing-based gating | High | 2 days | Medium | P1 |
| 2.2 | Sync→Tick epoch feed | Medium | 4 hr | Low | P2 |
| 3.1 | Parallel filter banks | High | 5 days | Medium | P2 |
| 4.1 | Optimize matched filter | Low | 4 hr | Low | P3 |
| 4.2 | Add comb filter | Medium | 2 days | Low | P3 |
| 5.1 | Signal normalization | Medium | 1 day | Low | P2 |

**Recommended Implementation Order:**
1. Phase 1.1 + 1.2 (quick wins, validate theory)
2. Phase 2.1 (timing gating - highest impact)
3. Phase 5.1 (signal normalization - addresses gain issues)
4. Phase 2.2 + 3.1 (architectural improvements)
5. Phase 4.1 + 4.2 (optimization)

---

## Acceptance Criteria

### Minimum Viable (Phase 1-2 Complete)
- [ ] No 497ms false tick detections on 25 MHz marginal signal
- [ ] Tick detection rate >90% during BCD_ONE symbols
- [ ] Sync reaches LOCKED within 3 minutes on marginal signal
- [ ] No regression on strong signals (5, 10, 15 MHz)

### Full Implementation (All Phases Complete)
- [ ] Tick detection rate >95% on all signals
- [ ] Sub-10ms epoch accuracy after 30 seconds
- [ ] BCD decode rate >98% when sync LOCKED
- [ ] Consistent performance across GR=40 to GR=60
- [ ] Recovery from signal loss within 2 marker intervals

---

## Testing Plan

### Unit Tests
- Filter frequency response verification
- Gating window timing accuracy
- Matched filter correlation peak shape

### Integration Tests
- Live signal: 5 MHz (strong), 10 MHz (medium), 25 MHz (weak)
- Signal generator: Controlled BCD patterns
- Overnight soak test: 8+ hours continuous operation

### Regression Tests
- Existing tick detection on known-good recordings
- Marker detection timing accuracy
- Sync state machine transitions

---

## References

1. NTP Driver36 Documentation: https://www.eecis.udel.edu/~mills/ntp/html/drivers/driver36.html
2. Mills, D.L., "WWV/H Audio Demodulator/Decoder", University of Delaware
3. NIST Special Publication: WWV/WWVH Broadcast Format
4. Phoenix SDR Architecture: /mnt/transcripts/2025-12-18-*

---

## Appendix A: Filter Coefficient Calculator

```python
# Python script to generate filter coefficients
from scipy.signal import butter, ellip, sosfreqz
import numpy as np

fs = 50000  # 50 kHz sample rate

# Sync channel bandpass (800-1400 Hz)
sos_sync = butter(4, [800, 1400], btype='band', fs=fs, output='sos')
print("Sync BPF coefficients:", sos_sync)

# Data channel lowpass (150 Hz)
sos_data = butter(4, 150, btype='low', fs=fs, output='sos')
print("Data LPF coefficients:", sos_data)

# Highpass for Phase 1.2 (500 Hz)
sos_hp = butter(2, 500, btype='high', fs=fs, output='sos')
print("Highpass coefficients:", sos_hp)
```

---

## Appendix B: Quick Reference - WWV Timing

| Second | Content | Tick? | BCD Symbol |
|--------|---------|-------|------------|
| :00 | Minute marker | 800ms tone | P0 (800ms) |
| :01-:08 | Minutes tens/units | 5ms | Data |
| :09 | Position marker | 5ms | P1 (800ms) |
| :10-:18 | Hours tens/units | 5ms | Data |
| :19 | Position marker | 5ms | P2 (800ms) |
| :20-:28 | Days hundreds/tens | 5ms | Data |
| :29 | Position marker | NO TICK | P3 (800ms) |
| :30-:38 | Days units, reserved | 5ms | Data |
| :39 | Position marker | 5ms | P4 (800ms) |
| :40-:48 | Year tens/units | 5ms | Data |
| :49 | Position marker | 5ms | P5 (800ms) |
| :50-:58 | UT1 correction, DUT1 | 5ms | Data |
| :59 | Position marker | NO TICK | P0 (800ms) |

**Note:** Seconds 29 and 59 have NO tick pulse per NIST spec.
