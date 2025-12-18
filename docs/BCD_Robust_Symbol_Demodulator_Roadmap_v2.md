# BCD Robust Detection - Implementation Roadmap v2

> ✅ **CURRENT** - Active roadmap for BCD detection improvements
>
> Phases 1-5 complete. Phase 6 identified P-marker false positive issue.
> Phases 8-12 are new noise rejection phases based on NTP WWV driver techniques.
> See also: [wwv_bcd_noise_rejection.md](wwv_bcd_noise_rejection.md) for detailed techniques.

## Document Info
- **Date:** 2024-12-18 (Updated)
- **Project:** Phoenix SDR WWV Decoder
- **Purpose:** Add redundant time-domain and frequency-domain FFT paths for BCD symbol detection
- **Revision:** v2 - Added P-marker false positive analysis and advanced noise rejection phases

---

## Implementation Status

| Phase | Module | Status | Notes |
|-------|--------|--------|-------|
| 1 | `bcd_time_detector.c/h` | ✅ Done | 256-pt FFT, hysteresis, state machine |
| 2 | `bcd_freq_detector.c/h` | ✅ Done | 2048-pt FFT, sliding window accumulator |
| 3 | `bcd_correlator.c/h` | ✅ Done | Window-based integration, sync-gated |
| 4 | Wire into `waterfall.c` | ✅ Done | Parallel IQ feed, P2 isolation |
| 5 | Update `build.ps1` | ✅ Done | Added build objects + linker entries |
| 6 | Test & tune thresholds | 🔶 Partial | **Problem identified - see analysis below** |
| 7 | Deprecate old `bcd_decoder.c` | ⬜ Blocked | Blocked on Phase 6 validation |
| **8** | **Position-based P-marker gating** | ⬜ New | Quick win - reject P at wrong positions |
| **9** | **Minimum duration validation** | ⬜ New | Require sustained signal state |
| **10** | **Multi-point sampling** | ⬜ New | NTP-style within-second sampling |
| **11** | **Matched filter correlation** | ⬜ New | Correlate against expected pulse shape |
| **12** | **Comb filter averaging** | ⬜ New | Frame-to-frame averaging |

---

## Problem Analysis: P-Marker False Positives

### Observed Behavior (from 3.4-hour capture)

Debug log shows P-markers falsely detected at positions:
```
FALSE: 4, 21, 33, 35, 42, 43, 44, 45, 46, 47
VALID: 0, 9, 19, 29, 39, 49, 59 (expected)
```

### Root Cause Analysis

**Fundamental Architecture Issue:**

The current detectors are optimized to detect when 100Hz subcarrier is **PRESENT**:
```
bcd_time_detector:  Triggers when energy > threshold_high
bcd_freq_detector:  Triggers when accumulated_energy > threshold
```

But WWV BCD encoding works by **ABSENCE** of the 100Hz subcarrier:
```
100Hz subcarrier is NORMALLY ON during each second
Symbols encoded by how long subcarrier is OFF ("hole punch"):
  - Binary 0:  ~200ms absence (short hole)
  - Binary 1:  ~500ms absence (medium hole)  
  - P marker:  ~800ms absence (long hole)
```

**The detectors are essentially inverted!**

### Why False Positives Occur

1. **Weak signal conditions** → Noise floor rises
2. **Brief noise spikes** → Trigger "signal present" detector
3. **Random timing** → Pulses detected at arbitrary positions
4. **Duration mismatch** → Short spurious pulses get misclassified

### What NTP WWV Driver Does Differently

From Dave Mills' NTP driver (driver36.c) - proven reliable for 25+ years:

| Technique | NTP Implementation | Phoenix Current |
|-----------|-------------------|-----------------|
| **Detection target** | Detects signal ABSENCE | Detects signal PRESENCE |
| **Multi-point sampling** | Samples at 15ms, 200ms, 500ms, end | Single threshold crossing |
| **Pulse width discrimination** | Bipolar: 2×s1 - s0 - n | Simple duration measurement |
| **Matched filter** | 170ms synchronous matched filter | None |
| **Comb filter** | 8000-stage averaging across frames | None |
| **Noise cancellation** | Noise term cancels in bipolar formula | Adaptive threshold only |

