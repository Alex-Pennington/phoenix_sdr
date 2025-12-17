# Phoenix SDR v1.0.1+19.2ba99aa Signal Chain - TIER 3: Post-Detection Correlation

## INPUT FROM TIER 2

```
DETECTOR PATH (50 kHz)                    DISPLAY PATH (12 kHz)
        │                                         │
        ├── tick_detector                         ├── slow_marker_detector
        │   ├── tick_event_t (per tick)          │   └── slow_marker_frame_t (85ms)
        │   └── tick_marker_event_t (minute)     │
        │                                         └── tone_tracker ×3
        └── marker_detector                           └── tone metrics
            └── marker_event_t (per marker)
```

---

## DETECTION EVENT TYPES

### From tick_detector (50 kHz path)

```c
// Normal tick (every second)
typedef struct {
    int tick_number;
    float timestamp_ms;
    float interval_ms;      // Time since previous tick
    float duration_ms;      // 2-50ms for valid tick
    float peak_energy;
    float avg_interval_ms;  // Rolling 15s average
    float noise_floor;
    float corr_peak;        // Matched filter peak
    float corr_ratio;       // corr_peak / corr_noise_floor
} tick_event_t;

// Minute marker (detected by duration 600-1500ms)
typedef struct {
    int marker_number;
    float timestamp_ms;
    float duration_ms;      // 600-1500ms
    float corr_ratio;
    float interval_ms;      // Since last marker (~60s)
} tick_marker_event_t;
```

### From marker_detector (50 kHz path)

```c
typedef struct {
    int marker_number;
    float timestamp_ms;
    float since_last_marker_sec;
    float accumulated_energy;  // Sum of 195 frames (~1s)
    float peak_energy;
    float duration_ms;         // 500-5000ms
} marker_event_t;
```

### From slow_marker_detector (12 kHz path)

```c
typedef struct {
    float energy;           // Accumulated over 10 frames (~850ms)
    float snr_db;
    float noise_floor;
    float timestamp_ms;
    bool above_threshold;   // energy > 2× noise floor
} slow_marker_frame_t;
```

---

## CORRELATION HIERARCHY

```
                     TIER 2: Detections
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
        ▼                   ▼                   ▼
   tick_event        tick_marker_event    marker_event
   (every second)    (minute markers)     (minute markers)
        │                   │                   │
        │                   └─────────┬─────────┘
        │                             │
        ▼                             ▼
  tick_correlator              sync_detector
  (chain tracking)          (marker correlation)
        │                             │
        │                             │
        ▼                             ▼
  tick_record_t              sync_state_t
  chain_stats_t           ACQUIRING → TENTATIVE → LOCKED
                                      │
                                      │ also receives
                                      ▼
                              slow_marker_frame
                              (confirmation from 12kHz path)
                                      │
                                      ▼
                              marker_correlator
                              (fast/slow fusion)
```

---

## TICK CORRELATOR (tick_correlator.c)

### Purpose
Group consecutive ticks into "correlation chains" to track timing stability.

### Chain Formation Rules

```
Tick arrives at timestamp T
    │
    ├── Calculate interval = T - last_tick_ms
    │
    ├── If 900ms ≤ interval ≤ 1050ms:
    │   └── CORRELATES: Add to current chain
    │
    └── Else:
        └── BREAK: Start new chain
```

### Data Flow

```
tick_event_t from tick_detector
    │
    └──► tick_correlator_add_tick()
         │
         ├── Interval Check:
         │   ├── 900-1050ms → Add to current chain
         │   └── else → Start new chain
         │
         ├── Drift Tracking:
         │   drift_this_tick = interval - 1000ms
         │   cumulative_drift += drift_this_tick
         │
         ├── Chain Stats Update:
         │   ├── tick_count++
         │   ├── avg_interval (running average)
         │   ├── min/max interval
         │   └── total_drift
         │
         └── Output:
             ├── tick_record_t (in-memory)
             └── CSV row (wwv_tick_corr.csv)
```

### Output Fields

```
CSV: time,timestamp_ms,tick_num,expected,energy_peak,duration_ms,
     interval_ms,avg_interval_ms,noise_floor,corr_peak,corr_ratio,
     chain_id,chain_pos,chain_start_ms,drift_ms

Example:
08:15:23,42523.1,42,TICK,0.234,12.3,998,999.2,0.012,145.3,12.1,
3,15,27523.1,-28.4

Interpretation:
- Chain #3, position 15 (15th consecutive tick)
- Cumulative drift: -28.4ms (running slightly fast)
```

