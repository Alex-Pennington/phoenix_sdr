# Tick-Chain Epoch Implementation

## Overview
Implemented tick correlation chain-based second epoch with marker fallback to fix timing gate alignment issues. Tick chains provide ±5ms precision vs marker's ±50ms precision.

## Problem Statement
- Marker-based epoch had ~200ms systematic error
- Timing gate rejected valid ticks outside 0-100ms window
- Gate recovery cycling: "Gate recovery mode ENABLED (5.0s without tick)"
- Erratic tick intervals: 5985ms, 8084ms, 6518ms

## Solution Architecture

### Precision Sources
1. **Tick Chain** (±5ms): 5+ consecutive ticks ~1000ms apart → precise second epoch
2. **Marker** (±50ms): Fallback for startup/recovery, identifies minute boundary

### Callback Flow
```
tick_correlator → on_tick_chain_epoch() → tick_detector_set_epoch_with_source()
                     (waterfall.c)            (EPOCH_SOURCE_TICK_CHAIN)
```

### Source Priority
- Tick chain sets epoch when: chain_length ≥ 5 AND confidence > 0.8
- Marker sets epoch only when: current source == EPOCH_SOURCE_NONE (fallback)

## Implementation Details

### 1. tick_correlator Epoch Calculation
**Files:** `tools/tick_correlator.h`, `tools/tick_correlator.c`

**Added:**
- `typedef void (*epoch_callback_fn)(float epoch_offset_ms, float std_dev_ms, float confidence, void *user_data)`
- `void tick_correlator_set_epoch_callback(tick_correlator_t *tc, epoch_callback_fn callback, void *user_data)`
- Struct fields: `epoch_callback`, `epoch_callback_user_data`, `recent_intervals[5]`, `recent_interval_idx`, `recent_interval_count`

**Logic in `tick_correlator_add_tick()` after chain stats update:**
```c
/* Track recent intervals for epoch calculation */
if (interval_ms > 0) {
    tc->recent_intervals[tc->recent_interval_idx] = interval_ms;
    tc->recent_interval_idx = (tc->recent_interval_idx + 1) % 5;
    if (tc->recent_interval_count < 5) {
        tc->recent_interval_count++;
    }
}

/* Calculate epoch when chain length ≥ 5 */
if (tc->current_chain_length >= 5 && tc->epoch_callback) {
    /* Calculate std_dev from recent intervals */
    float sum = 0, sum_sq = 0;
    int n = tc->recent_interval_count;
    for (int i = 0; i < n; i++) {
        sum += tc->recent_intervals[i];
        sum_sq += tc->recent_intervals[i] * tc->recent_intervals[i];
    }
    float mean = sum / n;
    float variance = (sum_sq / n) - (mean * mean);
    float std_dev_ms = sqrtf(variance > 0 ? variance : 0);

    /* Calculate confidence (1.0 when std_dev=0, 0.0 when std_dev≥50ms) */
    float confidence = 1.0f - (std_dev_ms / 50.0f);
    if (confidence < 0) confidence = 0;
    if (confidence > 1.0f) confidence = 1.0f;

    /* Only call if confidence is reasonable */
    if (confidence > 0.8f) {
        float epoch_offset_ms = fmodf(timestamp_ms, 1000.0f);
        if (epoch_offset_ms < 0) epoch_offset_ms += 1000.0f;
        tc->epoch_callback(epoch_offset_ms, std_dev_ms, confidence, tc->epoch_callback_user_data);
    }
}
```

### 2. tick_detector Source Tracking
**Files:** `tools/tick_detector.h`, `tools/tick_detector.c`

**Added:**
- `typedef enum { EPOCH_SOURCE_NONE, EPOCH_SOURCE_MARKER, EPOCH_SOURCE_TICK_CHAIN } epoch_source_t`
- `void tick_detector_set_epoch_with_source(tick_detector_t *td, float epoch_ms, epoch_source_t source, float confidence)`
- `epoch_source_t tick_detector_get_epoch_source(tick_detector_t *td)`
- `float tick_detector_get_epoch_confidence(tick_detector_t *td)`
- Struct fields: `epoch_source_t epoch_source`, `float epoch_confidence`

**Modified `tick_detector_set_epoch()` to call with_source:**
```c
void tick_detector_set_epoch(tick_detector_t *td, float epoch_ms) {
    /* Legacy function - assume marker source with medium confidence */
    tick_detector_set_epoch_with_source(td, epoch_ms, EPOCH_SOURCE_MARKER, 0.7f);
}
```

