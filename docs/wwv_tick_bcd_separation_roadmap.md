# WWV Tick/BCD Separation Roadmap

## Phoenix SDR Modem Enhancement Plan v2.1

**Date:** December 18, 2025
**Status:** Draft for Review (Revised)
**Priority:** Critical - Blocking sync lock on marginal signals

---

## Executive Summary

The tick detector is experiencing false detections of ~497ms duration caused by the 100 Hz BCD subcarrier's 10th harmonic bleeding into the 1000 Hz tick detection band. This prevents reliable sync lock on marginal signals (observed on 25 MHz WWV).

Research into existing WWV receiver implementations (NTP driver36, commercial time receivers, amateur radio designs) reveals a well-documented solution: **NIST designed a 40ms protected zone around each tick where the BCD subcarrier is guaranteed silent.** Our current implementation doesn't exploit this window.

This roadmap outlines changes in priority order, from quick wins to comprehensive architectural improvements.

**Key Architectural Insight (v2.0):** Markers establish epoch before ticks can reliably lock on marginal signals. Phase 2 is reordered so marker-derived epoch bootstraps tick gating, solving the cold-start problem.

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

## Phase 1: Quick Wins + Validation (2-3 days)

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

**Proposed:** 2nd-order Butterworth highpass at 500 Hz (with compile-time 4th-order fallback)

```c
// Compile-time filter order selection for A/B testing
// Set to 2 for initial validation, change to 4 if insufficient improvement
#define TICK_HIGHPASS_ORDER 2  // 2 = -12dB/oct, 4 = -24dB/oct

typedef struct {
    float x1, x2;  // Input history
    float y1, y2;  // Output history
} biquad_state_t;

#if TICK_HIGHPASS_ORDER == 2
// 2nd-order Butterworth highpass @ 500Hz, fs=50kHz
// Attenuation: -20dB @ 100Hz, -12dB @ 200Hz, -6dB @ 300Hz
static biquad_state_t g_tick_hp_i = {0};
static biquad_state_t g_tick_hp_q = {0};

const float hp_b0 =  0.9695312529f;
const float hp_b1 = -1.9390625058f;
const float hp_b2 =  0.9695312529f;
const float hp_a1 = -1.9380050618f;
const float hp_a2 =  0.9401199497f;

float highpass_process(biquad_state_t *hp, float x) {
    float y = hp_b0*x + hp_b1*hp->x1 + hp_b2*hp->x2
            - hp_a1*hp->y1 - hp_a2*hp->y2;
    hp->x2 = hp->x1; hp->x1 = x;
    hp->y2 = hp->y1; hp->y1 = y;
    return y;
}

#elif TICK_HIGHPASS_ORDER == 4
// 4th-order Butterworth highpass @ 500Hz, fs=50kHz (cascade of 2 biquads)
// Attenuation: -40dB @ 100Hz, -24dB @ 200Hz, -12dB @ 300Hz
static biquad_state_t g_tick_hp_s1_i = {0}, g_tick_hp_s1_q = {0};
static biquad_state_t g_tick_hp_s2_i = {0}, g_tick_hp_s2_q = {0};

// Section 1 coefficients
const float hp_s1_b0 =  0.9847106486f;
const float hp_s1_b1 = -1.9694212973f;
const float hp_s1_b2 =  0.9847106486f;
const float hp_s1_a1 = -1.9688263941f;
const float hp_s1_a2 =  0.9700162005f;

// Section 2 coefficients
const float hp_s2_b0 =  0.9847106486f;
const float hp_s2_b1 = -1.9694212973f;
const float hp_s2_b2 =  0.9847106486f;
const float hp_s2_a1 = -1.9688263941f;
const float hp_s2_a2 =  0.9700162005f;

float biquad_process(biquad_state_t *bq, float x,
                     float b0, float b1, float b2, float a1, float a2) {
    float y = b0*x + b1*bq->x1 + b2*bq->x2 - a1*bq->y1 - a2*bq->y2;
    bq->x2 = bq->x1; bq->x1 = x;
    bq->y2 = bq->y1; bq->y1 = y;
    return y;
}

float highpass_process_4th(biquad_state_t *s1, biquad_state_t *s2, float x) {
    float y1 = biquad_process(s1, x, hp_s1_b0, hp_s1_b1, hp_s1_b2, hp_s1_a1, hp_s1_a2);
    return biquad_process(s2, y1, hp_s2_b0, hp_s2_b1, hp_s2_b2, hp_s2_a1, hp_s2_a2);
}
#endif

// In detector path:
#if TICK_HIGHPASS_ORDER == 2
float filt_i = highpass_process(&g_tick_hp_i, det_i);
float filt_q = highpass_process(&g_tick_hp_q, det_q);
#elif TICK_HIGHPASS_ORDER == 4
float filt_i = highpass_process_4th(&g_tick_hp_s1_i, &g_tick_hp_s2_i, det_i);
float filt_q = highpass_process_4th(&g_tick_hp_s1_q, &g_tick_hp_s2_q, det_q);
#endif
tick_detector_process_sample(g_tick_detector, filt_i, filt_q);
```

**Filter Order Selection:**

| Order | Attenuation @ 100Hz | Phase @ 1000Hz | When to Use |
|-------|---------------------|----------------|-------------|
| 2nd | -20 dB | ~18° | **Default** - start here |
| 4th | -40 dB | ~36° | Fallback if 2nd insufficient |

**A/B Testing Workflow:**
1. Build with `TICK_HIGHPASS_ORDER=2`, run Phase 1.3 validation
2. If <50% improvement in 497ms rejections, rebuild with `TICK_HIGHPASS_ORDER=4`
3. No code changes required between tests - just change define and rebuild

**Risk:** Low - doesn't change tick detector internals
**Testing:**
- Verify tick detection unchanged on strong signals
- Verify BCD detection unaffected (separate path)

---

### 1.3 Pre-Filter Validation Test (MANDATORY)

**Purpose:** Quantify improvement before investing in Phase 2. Stop here if theory is wrong.