---

## Technical Specifications (Existing - No Changes)

### Sample Rate
- **Input:** 50kHz (decimated from 2MHz)

### FFT Library
- **Library:** kiss_fft (following existing tick_detector/marker_detector pattern)

### Integration Point
- **File:** `tools/waterfall.c`
- **Pattern:** Parallel IQ feed to each detector
- **Isolation:** Per P2 rules - raw `i_raw`, `q_raw` only shared data

---

## Current Architecture (Phases 1-5 Complete)

```
                          ┌─────────────────────┐
                          │   SDR IQ Source     │
                          │      (50kHz)        │
                          └──────────┬──────────┘
                                     │
     ┌───────────────┬───────────────┼───────────────┬───────────────┐
     │               │               │               │               │
     ▼               ▼               ▼               ▼               ▼
tick_detector  marker_detector  bcd_time_det   bcd_freq_det    (existing)
  (1000Hz)       (1000Hz)         (100Hz)        (100Hz)
     │               │               │               │
     │               │          [Time FFT]     [Freq FFT]
     │               │          256-pt/5ms    2048-pt/41ms
     ▼               ▼               │               │
┌─────────────────────┐              └───────┬───────┘
│    sync_detector    │                      │
│  (minute boundary)  │◄─────────────────────┤ (sync gate)
└──────────┬──────────┘                      ▼
           │                         ┌───────────────┐
           │                         │bcd_correlator │
           │                         │ (1s windows)  │
           │                         └───────┬───────┘
           │                                 │
           │                                 ▼
           │                          [SYMBOLS OUT]
           │                          (P false +ves)  ◄── PROBLEM HERE
           └───────────────┬─────────────────┘
                           ▼
                  ┌─────────────────┐
                  │  Controller via │
                  │  UDP telemetry  │
                  └─────────────────┘
```

---

## NEW PHASES: Noise Rejection Improvements

### Phase 8: Position-Based P-Marker Gating (Quick Win)

**Effort:** Low  
**Impact:** High  
**Location:** `bcd_decoder.c` (controller side)