---

## SYNC DETECTOR (sync_detector.c)

### Purpose
Correlate minute marker detections from multiple sources for high confidence.

### Input Sources

| Source | Event Type | Trigger |
|--------|-----------|---------|
| tick_detector | tick_marker_event_t | Duration 600-1500ms |
| marker_detector | marker_event_t | Accumulated energy threshold |

### State Machine

```
                    ┌────────────────────┐
                    │     ACQUIRING      │
                    │ (waiting for first │
                    │      marker)       │
                    └─────────┬──────────┘
                              │
                              │ First confirmed marker
                              ▼
                    ┌────────────────────┐
                    │     TENTATIVE      │
                    │ (1 marker seen,    │
                    │  need more data)   │
                    └─────────┬──────────┘
                              │
                              │ 2+ markers at ~60s intervals
                              ▼
                    ┌────────────────────┐
                    │      LOCKED        │
                    │ (stable timing,    │
                    │  high confidence)  │
                    └────────────────────┘
```

### Correlation Logic

```c
// When tick_marker_event arrives:
sync_detector_tick_marker(timestamp, duration, corr_ratio)
    │
    ├── Store as pending_tick
    └── try_correlate()

// When marker_event arrives:
sync_detector_marker_event(timestamp, energy, duration)
    │
    ├── Store as pending_marker
    └── try_correlate()

// try_correlate():
if (both pending) {
    delta = |pending_marker_ms - pending_tick_ms|
    if (delta < 1500ms) {
        // Both detectors agree!
        confirm_marker(tick_timestamp, delta, "BOTH")
    } else {
        // Use earlier one
        confirm_marker(earlier_one, 0, source)
    }
}

// Timeout (3 seconds):
if (one pending && partner didn't arrive) {
    confirm_marker(pending, 0, single_source)
}
```

### Interval Validation

```c
// Confirmed markers must be ~60s apart (or multiples)
interval = current_marker - last_confirmed
periods = round(interval / 60000)  // How many minutes
expected = periods * 60000
error = |interval - expected|

if (error <= 5000) {
    // Valid: within ±5 seconds of a minute multiple
    accept_marker()
} else {
    // Invalid: unexpected timing
    reject_marker()
}
```

---

## MARKER CORRELATOR (marker_correlator.c)

### Purpose
Fuse fast-path (50kHz marker_detector) and slow-path (12kHz slow_marker) for confidence scoring.

### Data Flow

```
marker_detector (50kHz path)
    │
    └──► marker_correlator_fast_event(timestamp, duration)
         │
         └── Store fast_pending = true
             fast_timestamp = timestamp
             fast_duration = duration

slow_marker_detector (12kHz path)
    │
    └──► marker_correlator_slow_frame(timestamp, energy, snr, above_threshold)
         │
         ├── Track slow path state during window
         │   ├── slow_triggered = above_threshold
         │   └── slow_peak_energy/snr
         │
         └── If fast_pending and window expired (500ms):
             │
             ├── Both agree → CONF_HIGH
             │   (fast_duration >= 500ms AND slow_triggered)
             │
             ├── Fast only → CONF_LOW
             │   (fast_duration >= 500ms, no slow)
             │
             └── Slow only → CONF_LOW
                 (slow_triggered, fast too short)
```

### Confidence Levels

| Confidence | Fast Path | Slow Path | Meaning |
|------------|-----------|-----------|---------|
| HIGH | ≥500ms duration | above_threshold | Both independent detectors agree |
| LOW | ≥500ms duration | - | Only fast path detected |
| LOW | - | above_threshold | Only slow path detected |
| NONE | <500ms | below threshold | No valid detection |

---

## SLOW MARKER DETECTOR (slow_marker_detector.c)

### Purpose
Independent marker detection on high-resolution 12kHz display path.

### Processing

```
Display FFT output (2048-pt, 12kHz)
    │
    └──► slow_marker_detector_process_fft()
         │
         ├── Extract 1000Hz bucket (±50Hz):
         │   center_bin = 1000 / 5.86 = 170
         │   bin_span = 50 / 5.86 = 8
         │   signal_energy = Σ |fft[162..178]|
         │
         ├── Noise estimate (adjacent bands):
         │   800-900 Hz: bins ~137-153
         │   1100-1200 Hz: bins ~187-204
         │
         ├── Sliding accumulator (10 frames = 850ms):
         │   accumulated = Σ(last 10 signal energies)
         │
         └── Threshold:
             above_threshold = (accumulated > 2× noise× 10)
```