**Procedure:**
```bash
# 1. Record baseline (no changes)
./waterfall.exe -t 192.168.1.153:4536 > baseline.log 2>&1 &
sleep 120  # 2 minutes on marginal signal (25 MHz)
kill %1

# 2. Count 497ms rejections
grep -c "dur=49[0-9]ms" baseline.log
# Example: 847 rejections

# 3. Apply Phase 1.1 + 1.2, rebuild, repeat recording
./waterfall.exe -t 192.168.1.153:4536 > filtered.log 2>&1 &
sleep 120
kill %1

# 4. Count again
grep -c "dur=49[0-9]ms" filtered.log
# Target: <424 rejections (50% reduction)
```

**Acceptance Gate:**
- [ ] 497ms rejection count reduced by >50%
- [ ] Tick detection rate on strong signal unchanged (±5%)
- [ ] No new rejection categories introduced

**If validation fails:**
1. Try 4th-order highpass (24 dB/octave vs 12 dB/octave)
2. If still failing, investigate: Is the problem really BCD harmonics?
3. Consider narrow bandpass (950-1050 Hz) as alternative approach

**Risk:** None - this is measurement only
**Effort:** 2 hours including rebuild and test cycles

---

### 1.4 Signal Normalization (Slow AGC)

**Rationale:** Moved to Phase 1 because gain-dependent thresholds could mask the filter improvement. Normalization helps both tick AND marker detection.

**File:** `waterfall.c` (detector path)

**Concept:** Normalize signal level before tick detection so thresholds work consistently across gain settings (GR=40 to GR=60).

```c
// Slow envelope tracker (τ ≈ 10 seconds at 50 kHz)
typedef struct {
    float level;
    float adapt_rate;
    int warmup_samples;
    int sample_count;
} signal_normalizer_t;

static signal_normalizer_t g_normalizer = {
    .level = 0.01f,
    .adapt_rate = 0.0001f,      // Slow: τ ≈ 10 seconds
    .warmup_samples = 50000,    // 1 second fast warmup
    .sample_count = 0
};

float normalize_sample(signal_normalizer_t *norm, float i, float q) {
    float mag = sqrtf(i*i + q*q);

    // Fast adaptation during warmup
    float rate = (norm->sample_count < norm->warmup_samples)
               ? 0.01f : norm->adapt_rate;

    norm->level += rate * (mag - norm->level);
    norm->sample_count++;

    // Clamp to prevent division issues
    if (norm->level < 0.0001f) norm->level = 0.0001f;

    return 1.0f / norm->level;
}

// In detector path (after highpass, before tick detector):
float norm_factor = normalize_sample(&g_normalizer, filt_i, filt_q);
tick_detector_process_sample(g_tick_detector, filt_i * norm_factor, filt_q * norm_factor);
```

**Reset on reconnect:**
```c
void reset_normalizer(signal_normalizer_t *norm) {
    norm->level = 0.01f;
    norm->sample_count = 0;
}

// In tcp_reconnect():
reset_normalizer(&g_normalizer);
```

**Risk:** Low - isolated change, adds processing
**Testing:**
- Verify consistent tick detection from GR=40 to GR=60
- Verify warmup completes within 2 seconds
- Verify recovery after signal dropout

---

### Phase 1 Acceptance Criteria

**Before proceeding to Phase 2, ALL must pass:**

- [ ] 497ms rejection count reduced by >50% (Phase 1.3 validation)
- [ ] No regression in tick detection on strong signals (5/10/15 MHz)
- [ ] Tick correlation values remain in expected range (>5.0)
- [ ] Tick detection consistent across GR=40 to GR=60 (after 1.4)
- [ ] BCD detection unaffected (separate signal path)

---

## Phase 2: Marker-Bootstrapped Tick Gating (3-5 days)

### Critical Design Decision: Solving the Cold Start Problem

**The chicken-and-egg problem:**
```
Need epoch to gate ticks → Need gated ticks to acquire epoch → ???
```

**Solution:** Use minute markers (already reliable on marginal signals) to bootstrap tick gating. Markers give us authoritative second-epoch because:
- Minute marker starts at second :00 + 10ms
- Therefore: `second_boundary = marker_start - 10ms`
- Next tick expected at: `second_boundary + 1000ms + 10ms` (tick is 10-15ms into second)

**Cold Start Sequence:**
```
1. Startup: Tick gating DISABLED (wide open, best effort detection)
2. Sync detector in ACQUIRING, accumulating marker evidence
3. First marker confirmed → TENTATIVE, epoch estimate available
4. Feed epoch to tick detector, enable gating with WIDE window (±50ms)
5. Gated ticks start arriving (BCD interference rejected)
6. Tick PLL refines epoch, window narrows to ±20ms
7. Second marker confirmed → sync confidence increases
8. Third marker → LOCKED, tick gating fully operational
```

---

### 2.1 Feed Marker Epoch to Tick Detector (FIRST)

**Rationale:** Markers work on marginal signals. Use them to bootstrap tick gating.

**Files:** `sync_detector.c`, `sync_detector.h`, `tick_detector.c`, `tick_detector.h`

**New Tick Detector API:**
```c
// tick_detector.h - Add to public API
void tick_detector_set_epoch(tick_detector_t *td, float epoch_ms, float confidence);
void tick_detector_set_gating_enabled(tick_detector_t *td, bool enabled);
float tick_detector_get_epoch(tick_detector_t *td);
bool tick_detector_is_gating_enabled(tick_detector_t *td);
```

**Sync Detector Integration:**
```c
// sync_detector.c - Call on marker confirmation

void sync_detector_feed_epoch_to_tick(sync_detector_t *sd, tick_detector_t *td) {
    if (sd->state < SYNC_TENTATIVE || sd->last_confirmed_marker_ms <= 0) {
        return;  // No reliable epoch yet
    }

    // Minute marker starts at second :00 + 10ms (per NIST spec)
    // So second boundary was 10ms before marker start
    float marker_start_ms = sd->last_confirmed_marker_ms;
    float second_boundary_ms = marker_start_ms - 10.0f;

    // Epoch is the fractional millisecond within each second
    float epoch_ms = fmodf(second_boundary_ms, 1000.0f);
    if (epoch_ms < 0) epoch_ms += 1000.0f;

    // Feed to tick detector with current confidence
    tick_detector_set_epoch(td, epoch_ms, sd->confidence);

    // Enable gating once we have TENTATIVE or better
    if (sd->state >= SYNC_TENTATIVE) {
        tick_detector_set_gating_enabled(td, true);
        printf("[SYNC] Fed epoch %.1fms to tick detector, gating enabled\n", epoch_ms);
    }
}
```

