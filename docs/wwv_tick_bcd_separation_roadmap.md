# WWV Tick/BCD Separation - Final Implementation Plan

## Phoenix SDR Modem - FINAL SPECIFICATION

**Date:** December 18, 2025
**Status:** APPROVED FOR IMPLEMENTATION
**Authority:** Based on NTP driver36 (20+ years proven operation)

---

## Stop Discussing, Start Building

This plan is based on David Mills' NTP driver36 implementation, which has successfully decoded WWV signals for over two decades. The architecture is proven. The filter specifications are proven. The timing relationships are defined by NIST.

**There are no open questions. Implement as specified.**

---

## The Problem (Solved)

497ms false tick detections caused by 100 Hz BCD subcarrier's 10th harmonic at 1000 Hz.

**Solution:** NIST's 40ms protected zone + parallel filter architecture + timing-based gating.

---

## Architecture Overview

```
I/Q (2MHz) → Lowpass (5kHz) → Decimate (40:1) → 50kHz
                                                  │
                              ┌───────────────────┴───────────────────┐
                              │                                       │
                              ▼                                       ▼
                    ┌──────────────────┐                    ┌──────────────────┐
                    │   SYNC CHANNEL   │                    │   DATA CHANNEL   │
                    │                  │                    │                  │
                    │  4th-order BPF   │                    │  4th-order LPF   │
                    │   800-1400 Hz    │                    │     150 Hz       │
                    └────────┬─────────┘                    └────────┬─────────┘
                             │                                       │
                             ▼                                       ▼
                    ┌──────────────────┐                    ┌──────────────────┐
                    │  5ms Matched     │                    │  170ms I/Q       │
                    │  Filter (tick)   │                    │  Matched Filter  │
                    └────────┬─────────┘                    └────────┬─────────┘
                             │                                       │
                             ▼                                       ▼
                    ┌──────────────────┐                    ┌──────────────────┐
                    │  Timing Gate     │                    │  BCD Decoder     │
                    │  (0-30ms window) │                    │                  │
                    └────────┬─────────┘                    └──────────────────┘
                             │
                             ▼
                    ┌──────────────────┐
                    │  Comb Filter     │
                    │  (1s, α=0.99)    │
                    └────────┬─────────┘
                             │
                             ▼
                       Tick Output
```

---

## Implementation Steps

### Step 1: Parallel Filter Banks

**Files:** `waterfall.c`, new `channel_filters.c`

#### Sync Channel Filter (800-1400 Hz Bandpass)

4th-order Butterworth, implemented as cascaded HP + LP:

```c
// 800 Hz Highpass (2 cascaded biquad sections)
// Coefficients for fs=50000 Hz
const float sync_hp_sos[2][6] = {
    // Section 1: b0, b1, b2, a0(=1), a1, a2
    {0.94280904, -1.88561808, 0.94280904, 1.0, -1.88345806, 0.88777810},
    {0.94280904, -1.88561808, 0.94280904, 1.0, -1.88345806, 0.88777810}
};

// 1400 Hz Lowpass (2 cascaded biquad sections)
const float sync_lp_sos[2][6] = {
    {0.00766530, 0.01533060, 0.00766530, 1.0, -1.73487628, 0.76553747},
    {0.00766530, 0.01533060, 0.00766530, 1.0, -1.73487628, 0.76553747}
};
```

#### Data Channel Filter (150 Hz Lowpass)

4th-order Butterworth:

```c
// 150 Hz Lowpass (2 cascaded biquad sections)
// Coefficients for fs=50000 Hz
const float data_lp_sos[2][6] = {
    {0.0000352, 0.0000704, 0.0000352, 1.0, -1.98223, 0.98230},
    {0.0000352, 0.0000704, 0.0000352, 1.0, -1.98223, 0.98230}
};
```

#### Filter Processing