### Differences from Fast Path

| Aspect | Fast (marker_detector) | Slow (slow_marker) |
|--------|----------------------|-------------------|
| Sample Rate | 50 kHz | 12 kHz |
| FFT Size | 256 | 2048 |
| Hz/bin | 195.3 Hz | 5.86 Hz |
| Frame Time | 5.12 ms | 85.3 ms (effective) |
| Window | 195 frames (1s) | 10 frames (850ms) |
| Resolution | Coarse frequency | Fine frequency |
| Speed | Fast updates | Slower, smoother |

---

## TONE TRACKERS (tone_tracker.c)

### Purpose
Precision frequency measurement for carrier and subcarrier characterization.

### Instances

| Tracker | Target | Purpose |
|---------|--------|---------|
| g_tone_carrier | 0 Hz (DC) | Carrier frequency offset → PPM error |
| g_tone_500 | 500 Hz | 500 Hz subcarrier presence |
| g_tone_600 | 600 Hz | 600 Hz subcarrier presence |

### Output Metrics

```c
tone_tracker_get_measured_hz()   // Actual measured frequency
tone_tracker_get_offset_hz()     // Error from nominal
tone_tracker_get_offset_ppm()    // PPM frequency error
tone_tracker_get_snr_db()        // Signal-to-noise ratio
tone_tracker_is_valid()          // Lock status
```

---

## OVERALL DATA FLOW SUMMARY

```
                    SDRplay RSP2 @ 2 MHz
                           │
                    ┌──────┴──────┐
                    │             │
                    ▼             ▼
              50 kHz path    12 kHz path
                    │             │
           ┌────────┼────────┐    │
           │        │        │    │
           ▼        ▼        ▼    ▼
         tick    marker   slow   tone×3
         det.    det.    marker tracker
           │        │        │    │
           │        │        │    └──► CSV/UDP (metrics)
           │        │        │
           │        └────────┼───────► marker_correlator
           │                 │              │
           │                 │              └──► correlated_marker
           │                 │
           ├── tick_event ───┼───────────────► tick_correlator
           │                 │                       │
           │                 │                       └──► chains, drift
           │                 │
           └── marker_event ─┴───────────────► sync_detector
                                                     │
                                                     └──► sync_state
                                                          LOCKED/etc
```

---

## OUTPUT FILES

| File | Source | Content |
|------|--------|---------|
| wwv_ticks.csv | tick_detector | Every detected tick |
| wwv_markers.csv | marker_detector | Every detected marker |
| wwv_tick_corr.csv | tick_correlator | Ticks + chain info |
| wwv_markers_corr.csv | marker_correlator | Correlated markers |
| wwv_sync.csv | sync_detector | Confirmed markers + state |
| wwv_channel.csv | waterfall.c | Channel conditions |
| wwv_subcarrier.csv | waterfall.c | Subcarrier detection |
| wwv_carrier.csv | tone_tracker | Carrier frequency |
| wwv_tone_500.csv | tone_tracker | 500 Hz metrics |
| wwv_tone_600.csv | tone_tracker | 600 Hz metrics |

---

## KEY TIMING CONSTANTS

| Constant | Value | Used By |
|----------|-------|---------|
| Tick interval | 900-1050 ms | tick_correlator chain |
| Marker min duration | 500-600 ms | marker_detector, sync |
| Marker max duration | 1500-5000 ms | marker_detector |
| Marker interval | 55-65 sec | sync_detector |
| Correlation window | 500-1500 ms | marker_correlator, sync |
| Pending timeout | 3000 ms | sync_detector |
| Cooldown (tick) | 500 ms | tick_detector |
| Cooldown (marker) | 30 sec | marker_detector |

---

## POTENTIAL ISSUES IDENTIFIED

1. **Duplicate Marker Detection**
   - tick_detector and marker_detector both detect minute markers
   - tick_detector: via duration (600-1500ms)
   - marker_detector: via accumulated energy threshold
   - sync_detector correlates them, but adds complexity

2. **Path Timing Mismatch**
   - 50 kHz path: 5.12ms frames
   - 12 kHz path: 85.3ms effective frames
   - 16x difference in update rate
   - marker_correlator window (500ms) may be too tight

3. **No Shared State**
   - Each detector runs independently
   - Could miss optimizations (share FFT, share baseline)
   - Trade-off: cleaner code vs. efficiency

4. **Chain Break on Noise**
   - Single missed tick breaks correlation chain
   - No interpolation/smoothing for brief dropouts
   - Could add "grace period" for chain continuation