**Call Site:** In `sync_detector_confirm_marker()` after successful confirmation:
```c
// After confirming marker and updating state
if (sd->tick_detector_ref) {
    sync_detector_feed_epoch_to_tick(sd, sd->tick_detector_ref);
}
```

**Wiring:** In `waterfall.c` initialization:
```c
// After creating both detectors
sync_detector_set_tick_detector(g_sync_detector, g_tick_detector);
```

**Risk:** Low - adds API, doesn't change existing detection logic
**Testing:**
- Verify epoch is fed after first marker confirmation
- Verify epoch value is correct (second boundary mod 1000)
- Verify gating enables at TENTATIVE state

---

### 2.2 Implement Tick Detection Window Gating (SECOND)

**Depends on:** Phase 2.1 (needs epoch source before gating can work)

**File:** `tick_detector.c`

**New State in tick_detector struct:**
```c
typedef enum {
    EPOCH_SOURCE_NONE,      // No epoch yet
    EPOCH_SOURCE_TICKS,     // Derived from consecutive good ticks
    EPOCH_SOURCE_MARKER     // Derived from sync detector marker
} epoch_source_t;

typedef struct {
    bool enabled;                   // Gating active?
    float epoch_ms;                 // Millisecond offset of second boundary
    float window_ms;                // Half-width of detection window
    float confidence;               // Epoch confidence (0.0 - 1.0)
    epoch_source_t source;          // How we got our epoch
    int consecutive_good_ticks;     // For tick-derived epoch
    int consecutive_gated_ticks;    // Count since gating enabled
} tick_gating_state_t;

// Add to struct tick_detector:
tick_gating_state_t gating;
```

**Dual-Mode Epoch Acquisition:**

The cold-start problem: On marginal signals, BCD harmonics mask ticks during initial acquisition, but markers are reliable. We accept epoch from whichever source provides it first:

```c
// Mode A: Tick-derived epoch (works on strong signals)
// After detecting tick with good interval (950-1050ms):
void tick_detector_update_tick_epoch(tick_detector_t *td, float tick_ms, float interval_ms) {
    if (interval_ms >= 950.0f && interval_ms <= 1050.0f) {
        td->gating.consecutive_good_ticks++;

        // Need 5 consecutive good ticks to trust tick-derived epoch
        if (td->gating.consecutive_good_ticks >= 5 &&
            td->gating.source == EPOCH_SOURCE_NONE) {

            td->gating.epoch_ms = fmodf(tick_ms - 12.5f, 1000.0f);  // Tick at 12.5ms into second
            if (td->gating.epoch_ms < 0) td->gating.epoch_ms += 1000.0f;
            td->gating.source = EPOCH_SOURCE_TICKS;
            td->gating.confidence = 0.5f;
            td->gating.enabled = true;
            td->gating.window_ms = 50.0f;  // Start wide

            printf("[TICK] Epoch from ticks: %.1fms (5 good intervals)\n", td->gating.epoch_ms);
        }
    } else {
        td->gating.consecutive_good_ticks = 0;  // Reset on bad interval
    }
}

// Mode B: Marker-derived epoch (works on marginal signals)
// Called by sync_detector when marker is confirmed:
void tick_detector_set_epoch(tick_detector_t *td, float epoch_ms, float confidence) {
    // Marker epoch always takes priority (more reliable on marginal signals)
    // OR use it if we don't have an epoch yet
    if (td->gating.source != EPOCH_SOURCE_MARKER || confidence > td->gating.confidence) {
        td->gating.epoch_ms = epoch_ms;
        td->gating.confidence = confidence;
        td->gating.source = EPOCH_SOURCE_MARKER;
        td->gating.enabled = true;
        td->gating.window_ms = 40.0f;  // Tighter window - markers are accurate

        printf("[TICK] Epoch from marker: %.1fms (conf=%.2f)\n", epoch_ms, confidence);
    }
}
```

**Initialization:**
```c
// In tick_detector_create():
td->gating.enabled = false;
td->gating.epoch_ms = 0.0f;
td->gating.window_ms = 50.0f;      // Start wide, narrow over time
td->gating.confidence = 0.0f;
td->gating.source = EPOCH_SOURCE_NONE;
td->gating.consecutive_good_ticks = 0;
td->gating.consecutive_gated_ticks = 0;
```

**Gate Check Function:**
```c
static bool is_tick_gate_open(tick_detector_t *td, float current_ms) {
    if (!td->gating.enabled) {
        return true;  // No gating - accept all detections
    }

    // Calculate position within current second
    float ms_since_epoch = current_ms - td->gating.epoch_ms;
    float ms_into_second = fmodf(ms_since_epoch, 1000.0f);
    if (ms_into_second < 0) ms_into_second += 1000.0f;

    // Tick expected at 10-15ms into second (center at 12.5ms)
    float tick_center_ms = 12.5f;
    float distance_from_tick = fabsf(ms_into_second - tick_center_ms);

    // Also check wrapped distance (for ticks near second boundary)
    float wrapped_distance = fabsf(ms_into_second - 1000.0f - tick_center_ms);
    if (wrapped_distance < distance_from_tick) {
        distance_from_tick = wrapped_distance;
    }

    return (distance_from_tick <= td->gating.window_ms);
}
```