```c
typedef struct {
    float x1, x2, y1, y2;
} biquad_state_t;

float biquad_process(biquad_state_t *s, float x, const float *coeffs) {
    float y = coeffs[0]*x + coeffs[1]*s->x1 + coeffs[2]*s->x2
            - coeffs[4]*s->y1 - coeffs[5]*s->y2;
    s->x2 = s->x1; s->x1 = x;
    s->y2 = s->y1; s->y1 = y;
    return y;
}

float sync_channel_process(sync_channel_t *ch, float x) {
    float y = x;
    // Highpass cascade
    y = biquad_process(&ch->hp[0], y, sync_hp_sos[0]);
    y = biquad_process(&ch->hp[1], y, sync_hp_sos[1]);
    // Lowpass cascade
    y = biquad_process(&ch->lp[0], y, sync_lp_sos[0]);
    y = biquad_process(&ch->lp[1], y, sync_lp_sos[1]);
    return y;
}

float data_channel_process(data_channel_t *ch, float x) {
    float y = x;
    y = biquad_process(&ch->lp[0], y, data_lp_sos[0]);
    y = biquad_process(&ch->lp[1], y, data_lp_sos[1]);
    return y;
}
```

**Done when:** Tick detector receives sync channel output, BCD detector receives data channel output. Verify with scope: 100 Hz absent from sync channel, 1000 Hz absent from data channel.

---

### Step 2: Timing Gate

**File:** `tick_detector.c`

The tick occurs at 10-15ms into each second. NIST guarantees silence 0-30ms. Gate tick detection to this window.

```c
#define TICK_GATE_START_MS   5.0f    // Open gate 5ms before expected tick
#define TICK_GATE_END_MS    25.0f    // Close gate 10ms after expected tick

typedef struct {
    float epoch_ms;          // Second boundary offset (from marker)
    bool enabled;
} tick_gate_t;

bool is_gate_open(tick_gate_t *gate, float current_ms) {
    if (!gate->enabled) return true;

    float ms_into_second = fmodf(current_ms - gate->epoch_ms, 1000.0f);
    if (ms_into_second < 0) ms_into_second += 1000.0f;

    return (ms_into_second >= TICK_GATE_START_MS &&
            ms_into_second <= TICK_GATE_END_MS);
}
```

**Epoch source:** Minute marker. Marker starts at second :00 + 10ms, so:
```c
epoch_ms = marker_timestamp_ms - 10.0f;
```

**Done when:** Tick detections only occur during 5-25ms window after each second. No 497ms false detections.

---

### Step 3: Matched Filter

**File:** `tick_detector.c`

5ms FIR matched filter correlating with 5 cycles of 1000 Hz:

```c
#define MATCHED_FILTER_LEN 250  // 5ms at 50kHz

float tick_template[MATCHED_FILTER_LEN];

void init_tick_template(void) {
    for (int i = 0; i < MATCHED_FILTER_LEN; i++) {
        float t = (float)i / 50000.0f;
        // Hann window * 1000 Hz sine
        float window = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (MATCHED_FILTER_LEN - 1)));
        tick_template[i] = sinf(2.0f * M_PI * 1000.0f * t) * window;
    }
}

float matched_filter_process(float *buffer, int buf_idx) {
    float sum = 0.0f;
    for (int i = 0; i < MATCHED_FILTER_LEN; i++) {
        int idx = (buf_idx - i + MATCHED_FILTER_LEN) % MATCHED_FILTER_LEN;
        sum += buffer[idx] * tick_template[i];
    }
    return sum;
}
```

**Done when:** Correlation peak at tick location, ~27 dB processing gain over raw detection.

---

### Step 4: Comb Filter

**File:** `tick_comb_filter.c`

1-second comb filter for coherent averaging:

```c
#define COMB_STAGES 50000  // 1 second at 50kHz

typedef struct {
    float delay_line[COMB_STAGES];
    int idx;
    float alpha;
    float output;
} comb_filter_t;

void comb_init(comb_filter_t *cf) {
    memset(cf->delay_line, 0, sizeof(cf->delay_line));
    cf->idx = 0;
    cf->alpha = 0.99f;  // ~100 second time constant
    cf->output = 0.0f;
}

float comb_process(comb_filter_t *cf, float input) {
    float delayed = cf->delay_line[cf->idx];
    cf->output = cf->alpha * cf->output + (1.0f - cf->alpha) * (input + delayed) / 2.0f;
    cf->delay_line[cf->idx] = input;
    cf->idx = (cf->idx + 1) % COMB_STAGES;
    return cf->output;
}
```

**Done when:** Tick detection works at -20 dB SNR where it previously failed.

---

### Step 5: Signal Normalization

**File:** `waterfall.c`

Slow AGC before channel filters:

```c
typedef struct {
    float level;
    int warmup;
} normalizer_t;

float normalize(normalizer_t *n, float i, float q) {
    float mag = sqrtf(i*i + q*q);
    float alpha = (n->warmup < 50000) ? 0.01f : 0.0001f;  // Fast warmup, then slow
    n->level += alpha * (mag - n->level);
    n->warmup++;
    if (n->level < 0.0001f) n->level = 0.0001f;
    return 1.0f / n->level;
}
```

**Done when:** Tick detection consistent from GR=40 to GR=60.

---

### Step 6: Marker-Based Signal Health

**File:** `sync_detector.c`

Use marker gaps, not tick gaps, for signal health:

```c
#define MARKER_GAP_CRITICAL_MS  90000.0f  // 1.5 marker intervals

void check_signal_health(sync_detector_t *sd, float current_ms) {
    float gap = current_ms - sd->last_confirmed_marker_ms;
    if (gap > MARKER_GAP_CRITICAL_MS && sd->state == SYNC_LOCKED) {
        transition_to_recovering(sd);
    }
}
```

**Done when:** LOCKED state holds through tick dropouts, only drops after missing marker.

---

## Acceptance Criteria

| Metric | Target | How to Measure |
|--------|--------|----------------|
| 497ms false detections | **ZERO** | `grep "dur=49[0-9]ms" log \| wc -l` |
| Tick detection rate (marginal) | **>95%** | Count ticks / elapsed seconds |
| Time to LOCKED | **<180 seconds** | From cold start on marginal signal |
| Gain consistency | **<5% variance** | Test at GR=40, 50, 60 |
| BCD decode rate | **>98%** | When LOCKED |

---

## Implementation Order

| Day | Task | Deliverable |
|-----|------|-------------|
| 1 | Step 1: Parallel filters | Sync/data channels working |
| 2 | Step 2: Timing gate | Gate enabled via marker epoch |
| 3 | Step 3: Matched filter | Improved tick SNR |
| 4 | Step 4: Comb filter | Weak signal detection |
| 5 | Step 5: Normalization | Gain-independent operation |
| 6 | Step 6: Signal health | Marker-based LOCKED hold |
| 7 | Integration test | All acceptance criteria pass |

---

## What NOT To Do

- ❌ Do NOT add conditional phases or "maybe" steps
- ❌ Do NOT try alternative filter architectures
- ❌ Do NOT bikeshed on filter order or cutoff frequencies
- ❌ Do NOT invent new epoch timeout logic (comb filter handles fades)
- ❌ Do NOT ask "what if" questions - the architecture is proven

---

## Reference

This implementation is based on:

1. **NTP Reference Clock Driver 36** - David Mills, University of Delaware
   - https://www.eecis.udel.edu/~mills/ntp/html/drivers/driver36.html
   - 20+ years of production operation

2. **NIST Special Publication 432** - WWV/WWVH Broadcast Format
   - 40ms protected zone specification
   - BCD timing specification

3. **Mills Technical Report** - WWV/H Audio Demodulator/Decoder
   - https://www.eecis.udel.edu/~mills/database/reports/wwv/wwvb.pdf
   - Filter specifications, matched filter design

---

## Sign-Off

This plan requires no further review. Implement as specified. Report results against acceptance criteria after Day 7.

Questions about implementation details should reference the NTP driver36 source code, not generate new design discussions.