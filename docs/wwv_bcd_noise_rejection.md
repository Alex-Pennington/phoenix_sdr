# Noise Rejection Techniques for WWV BCD P-Marker Detection

> 📚 **REFERENCE** - Techniques for improving BCD P-marker detection
>
> This document describes noise rejection techniques from established implementations (NTP WWV Driver by Dave Mills).
> Applicable to `bcd_correlator.c` and `bcd_time_detector.c` for improving P-marker detection accuracy.

---

## The Core Problem

Debug logs show P-markers being falsely detected at positions 4, 21, 33, 35, 42-47 when they should only occur at **0, 9, 19, 29, 39, 49, 59**. This indicates the envelope detector is triggering on transient noise spikes rather than sustained 800ms "hole punches."

---

## Key Techniques from Established Implementations

### 1. Matched Filtering (NTP WWV Driver - Dave Mills)

The gold standard NTP driver uses **synchronous matched filters** rather than simple threshold detection:

- 170-ms matched filter for I/Q channels
- 800-ms synchronous matched filter for minute pulse
- 5-ms FIR matched filter for second pulse
- 8000-stage comb filter for periodic averaging

**Why it works:** A matched filter correlates the incoming signal against a template of the expected pulse shape. Noise spikes that don't match the 800ms marker profile produce low correlation values even if they exceed amplitude thresholds.

**Implementation approach:**

```c
// Correlate against 800ms marker template
float marker_template[800];  // Pre-computed expected envelope shape
float correlation = 0;
for (int i = 0; i < 800; i++) {
    correlation += envelope[sample_offset + i] * marker_template[i];
}
// Only declare P-marker if correlation exceeds threshold
```

---

### 2. Multi-Point Sampling (Pulse-Width Discriminator)

The NTP driver doesn't just check one sample point—it samples at multiple strategic times:

| Sample Point | Time (ms) | Purpose |
|--------------|-----------|---------|
| n | 15 | Noise floor reference |
| s0 | 200 | Short pulse (0 bit) energy |
| s1 | 500 | Long pulse (1 bit) energy |
| e0 | end | Envelope at second end |
| e1 | 200 | Envelope at 200ms |

**Bipolar signal formulation:** `2*s1 - s0 - n`

This formula inherently cancels noise since both s0 and s1 contain the noise component n.

---

### 3. Minimum Duration Validation

From WWVB and IRIG decoders, a fundamental technique is requiring the signal to remain in the "hole" state for a minimum duration:

```c
#define MIN_MARKER_DURATION_MS  600  // P-markers are ~800ms
#define SAMPLES_PER_MS          (SAMPLE_RATE / 1000)

int consecutive_low_samples = 0;
bool marker_valid = false;

for (each sample in detection_window) {
    if (envelope < threshold) {
        consecutive_low_samples++;
        if (consecutive_low_samples >= MIN_MARKER_DURATION_MS * SAMPLES_PER_MS) {
            marker_valid = true;
        }
    } else {
        consecutive_low_samples = 0;  // Reset on any spike
    }
}
```

---

### 4. Hysteresis Thresholds

Use different thresholds for entering vs. exiting the "marker" state:

```c
#define ENTER_THRESHOLD_DB  -6.0   // Must drop below this to start
#define EXIT_THRESHOLD_DB   -3.0   // Must rise above this to end

typedef enum { IDLE, IN_MARKER } MarkerState;
MarkerState state = IDLE;

if (state == IDLE && envelope_db < ENTER_THRESHOLD_DB) {
    state = IN_MARKER;
    marker_start_time = current_time;
} else if (state == IN_MARKER && envelope_db > EXIT_THRESHOLD_DB) {
    state = IDLE;
    marker_duration = current_time - marker_start_time;
    // Validate duration before declaring P-marker
}
```

---

### 5. Comb Filter / Frame Averaging

The NTP driver uses an 8000-stage comb filter that exponentially averages corresponding samples across successive frame intervals:

```c
// Accumulate over multiple frames
integ[phase] += (sample - integ[phase]) / (5 * time_constant);
```

This technique reinforces periodic signals (true markers that repeat every second) while suppressing random noise.

---

### 6. Position-Based Validation

Once frame sync is established, only accept P-markers at expected positions:

```c
if (frame_sync_locked) {
    int expected_positions[] = {0, 9, 19, 29, 39, 49, 59};
    bool position_valid = false;

    for (int i = 0; i < 7; i++) {
        if (abs(detected_position - expected_positions[i]) < tolerance) {
            position_valid = true;
            break;
        }
    }

    if (!position_valid) {
        // Log warning but don't update BCD frame position
        return;
    }
}
```

---

### 7. Maximum-Likelihood with Erasures

The NTP driver treats ambiguous readings as erasures rather than hard 0/1 decisions:

```c
if (signal > positive_threshold) {
    accumulator[bit_position] += 1;   // Hit
} else if (signal < negative_threshold) {
    accumulator[bit_position] -= 1;   // Miss
} else {
    // Erasure - no change to accumulator
}
```

Over multiple frames, the accumulated values converge to the correct interpretation.

---

## Recommended Implementation for Phoenix SDR

### Phase 1: Quick Wins

1. Add minimum duration check (600-700ms sustained dropout)
2. Implement hysteresis (different enter/exit thresholds)
3. Position gating when locked (only accept markers at expected positions)

### Phase 2: Robust Detection

1. Multi-point sampling within each second (15ms, 200ms, 500ms, end)
2. Bipolar signal formulation to cancel noise components
3. Add second-by-second comb filter to reinforce periodic structure

### Phase 3: Maximum Reliability

1. Implement matched filter correlation against expected marker shape
2. Maximum-likelihood accumulator with erasure handling
3. Cross-validate detected time against minute boundary markers

---

## Sample Code Structure

```c
typedef struct {
    float envelope_history[1000];  // 1 second at 1kHz
    int history_idx;
    float baseline_avg;
    float noise_floor;
    MarkerState state;
    uint32_t marker_start_sample;
    int consecutive_low;
} BCDDecoder;

bool detect_p_marker(BCDDecoder* dec, float envelope, int position_in_second) {
    // Update history
    dec->envelope_history[dec->history_idx++ % 1000] = envelope;

    // Hysteresis state machine
    float enter_thresh = dec->noise_floor * 0.25;  // 12dB below
    float exit_thresh = dec->noise_floor * 0.5;    // 6dB below

    if (dec->state == IDLE) {
        if (envelope < enter_thresh) {
            dec->state = IN_MARKER;
            dec->marker_start_sample = dec->history_idx;
            dec->consecutive_low = 1;
        }
    } else {
        if (envelope < enter_thresh) {
            dec->consecutive_low++;
        } else if (envelope > exit_thresh) {
            // Marker ended - validate duration
            int duration_ms = dec->consecutive_low;  // Assuming 1kHz sample rate
            dec->state = IDLE;

            if (duration_ms >= 600 && duration_ms <= 900) {
                // Valid P-marker duration
                // Now check position if we're locked
                return true;
            }
            // Too short or too long - ignore
        }
    }
    return false;
}
```

---

## References

- NTP WWV/WWVH Driver (refclock_wwv.c) by Dave Mills
- WWVB decoders using IRIG-like techniques