**Integration with State Machine:**
```c
// In run_state_machine(), modify STATE_IDLE transition:
case STATE_IDLE:
    if (energy > td->threshold_high) {
        // NEW: Check if gate is open before starting detection
        float current_ms = td->frame_count * FRAME_DURATION_MS;
        if (!is_tick_gate_open(td, current_ms)) {
            // Gate closed - ignore this energy spike (likely BCD harmonic)
            break;
        }

        td->state = STATE_IN_TICK;
        td->tick_start_frame = frame;
        // ... rest of existing code
    }
    break;
```

**Risk:** Medium - modifies detection state machine
**Testing:**
- Mode A: Verify 5 consecutive ticks enables gating on strong signal
- Mode B: Verify marker epoch enables gating on marginal signal
- Verify gating blocks detections outside window
- Verify 497ms false detections eliminated when gating active
- Verify no regression when gating disabled (startup behavior)

---

### 2.3 Tick-Based Epoch Refinement (THIRD)

**Depends on:** Phase 2.2 (needs gated ticks flowing)

**Concept:** Once gated ticks are detected reliably, use them to refine epoch estimate and narrow the gate window.

**File:** `tick_detector.c`

**Refinement Function:**
```c
static void refine_epoch_from_tick(tick_detector_t *td, float tick_ms) {
    if (!td->gating.enabled) {
        return;  // Only refine when gating is active
    }

    // Calculate where this tick falls relative to expected position
    float expected_tick_ms = td->gating.epoch_ms + 12.5f;  // Tick at 12.5ms into second
    float error = fmodf(tick_ms - expected_tick_ms + 500.0f, 1000.0f) - 500.0f;

    // Simple PLL: adjust epoch by 10% of error
    td->gating.epoch_ms += 0.1f * error;

    // Keep epoch in valid range
    td->gating.epoch_ms = fmodf(td->gating.epoch_ms, 1000.0f);
    if (td->gating.epoch_ms < 0) td->gating.epoch_ms += 1000.0f;

    // Track consecutive gated ticks
    td->gating.consecutive_gated_ticks++;

    // Narrow window as confidence builds
    if (td->gating.consecutive_gated_ticks > 10 && td->gating.window_ms > 25.0f) {
        td->gating.window_ms *= 0.95f;  // Slowly narrow toward 25ms
    }
    if (td->gating.consecutive_gated_ticks > 30 && td->gating.window_ms > 15.0f) {
        td->gating.window_ms *= 0.98f;  // Very slowly narrow toward 15ms
    }
}
```

**Call Site:** After successful tick detection in state machine:
```c
// After incrementing ticks_detected and before callback:
if (td->gating.enabled) {
    refine_epoch_from_tick(td, timestamp_ms);
}
```

**Epoch Loss Recovery:**
```c
// If interval is bad (not 950-1050ms), reset gating confidence:
if (interval_ms < 950.0f || interval_ms > 1050.0f) {
    td->gating.consecutive_gated_ticks = 0;
    td->gating.window_ms = 50.0f;  // Widen window again
}
```

**Risk:** Low - only affects epoch tracking, not core detection
**Testing:**
- Verify epoch converges to correct value over 30+ seconds
- Verify window narrows from 50ms toward 15ms
- Verify recovery after signal dropout (window widens)

---

### Phase 2 Acceptance Criteria

- [ ] Sync detector successfully feeds epoch to tick detector on marker confirmation
- [ ] Tick gating activates within 3 minutes using marker-derived epoch (marginal signal)
- [ ] Tick gating activates within 10 seconds using tick-derived epoch (strong signal)
- [ ] No epoch cold-start delays on marginal signals (marker bootstrap works)
- [ ] 497ms false detections eliminated when gating is active
- [ ] Epoch refines to stable value (±2ms jitter) after 30 seconds
- [ ] Gate window narrows from 50ms to <20ms after 60 seconds of good signal
- [ ] System recovers if gating epoch is lost (auto-widens window)

### Phase 2 Exit Criteria (Phase 3 Go/No-Go Decision)

After Phase 2 is complete, evaluate whether Phase 3 is necessary:

| Condition | Tick Detection Rate | 497ms Rejections | Decision |
|-----------|---------------------|------------------|----------|
| **Success** | ≥90% on marginal signal | Eliminated | **DEFER Phase 3** to backlog |
| **Partial** | 70-90% on marginal signal | Reduced but present | **PROCEED** to Phase 3 |
| **Insufficient** | <70% on marginal signal | Still frequent | **PROCEED** to Phase 3 (required) |

**Rationale:** Phase 3's parallel filter banks provide complete signal separation but require significant implementation effort (5-7 days). If Phase 1-2 achieves minimum viable performance (≥90% tick detection), Phase 3 can be deferred to a future hardening sprint.

**Measurement:**
```bash
# Record 5 minutes on marginal signal (25 MHz)
./waterfall.exe -t 192.168.1.153:4536 > phase2_test.log 2>&1 &
sleep 300
kill %1

# Count successful ticks vs total seconds
grep -c "TICK #" phase2_test.log        # Detected ticks
# Target: ~270+ for 90% (300 seconds × 0.9)

# Verify 497ms rejections eliminated
grep -c "dur=49[0-9]ms" phase2_test.log
# Target: 0 (or near-zero)
```

---

## Phase 3: Parallel Filter Architecture (5-7 days) - CONDITIONAL

> **Note:** Phase 3 is conditional based on Phase 2 exit criteria. If tick detection ≥90% and 497ms rejections eliminated, defer to backlog. Proceed only if residual BCD crosstalk prevents minimum viable performance.

### 3.1 Split Detector Path into Sync and Data Channels

**Concept:** Process 1000 Hz ticks/markers and 100 Hz BCD through completely separate filter chains immediately after I/Q decimation.

**File:** `waterfall.c`

**Current Architecture:**
```
I/Q (2MHz) → Lowpass (5kHz) → Decimate (40:1) → 50kHz stream
                                                    │
                                                    ├─→ tick_detector (1000 Hz)
                                                    ├─→ marker_detector (1000 Hz)
                                                    ├─→ slow_marker_detector (1000 Hz)
                                                    ├─→ bcd_time_detector (100 Hz)
                                                    └─→ bcd_freq_detector (100 Hz)
```