**Concept:** When frame sync is locked (we know which second we're in), only accept P-markers at valid positions.

**Implementation:**
```c
/* In bcd_decoder_process_symbol() */
static const int VALID_P_POSITIONS[] = {0, 9, 19, 29, 39, 49, 59};

if (symbol == BCD_SYMBOL_MARKER && sync_locked) {
    bool position_valid = false;
    for (int i = 0; i < 7; i++) {
        if (abs(frame_pos - VALID_P_POSITIONS[i]) <= 1) {  /* ±1 tolerance */
            position_valid = true;
            break;
        }
    }
    if (!position_valid) {
        LOG_WARN("[BCD] Rejected P-marker at position %d (not a valid P position)", frame_pos);
        return;  /* Don't store in frame buffer */
    }
}
```

**Why this works:** After minute sync is established, we know exactly which second we're in. P-markers can ONLY occur at positions 0,9,19,29,39,49,59. Any P-marker detected elsewhere is definitively wrong.

---

### Phase 9: Minimum Duration Validation (Quick Win)

**Effort:** Low  
**Impact:** Medium-High  
**Location:** `bcd_time_detector.c` and/or `bcd_freq_detector.c`

**Current problem:** Pulse ends as soon as energy drops below threshold_low - even a single-sample dip triggers end.

**Solution:** Require sustained low state before declaring pulse end.

**Implementation (bcd_time_detector.c):**
```c
#define MIN_LOW_FRAMES_FOR_END  3  /* ~15ms at 5.12ms/frame */

/* In run_state_machine(), STATE_IN_PULSE case: */
case STATE_IN_PULSE:
    td->pulse_duration_frames++;
    if (energy > td->pulse_peak_energy) {
        td->pulse_peak_energy = energy;
    }

    if (energy < td->threshold_low) {
        td->consecutive_low_frames++;
        if (td->consecutive_low_frames >= MIN_LOW_FRAMES_FOR_END) {
            /* SUSTAINED low - pulse truly ended */
            /* Adjust duration to subtract the low frames */
            td->pulse_duration_frames -= td->consecutive_low_frames;
            /* ... rest of pulse-end logic ... */
        }
    } else {
        td->consecutive_low_frames = 0;  /* Reset on any high sample */
    }
    break;
```

**Why this works:** Noise spikes are brief. Real signal dropouts are sustained. Requiring 3+ consecutive low frames (~15ms) filters out momentary glitches.

---

### Phase 10: Multi-Point Sampling (Medium Effort)

**Effort:** Medium  
**Impact:** High  
**Location:** New module or enhancement to `bcd_correlator.c`

**Concept:** Instead of continuous threshold detection, sample at strategic points within each second (NTP pattern).

**Sample points:**
| Time (ms) | Variable | Purpose |
|-----------|----------|---------|
| 15 | n | Noise floor reference (subcarrier normally OFF here) |
| 200 | s0 | Short pulse (0) would end by now |
| 500 | s1 | Medium pulse (1) would end by now |
| 800 | e1 | Long pulse (P) energy measurement |
| 980 | e0 | End-of-second envelope reference |

**Bipolar signal (NTP formula):**
```c
float bipolar = 2.0f * s1 - s0 - n;
/* Positive = data 1, Negative = data 0 */
/* Note: noise component n cancels out! */
```

**Classification:**
```c
if (e1 < noise_threshold) {
    symbol = NONE;  /* No signal this second */
} else if (s0 > threshold && s1 < threshold) {
    symbol = ZERO;  /* Dropped between 200-500ms */
} else if (s0 > threshold && s1 > threshold && e1 < threshold) {
    symbol = ONE;   /* Dropped between 500-800ms */
} else if (s0 > threshold && s1 > threshold && e1 > threshold) {
    symbol = MARKER; /* Still present at 800ms = P marker */
}
```

**Why this works:** 
1. Samples at known-good times rather than continuous edge detection
2. Noise cancellation built into bipolar formula
3. Less susceptible to brief glitches

---

### Phase 11: Matched Filter Correlation (Higher Effort)

**Effort:** High  
**Impact:** Very High  
**Location:** New module `bcd_matched_filter.c`

**Concept:** Correlate incoming envelope against template of expected pulse shapes.

**Templates:**
```c
/* Normalized envelope templates (100 samples = 1 second at 100Hz) */
static const float TEMPLATE_ZERO[100] = {
    /* 1.0 for first 20 samples, then 0.0 for 80 samples */
    1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,1,  /* 200ms ON */
    0,0,0,0,0, ... /* 800ms OFF */
};

static const float TEMPLATE_ONE[100] = {
    /* 1.0 for first 50 samples, then 0.0 for 50 samples */
    1,1,1,1,1, 1,1,1,1,1, ... /* 500ms ON */
    0,0,0,0,0, ... /* 500ms OFF */
};

static const float TEMPLATE_MARKER[100] = {
    /* 1.0 for first 80 samples, then 0.0 for 20 samples */
    1,1,1,1,1, 1,1,1,1,1, ... /* 800ms ON */
    0,0,0,0,0, ... /* 200ms OFF */
};
```

**Correlation:**
```c
float correlate(float *envelope, float *template, int len) {
    float sum = 0;
    for (int i = 0; i < len; i++) {
        sum += envelope[i] * template[i];
    }
    return sum / len;
}

/* Pick template with highest correlation */
float c0 = correlate(envelope, TEMPLATE_ZERO, 100);
float c1 = correlate(envelope, TEMPLATE_ONE, 100);
float cP = correlate(envelope, TEMPLATE_MARKER, 100);

if (c0 > c1 && c0 > cP && c0 > threshold) symbol = ZERO;
else if (c1 > c0 && c1 > cP && c1 > threshold) symbol = ONE;
else if (cP > c0 && cP > c1 && cP > threshold) symbol = MARKER;
else symbol = NONE;
```

**Why this works:** Noise that doesn't match the expected shape produces low correlation. Only signals matching the template produce high correlation.

---

### Phase 12: Comb Filter Averaging (Higher Effort)

**Effort:** High  
**Impact:** High (especially for weak signals)  
**Location:** New module or enhancement to existing detectors

**Concept:** Average corresponding samples across multiple 1-second frames to reinforce periodic signals and suppress random noise.

**Implementation:**
```c
#define COMB_LENGTH 8000  /* 8 seconds of history at 1kHz */
static float comb_filter[COMB_LENGTH];
static int comb_idx = 0;

void comb_filter_update(float sample) {
    /* Exponential average with existing sample at same phase */
    comb_filter[comb_idx] += 0.1f * (sample - comb_filter[comb_idx]);
    comb_idx = (comb_idx + 1) % COMB_LENGTH;
}

float comb_filter_get(int offset) {
    int idx = (comb_idx + offset) % COMB_LENGTH;
    return comb_filter[idx];
}
```

**Why this works:** Real WWV signals repeat with 1-second period. Averaging across multiple seconds reinforces the true signal while random noise averages toward zero.

---

## Recommended Implementation Order

### Quick Wins (Do First)
| Phase | Module | Effort | Impact | Dependencies |
|-------|--------|--------|--------|--------------|
| 8 | Position gating | Low | High | None |
| 9 | Min duration validation | Low | Medium-High | None |

### Medium Term
| Phase | Module | Effort | Impact | Dependencies |
|-------|--------|--------|--------|--------------|
| 10 | Multi-point sampling | Medium | High | Phases 8-9 validated |

### Longer Term (if needed)
| Phase | Module | Effort | Impact | Dependencies |
|-------|--------|--------|--------|--------------|
| 11 | Matched filter | High | Very High | Phase 10 |
| 12 | Comb filter | High | High | Phase 10 |

---

## Best Path Forward

### Immediate Actions (This Session)

1. **Phase 8: Position Gating** - Add to controller-side `bcd_decoder.c`
   - 15 minutes of work
   - Immediately eliminates false P-markers at wrong positions
   - Zero risk of breaking anything

2. **Phase 9: Min Duration Validation** - Add to `bcd_time_detector.c`
   - 30 minutes of work
   - Requires `consecutive_low_frames` counter
   - Reduces spurious pulse-end triggers

### After Field Validation

3. If false positives persist → Implement Phase 10 (multi-point sampling)
4. If weak signal reliability needed → Implement Phase 12 (comb filter)
5. If maximum robustness needed → Implement Phase 11 (matched filter)

---

## Files to Modify

### Phase 8
- `src/bdc/bcd_decoder.c` (controller) - Add position validation in `bcd_decoder_process_symbol()`

### Phase 9  
- `tools/bcd_time_detector.c` - Add `consecutive_low_frames` to struct and state machine
- `tools/bcd_freq_detector.c` - Same pattern

### Phase 10+
- New files or significant refactoring of correlator

---

## Verification Criteria

### Phase 8 Success
- Zero P-markers logged at positions other than 0,9,19,29,39,49,59
- Frame decode success rate improves

### Phase 9 Success
- Reduced total pulse count (spurious short pulses eliminated)
- Pulse duration histogram tightens around expected values (200/500/800ms)

### Overall Success
- Frame decode rate > 90%
- Correct time extraction in consecutive frames
- Stable operation across propagation changes (day/night transition)

---

## Summary: Current State vs Target

| Aspect | Current State | Target State |
|--------|---------------|--------------|
| P-marker accuracy | ~60% (false +ves at wrong positions) | >95% |
| Frame decode rate | Low (P-marker errors cascade) | >90% |
| Weak signal handling | Poor (noise triggers false pulses) | Graceful degradation |
| Architecture | Detects signal PRESENCE | Detects signal characteristics |

The dual-FFT architecture (Phases 1-5) provides a solid foundation. Phases 8-9 are targeted fixes for the specific P-marker issue observed. Phases 10-12 add NTP-grade robustness if needed.