**Console logging:**
```c
telem_console("[EPOCH] Set from %s: offset=%.1fms confidence=%.3f\n",
              source_str, normalized_epoch, confidence);
```

### 3. waterfall.c Callback Wiring
**File:** `tools/waterfall.c`

**Added `on_tick_chain_epoch()` callback:**
```c
static void on_tick_chain_epoch(float epoch_offset_ms, float std_dev_ms, float confidence, void *user_data) {
    (void)user_data;

    /* Tick chain has established precise second epoch - update tick detector */
    if (g_tick_detector) {
        /* Only update if we don't already have a better source, or this is higher confidence */
        epoch_source_t current_source = tick_detector_get_epoch_source(g_tick_detector);
        float current_confidence = tick_detector_get_epoch_confidence(g_tick_detector);

        if (current_source != EPOCH_SOURCE_TICK_CHAIN || confidence > current_confidence) {
            tick_detector_set_epoch_with_source(g_tick_detector, epoch_offset_ms,
                                                 EPOCH_SOURCE_TICK_CHAIN, confidence);

            /* Enable gate with tick-chain precision (better than marker) */
            if (!tick_detector_is_gating_enabled(g_tick_detector)) {
                tick_detector_set_gating_enabled(g_tick_detector, true);
            }
        }
    }
}
```

**Modified `on_tick_marker()` and `on_marker_event()` for fallback:**
```c
/* Only set marker epoch if we don't have tick chain epoch yet */
if (current_source == EPOCH_SOURCE_NONE) {
    tick_detector_set_epoch_with_source(g_tick_detector, leading_edge_ms,
                                         EPOCH_SOURCE_MARKER, 0.7f);
    /* ... enable gate ... */
}
```

**Wired callback in initialization:**
```c
/* Create tick correlator */
g_tick_correlator = tick_correlator_create(g_log_csv ? "wwv_tick_corr.csv" : NULL);
if (!g_tick_correlator) {
    fprintf(stderr, "Failed to create tick correlator\n");
    return 1;
}

/* Wire tick chain epoch callback */
tick_correlator_set_epoch_callback(g_tick_correlator, on_tick_chain_epoch, NULL);
```

## Testing

### Build Status
✅ Build successful (no errors, only warnings)

### Expected Behavior
1. **Startup:** Marker sets initial epoch (EPOCH_SOURCE_MARKER, confidence=0.7)
2. **After 5 ticks:** Tick chain calculates epoch (EPOCH_SOURCE_TICK_CHAIN, confidence>0.8)
3. **Console output:**
   ```
   [EPOCH] Set from MARKER: offset=450.0ms confidence=0.700
   [EPOCH] Set from CHAIN: offset=452.3ms confidence=0.950
   ```
4. **Gate behavior:** Stop recovery cycling, tick intervals stabilize to ~1000ms

### Verification Steps
```powershell
# Start SDR server
.\bin\sdr_server.exe -f 5.000450 -g 59 -l 0

# In new terminal, start waterfall
.\bin\waterfall.exe --tcp localhost:4536

# Monitor console for:
# - [EPOCH] MARKER messages at startup
# - [EPOCH] CHAIN messages after 5+ tick correlation
# - Gate should NOT cycle into recovery mode
# - Tick intervals should be ~1000ms ±10ms
```

## Telemetry Extension (Future)
Could extend `TELEM_CORR` CSV output with:
- `epoch_offset_ms`: Calculated offset within second
- `std_dev_ms`: Standard deviation of last 5 intervals
- `confidence`: Confidence metric 0-1

**Backwards compatible:** Append to existing CSV format

## Math Validation
- **Epoch offset:** `timestamp_ms % 1000` → 0-999ms within second
- **Std dev:** Standard deviation of last 5 intervals measures jitter
- **Confidence:** `1.0 - (std_dev / 50.0)` → 1.0 when perfect, 0.0 when ≥50ms jitter
- **Threshold:** Only set epoch when confidence > 0.8 (std_dev < 10ms)

## Related Files
- `tools/tick_correlator.h` - Epoch callback API
- `tools/tick_correlator.c` - Epoch calculation logic
- `tools/tick_detector.h` - Source tracking API
- `tools/tick_detector.c` - Source tracking implementation
- `tools/waterfall.c` - Callback wiring and fallback logic

## References
- [Phoenix SDR Timing Requirements](phoenix_sdr_timing_requirements.md)
- [Phoenix SDR Timing Addendum](phoenix_sdr_timing_addendum.md)
- [WWV Signal Characteristics](docs/wwv_signal_characteristics.md)
- NTP WWV driver (similar tick-train approach for timing discipline)