**Problem:** All detectors share the same signal path. BCD harmonic energy at 1000 Hz corrupts tick/marker detection.

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
                             ├──► tick_detector (1000 Hz ticks)          │
                             ├──► marker_detector (1000 Hz markers)      │
                             └──► slow_marker_detector (visual)          │
                                                                         │
                                                               ┌─────────┴─────────┐
                                                               │                   │
                                                               ▼                   ▼
                                                     bcd_time_detector    bcd_freq_detector
                                                         (100 Hz)            (100 Hz)
```

**Critical Clarification: Marker Routing**

Minute markers are 800ms pulses of **1000 Hz** (or 1500 Hz for hour markers). They MUST stay on the sync channel along with tick detection:

| Detector | Frequency | Channel | Rationale |
|----------|-----------|---------|-----------|
| tick_detector | 1000 Hz | Sync (800-1400 Hz BPF) | Primary tick detection |
| marker_detector | 1000/1500 Hz | Sync (800-1400 Hz BPF) | Minute/hour markers |
| slow_marker_detector | 1000 Hz | Sync (800-1400 Hz BPF) | Visual display |
| bcd_time_detector | 100 Hz | Data (150 Hz LPF) | BCD envelope timing |
| bcd_freq_detector | 100 Hz | Data (150 Hz LPF) | BCD spectral detection |

**Filter Specifications:**

| Filter | Type | Order | Passband | Stopband Atten | Purpose |
|--------|------|-------|----------|----------------|---------|
| Sync BPF | Butterworth | 4 | 800-1400 Hz | -40 dB @ 500 Hz | Pass 1000/1200/1500 Hz |
| Data LPF | Butterworth | 4 | 0-150 Hz | -40 dB @ 300 Hz | Pass 100 Hz only |

**Implementation:**

```c
// New filter structures
typedef struct {
    // 4th order = 2 cascaded biquad sections
    float s1_x1, s1_x2, s1_y1, s1_y2;  // Section 1 state
    float s2_x1, s2_x2, s2_y1, s2_y2;  // Section 2 state
} biquad_cascade_t;

typedef struct {
    biquad_cascade_t highpass;  // 800 Hz highpass
    biquad_cascade_t lowpass;   // 1400 Hz lowpass
} sync_channel_filter_t;

typedef struct {
    biquad_cascade_t lowpass;   // 150 Hz lowpass
} data_channel_filter_t;

// Global filter instances (I and Q paths)
static sync_channel_filter_t g_sync_filter_i, g_sync_filter_q;
static data_channel_filter_t g_data_filter_i, g_data_filter_q;

// Coefficients for fs=50000 Hz
// Sync channel: 800 Hz highpass (4th order Butterworth, 2 sections)
static const float sync_hp_b[2][3] = {
    {0.9025f, -1.8050f, 0.9025f},  // Section 1: b0, b1, b2
    {0.9025f, -1.8050f, 0.9025f}   // Section 2: b0, b1, b2
};
static const float sync_hp_a[2][2] = {
    {-1.8010f, 0.8089f},           // Section 1: a1, a2
    {-1.8010f, 0.8089f}            // Section 2: a1, a2
};

// Sync channel: 1400 Hz lowpass (4th order Butterworth, 2 sections)
static const float sync_lp_b[2][3] = {
    {0.00766f, 0.01532f, 0.00766f},
    {0.00766f, 0.01532f, 0.00766f}
};
static const float sync_lp_a[2][2] = {
    {-1.7349f, 0.7660f},
    {-1.7349f, 0.7660f}
};

// Data channel: 150 Hz lowpass (4th order Butterworth, 2 sections)
static const float data_lp_b[2][3] = {
    {0.0000352f, 0.0000704f, 0.0000352f},
    {0.0000352f, 0.0000704f, 0.0000352f}
};
static const float data_lp_a[2][2] = {
    {-1.9822f, 0.9823f},
    {-1.9822f, 0.9823f}
};
```

**Processing Loop:**
```c
// In detector path processing (after decimation):
void process_detector_sample(float det_i, float det_q) {
    // Sync channel: bandpass 800-1400 Hz
    float sync_i = sync_channel_process(&g_sync_filter_i, det_i);
    float sync_q = sync_channel_process(&g_sync_filter_q, det_q);

    // Data channel: lowpass 150 Hz
    float data_i = data_channel_process(&g_data_filter_i, det_i);
    float data_q = data_channel_process(&g_data_filter_q, det_q);

    // Feed sync channel to tick/marker detectors
    tick_detector_process_sample(g_tick_detector, sync_i, sync_q);
    marker_detector_process_sample(g_marker_detector, sync_i, sync_q);
    slow_marker_detector_process_sample(g_slow_marker_detector, sync_i, sync_q);

    // Feed data channel to BCD detectors
    bcd_time_detector_process_sample(g_bcd_time_detector, data_i, data_q);
    bcd_freq_detector_process_sample(g_bcd_freq_detector, data_i, data_q);
}
```

**Risk:** Medium - significant refactor but well-understood DSP
**Testing:**
- Verify tick detection works through sync channel (no regression)
- Verify marker detection works through sync channel
- Verify BCD detection works through data channel
- Verify NO cross-contamination: BCD harmonics absent from sync channel
- Measure filter group delay and verify timing alignment

---

### 3.2 Add Filter State Reset on Reconnect

**File:** `waterfall.c`

When TCP connection drops and reconnects, filter states may contain stale data:

```c
void reset_channel_filters(void) {
    memset(&g_sync_filter_i, 0, sizeof(g_sync_filter_i));
    memset(&g_sync_filter_q, 0, sizeof(g_sync_filter_q));
    memset(&g_data_filter_i, 0, sizeof(g_data_filter_i));
    memset(&g_data_filter_q, 0, sizeof(g_data_filter_q));
    printf("[FILTER] Channel filters reset\n");
}

// Call in tcp_reconnect():
reset_channel_filters();
```

---

### Phase 3 Acceptance Criteria

- [ ] Sync channel passes 1000 Hz with <1 dB attenuation
- [ ] Sync channel attenuates 100 Hz by >40 dB
- [ ] Data channel passes 100 Hz with <1 dB attenuation
- [ ] Data channel attenuates 1000 Hz by >40 dB
- [ ] Tick detection works correctly through sync channel
- [ ] Marker detection works correctly through sync channel
- [ ] BCD detection works correctly through data channel
- [ ] No 497ms false detections (BCD harmonics filtered out)
- [ ] Filter states reset properly on reconnect

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

**Platform Decision:** Phoenix SDR targets desktop Windows with ample RAM. Use full-rate implementation for simplicity.

| Platform | Sample Rate | Delay Line | Memory | Recommendation |
|----------|-------------|------------|--------|----------------|
| **Desktop Windows** | **50 kHz** | **50,000 samples** | **200 KB** | **Use this** |
| Embedded ARM | 1 kHz (decimated) | 1,000 samples | 4 KB | Future target |

**File:** New `tick_comb_filter.c`

```c
// Desktop version - full rate, no decimation overhead
#define COMB_STAGES 50000  // 1 second at 50 kHz = 200 KB

typedef struct {
    float delay_line[COMB_STAGES];
    int write_idx;
    float alpha;           // Averaging coefficient (0.99 = ~100s time constant)
    float output;
} comb_filter_t;

void comb_filter_init(comb_filter_t *cf) {
    memset(cf->delay_line, 0, sizeof(cf->delay_line));
    cf->write_idx = 0;
    cf->alpha = 0.99f;
    cf->output = 0.0f;
}

// Call at 50 kHz with matched filter output
float comb_filter_process(comb_filter_t *cf, float input) {
    float delayed = cf->delay_line[cf->write_idx];
    cf->output = cf->alpha * cf->output + (1.0f - cf->alpha) * (input + delayed) / 2.0f;
    cf->delay_line[cf->write_idx] = input;
    cf->write_idx = (cf->write_idx + 1) % COMB_STAGES;
    return cf->output;
}

void comb_filter_reset(comb_filter_t *cf) {
    memset(cf->delay_line, 0, sizeof(cf->delay_line));
    cf->write_idx = 0;
    cf->output = 0.0f;
}
```

**Usage:** Feed matched filter output through comb filter. Periodic tick signal adds coherently; non-periodic interference averages to zero.

**Processing Gain:** At α=0.99, approximately 20 dB improvement in SNR for periodic signals.

**Future Embedded Support:** If Phoenix SDR targets memory-constrained platforms (STM32, ESP32), add decimated version behind `#ifdef EMBEDDED`:
```c
#ifdef EMBEDDED
#define COMB_STAGES 1000       // 1 second at 1 kHz = 4 KB
#define DECIMATE_RATIO 50      // 50 kHz → 1 kHz
// ... decimation logic ...
#endif
```

**Risk:** Low - additional processing, doesn't change existing detection
**Testing:** Verify epoch detection on marginal signals that currently fail

---

## Implementation Priority Matrix

| Phase | Change | Impact | Effort | Risk | Priority |
|-------|--------|--------|--------|------|----------|
| 1.1 | Tighten pulse width (15ms) | Low | 1 hr | Low | P1 |
| 1.2 | Add 500Hz highpass (2nd order) | Medium | 2 hr | Low | P1 |
| 1.3 | Validation test (mandatory gate) | Critical | 2 hr | None | P1 |
| 1.4 | Signal normalization (slow AGC) | Medium | 4 hr | Low | P1 |
| 2.1 | Marker→Tick epoch feed | High | 4 hr | Low | P1 |
| 2.2 | Dual-mode tick gating | High | 2 days | Medium | P1 |
| 2.3 | Tick PLL epoch refinement | Medium | 4 hr | Low | P2 |
| 3.1 | Parallel filter banks | High | 5 days | Medium | P2 |
| 3.2 | Filter reset on reconnect | Low | 1 hr | Low | P2 |
| 4.1 | Optimize matched filter window | Low | 4 hr | Low | P3 |
| 4.2 | Add comb filter (full-rate, 200KB) | Medium | 1 day | Low | P3 |

**Recommended Implementation Order:**

```
Week 1: Quick Wins + Validation + Marker Bootstrap
├── Day 1: Phase 1.1 + 1.2 (filter implementation)
├── Day 2: Phase 1.3 (validation test - MUST PASS before continuing)
│          └─► If <50% improvement: try 4th-order highpass
├── Day 3: Phase 1.4 (signal normalization)
├── Day 4: Phase 2.1 (marker→tick epoch feed)
└── Day 5: Phase 2.2 (dual-mode tick gating) + testing

Week 2: Refinement + Architecture
├── Day 1: Phase 2.3 (tick PLL refinement)
├── Day 2-4: Phase 3.1 (parallel filter banks)
└── Day 5: Phase 3.2 + integration testing

Week 3: Optimization (if needed)
├── Phase 4.1 (matched filter optimization)
└── Phase 4.2 (comb filter - 200KB, desktop target)
```

**Critical Path:**
```
Phase 1.1-1.2 ──► Phase 1.3 (GATE) ──► Phase 1.4 ──► Phase 2.1 ──► Phase 2.2
                      │
                      ▼
              STOP if <50% improvement
              Investigate before proceeding
```

**Go/No-Go Decision Points:**

1. **After Phase 1.3:** If 497ms rejection rate doesn't improve >50%, STOP. Try 4th-order filter or investigate alternative root causes.

2. **After Phase 2.2:** If gating doesn't eliminate remaining 497ms detections, proceed to Phase 3 (parallel filter banks) for complete signal separation.

3. **After Phase 3:** If still not achieving >95% tick detection, add Phase 4 optimizations.

---

## Acceptance Criteria

### Minimum Viable (Phase 1-2 Complete)
- [ ] No 497ms false tick detections on 25 MHz marginal signal
- [ ] Tick detection rate >90% during BCD_ONE symbols (when gating active)
- [ ] Sync reaches LOCKED within 3 minutes on marginal signal
- [ ] Marker-derived epoch successfully bootstraps tick gating
- [ ] Tick gating activates within 3 minutes using marker-derived epoch (marginal)
- [ ] Tick gating activates within 10 seconds using tick-derived epoch (strong)
- [ ] No epoch cold-start delays on marginal signals
- [ ] Consistent tick detection from GR=40 to GR=60
- [ ] No regression on strong signals (5, 10, 15 MHz)

### Full Implementation (All Phases Complete)
- [ ] Tick detection rate >95% on all signals
- [ ] Sub-10ms epoch accuracy after 30 seconds
- [ ] BCD decode rate >98% when sync LOCKED
- [ ] Consistent performance across GR=40 to GR=60
- [ ] Recovery from signal loss within 2 marker intervals
- [ ] Clean channel separation: <-40dB crosstalk between sync/data paths

---

## Testing Plan

### Unit Tests

**Phase 1:**
- Filter frequency response verification (measure actual -3dB points)
- Pulse width rejection at boundary cases (14ms, 15ms, 16ms)

**Phase 2:**
- Epoch calculation from marker timestamp (verify math)
- Gate window timing accuracy (scope or simulation)
- PLL convergence rate and steady-state error

**Phase 3:**
- Sync channel frequency response: 800-1400 Hz passband
- Data channel frequency response: 0-150 Hz passband
- Stopband attenuation at 100 Hz (sync) and 1000 Hz (data)

### Integration Tests

**Signal Sources:**
- Live signal: 5 MHz (strong), 10 MHz (medium), 15 MHz (variable), 25 MHz (weak)
- Signal generator: Controlled BCD patterns (all ZERO, all ONE, mixed)
- Recorded files: Known-good captures for regression testing

**Test Scenarios:**

| Test | Signal | Duration | Success Criteria |
|------|--------|----------|------------------|
| Strong signal baseline | 10 MHz | 5 min | >98% tick rate, LOCKED in <2 min |
| Marginal signal | 25 MHz | 10 min | >90% tick rate, LOCKED in <4 min |
| BCD stress test | Generator | 2 min | No 497ms detections during BCD_ONE |
| Cold start | 25 MHz | 5 min | Gating activates after first marker |
| Signal dropout | 10 MHz | 10 min | Recovery within 90s of signal return |
| Gain variation | 10 MHz | 5 min | Consistent detection GR=40 to GR=60 |

### Regression Tests

- Existing tick detection on known-good recordings
- Marker detection timing accuracy (±10ms of expected)
- Sync state machine transitions (ACQUIRING→TENTATIVE→LOCKED)
- BCD decode accuracy on recorded signal with known time

### Overnight Soak Test

- 8+ hours continuous operation on 10 MHz
- Log all state transitions, epoch drift, detection rates
- Success: No crashes, no state machine lockups, epoch drift <50ms

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Phase 2 gating too aggressive | Start with wide window (±50ms), narrow slowly |
| Marker epoch calculation wrong | Add debug logging, verify against known signal |
| Filter group delay causes timing error | Measure delay, compensate in epoch calculation |
| Phase 3 breaks existing detection | Implement behind feature flag, A/B test |
| Performance impact from added filters | Profile before/after, optimize if needed |

---

## References

1. **NTP Driver36 Documentation** - David Mills, University of Delaware
   - https://www.eecis.udel.edu/~mills/ntp/html/drivers/driver36.html
   - Parallel filter architecture, matched filtering, comb filter design

2. **WWV/H Audio Demodulator/Decoder Technical Report** - David Mills
   - https://www.eecis.udel.edu/~mills/database/reports/wwv/wwvb.pdf
   - Filter specifications, epoch tracking algorithms

3. **NIST Special Publication 432** - WWV/WWVH Broadcast Format
   - Official signal timing specifications
   - 40ms protected zone documentation

4. **DCF77 Receiver Design** - Radioengineering Journal
   - https://www.radioeng.cz/fulltexts/2013/13_04_1211_1217.pdf
   - Similar time signal receiver architecture

5. **WWVB Cross-Correlation Decoder** - jremington (GitHub)
   - https://github.com/jremington/WWVB_decoder
   - Template matching techniques for time signal decoding

6. **Phoenix SDR Architecture Documentation**
   - /mnt/transcripts/2025-12-18-* (session transcripts)
   - Current implementation details and design decisions

---

## Appendix A: Filter Coefficient Calculator

```python
#!/usr/bin/env python3
"""
Generate filter coefficients for WWV tick/BCD separation.
Run: python3 filter_coeffs.py
"""
from scipy.signal import butter, ellip, sosfreqz, bilinear, tf2sos
import numpy as np

fs = 50000  # 50 kHz detector path sample rate

print("=" * 60)
print("WWV Tick/BCD Separation Filter Coefficients")
print(f"Sample Rate: {fs} Hz")
print("=" * 60)

# Phase 1.2: Narrow bandpass for tick detector (950-1050 Hz)
print("\n--- Phase 1.2: Tick Bandpass (950-1050 Hz) ---")
sos_tick_bp = butter(2, [950, 1050], btype='band', fs=fs, output='sos')
print("2nd order Butterworth bandpass:")
for i, section in enumerate(sos_tick_bp):
    print(f"  Section {i+1}: b=[{section[0]:.6f}, {section[1]:.6f}, {section[2]:.6f}]")
    print(f"             a=[1.0, {section[4]:.6f}, {section[5]:.6f}]")

# Phase 3: Sync channel bandpass (800-1400 Hz)
print("\n--- Phase 3: Sync Channel BPF (800-1400 Hz) ---")
sos_sync = butter(4, [800, 1400], btype='band', fs=fs, output='sos')
print("4th order Butterworth bandpass (cascade of 2 biquads):")
for i, section in enumerate(sos_sync):
    print(f"  Section {i+1}: b=[{section[0]:.6f}, {section[1]:.6f}, {section[2]:.6f}]")
    print(f"             a=[1.0, {section[4]:.6f}, {section[5]:.6f}]")

# Phase 3: Data channel lowpass (150 Hz)
print("\n--- Phase 3: Data Channel LPF (150 Hz) ---")
sos_data = butter(4, 150, btype='low', fs=fs, output='sos')
print("4th order Butterworth lowpass (cascade of 2 biquads):")
for i, section in enumerate(sos_data):
    print(f"  Section {i+1}: b=[{section[0]:.8f}, {section[1]:.8f}, {section[2]:.8f}]")
    print(f"             a=[1.0, {section[4]:.6f}, {section[5]:.6f}]")

# Verify frequency response
print("\n--- Frequency Response Check ---")
import warnings
warnings.filterwarnings('ignore')

w, h_sync = sosfreqz(sos_sync, worN=10000, fs=fs)
w, h_data = sosfreqz(sos_data, worN=10000, fs=fs)

# Find -3dB points
def find_3db_points(w, h):
    h_db = 20 * np.log10(np.abs(h) + 1e-10)
    max_db = np.max(h_db)
    idx_3db = np.where(h_db >= max_db - 3)[0]
    if len(idx_3db) > 0:
        return w[idx_3db[0]], w[idx_3db[-1]]
    return None, None

sync_low, sync_high = find_3db_points(w, h_sync)
print(f"Sync BPF -3dB points: {sync_low:.0f} Hz to {sync_high:.0f} Hz")

data_low, data_high = find_3db_points(w, h_data)
print(f"Data LPF -3dB point: {data_high:.0f} Hz")

# Attenuation at key frequencies
idx_100 = np.argmin(np.abs(w - 100))
idx_1000 = np.argmin(np.abs(w - 1000))
print(f"Sync BPF @ 100 Hz: {20*np.log10(np.abs(h_sync[idx_100])+1e-10):.1f} dB")
print(f"Sync BPF @ 1000 Hz: {20*np.log10(np.abs(h_sync[idx_1000])+1e-10):.1f} dB")
print(f"Data LPF @ 100 Hz: {20*np.log10(np.abs(h_data[idx_100])+1e-10):.1f} dB")
print(f"Data LPF @ 1000 Hz: {20*np.log10(np.abs(h_data[idx_1000])+1e-10):.1f} dB")
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
| :29 | Position marker | **NO TICK** | P3 (800ms) |
| :30-:38 | Days units, reserved | 5ms | Data |
| :39 | Position marker | 5ms | P4 (800ms) |
| :40-:48 | Year tens/units | 5ms | Data |
| :49 | Position marker | 5ms | P5 (800ms) |
| :50-:58 | UT1 correction, DUT1 | 5ms | Data |
| :59 | Position marker | **NO TICK** | P0 (800ms) |

**Note:** Seconds 29 and 59 have NO tick pulse per NIST spec.

---

## Appendix C: Related Patches (Already Identified)

### C.1 Marker-Based Signal Health (Sync Detector)

**Status:** Patch ready, pending implementation
**File:** `sync_detector.c`

**Problem:** Signal health check uses tick gaps to trigger RECOVERING state. Ticks are unreliable at high gain or during BCD interference, causing immediate unlock after valid marker-based lock.

**Solution:** Change signal health authority from ticks to markers.

```c
/* Old - tick-based (problematic) */
#define TICK_GAP_WARNING_MS      2500.0f
#define TICK_GAP_CRITICAL_MS     5000.0f

/* New - marker-based */
#define MARKER_GAP_WARNING_MS    75000.0f  /* 1.25 marker intervals */
#define MARKER_GAP_CRITICAL_MS   90000.0f  /* 1.5 marker intervals */

/* In sync_detector_periodic_check(): */
float ms_since_marker = current_ms - sd->last_confirmed_marker_ms;
if (ms_since_marker > MARKER_GAP_CRITICAL_MS && sd->state == SYNC_LOCKED) {
    /* Transition to RECOVERING */
}
```

### C.2 Confidence Decay Rate (Sync Detector)

**Status:** Patch ready, pending implementation
**File:** `sync_detector.c`

**Problem:** Confidence decay too aggressive for 60-second marker interval. Decays 95% between markers, preventing LOCKED state.

**Solution:** Reduce decay rate:

```c
/* Old - decays 95% in 60 seconds */
#define CONFIDENCE_DECAY_NORMAL      0.995f

/* New - decays 6% in 60 seconds */
#define CONFIDENCE_DECAY_NORMAL      0.9999f
```

---

## Appendix D: Diagnostic Logging Additions

Add these log messages to aid debugging:

```c
// In tick_detector.c - when gating activates:
printf("[TICK] Gating enabled: epoch=%.1fms, window=±%.1fms\n",
       td->gating.epoch_ms, td->gating.window_ms);

// In tick_detector.c - when gate blocks a detection:
printf("[TICK] Gate blocked: energy=%.1f at %.1fms (gate center=%.1fms)\n",
       energy, current_ms, expected_tick_ms);

// In tick_detector.c - epoch refinement:
printf("[TICK] Epoch refined: error=%.2fms, new_epoch=%.1fms, window=%.1fms\n",
       error, td->gating.epoch_ms, td->gating.window_ms);

// In sync_detector.c - epoch feed:
printf("[SYNC] Fed epoch to tick detector: marker@%.1fms → epoch=%.1fms\n",
       marker_start_ms, epoch_ms);
```

---

## Revision History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2025-12-18 | Initial draft |
| 1.1 | 2025-12-18 | Reordered Phase 2 (marker bootstrap first), clarified Phase 3 marker routing, added acceptance criteria for Phase 1, changed filter from highpass to narrow bandpass |
| 2.0 | 2025-12-18 | Reverted to 500Hz highpass (simpler, A/B testable), added mandatory Phase 1.3 validation gate, moved signal normalization to Phase 1.4, added dual-mode epoch acquisition (ticks OR markers), updated comb filter to use decimation (4KB vs 200KB memory), added Go/No-Go decision points |
| 2.1 | 2025-12-18 | Added TICK_HIGHPASS_ORDER compile switch for A/B testing 2nd vs 4th order filter, added explicit Phase 2 exit criteria for Phase 3 deferral decision (≥90% = defer, <90% = proceed), reverted comb filter to full-rate 50kHz for desktop target (200KB, simpler implementation) |