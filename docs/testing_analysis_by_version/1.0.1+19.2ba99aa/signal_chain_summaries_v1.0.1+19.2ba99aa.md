# Phoenix SDR v1.0.1+19.2ba99aa - Individual Signal Chain Summaries

Each section documents one complete signal chain from input to output, including all math with code citations.

---

# 1. TICK DETECTOR CHAIN

## Purpose
Detect 5ms WWV tick pulses at 1000 Hz, occurring once per second.

## Signal Path

```
SDRplay RSP2 (2 MHz S16)
    │
    ▼ [waterfall.c:655-659] Format conversion
    │   i_raw = (float)samples[s * 2]
    │   q_raw = (float)samples[s * 2 + 1]
    │
    ▼ [waterfall.c:666-667] Lowpass filter @ 5 kHz
    │   det_i = lowpass_process(&g_detector_lowpass_i, i_raw)
    │   det_q = lowpass_process(&g_detector_lowpass_q, q_raw)
    │
    ▼ [waterfall.c:669-673] Decimation 40:1
    │   if (++counter >= 40) → output sample
    │
    ▼ [waterfall.c:671] tick_detector_process_sample(det_i, det_q)
    │
    ▼ [tick_detector.c:388-389] Buffer samples
    │   i_buffer[idx] = i_sample
    │   q_buffer[idx] = q_sample
    │
    ▼ [tick_detector.c:398-401] Apply Hann window + FFT
    │   fft_in[i].r = i_buffer[i] × window[i]
    │   fft_in[i].i = q_buffer[i] × window[i]
    │   kiss_fft(fft_cfg, fft_in, fft_out)
    │
    ▼ [tick_detector.c:143-165] Extract bucket energy
    │   energy = Σ |fft_out[bins]| / FFT_SIZE
    │
    ▼ [tick_detector.c:181-287] State machine
    │   IDLE → IN_TICK → COOLDOWN
    │
    ▼ [tick_detector.c:253-270] Callback + CSV output
```

## Math

### Lowpass Filter (Butterworth 2nd-order)
**Citation:** `waterfall.c:163-181`

```
Transfer function coefficients:
    ω₀ = 2π × f_cutoff / f_sample = 2π × 5000 / 2000000 = 0.01571
    α = sin(ω₀) / (2 × Q) = sin(0.01571) / 1.4142 = 0.01111
    
    b₀ = (1 - cos(ω₀)) / 2 / a₀ ≈ 6.0×10⁻⁶
    b₁ = (1 - cos(ω₀)) / a₀ ≈ 1.2×10⁻⁵  
    b₂ = b₀
    a₁ = -2 × cos(ω₀) / a₀ ≈ -1.978
    a₂ = (1 - α) / a₀ ≈ 0.978

Output:
    y[n] = b₀×x[n] + b₁×x[n-1] + b₂×x[n-2] - a₁×y[n-1] - a₂×y[n-2]
```

### FFT Parameters
**Citation:** `tick_detector.h:24-27`

```
FFT_SIZE = 256
SAMPLE_RATE = 50,000 Hz
FRAME_DURATION = 256 / 50000 = 5.12 ms
HZ_PER_BIN = 50000 / 256 = 195.3 Hz
```

### Hann Window
**Citation:** `tick_detector.c:320-322`

```
window[i] = 0.5 × (1 - cos(2πi / (N-1)))
```

### Bucket Energy Extraction
**Citation:** `tick_detector.c:143-165`

```
Target: 1000 Hz ± 100 Hz
Center bin = round(1000 / 195.3) = 5
Bin span = round(100 / 195.3) = 1

For bins {4, 5, 6} (positive) and {250, 251, 252} (negative):
    magnitude = √(re² + im²)
    energy += magnitude / FFT_SIZE

Total energy = Σ pos_energy + Σ neg_energy
```

### Matched Filter Correlation
**Citation:** `tick_detector.c:114-140`

```
Template: 250 samples (5ms × 50kHz) of Hann-windowed 1000 Hz tone
    template_i[n] = cos(2π × 1000 × n/50000) × hann(n)
    template_q[n] = sin(2π × 1000 × n/50000) × hann(n)

Correlation (complex multiply-accumulate):
    sum_i = Σ (sig_i × tpl_i + sig_q × tpl_q)
    sum_q = Σ (sig_q × tpl_i - sig_i × tpl_q)
    correlation = √(sum_i² + sum_q²)
```

### Adaptive Threshold
**Citation:** `tick_detector.c:181-206`

```
During IDLE (energy < threshold):
    if energy < noise_floor:
        noise_floor += 0.002 × (energy - noise_floor)  // Fast down
    else:
        noise_floor += 0.0002 × (energy - noise_floor) // Slow up

threshold_high = noise_floor × 2.0
threshold_low = threshold_high × 0.7  // Hysteresis
```

## Output
- `tick_event_t` callback for each valid tick (2-50ms duration)
- `tick_marker_event_t` callback for minute markers (600-1500ms duration)
- CSV: `wwv_ticks.csv`

---

# 2. MARKER DETECTOR CHAIN

## Purpose
Detect 800ms WWV minute markers at 1000 Hz using sliding window accumulator.

## Signal Path

```
SDRplay RSP2 (2 MHz S16)
    │
    ▼ [waterfall.c:655-667] Same decimation path as tick detector
    │
    ▼ [waterfall.c:672] marker_detector_process_sample(det_i, det_q)
    │
    ▼ [marker_detector.c:295-299] Buffer samples
    │
    ▼ [marker_detector.c:302-305] Apply Hann window + FFT
    │
    ▼ [marker_detector.c:105-126] Extract bucket energy
    │
    ▼ [marker_detector.c:139-146] Update sliding window accumulator
    │   accumulated = Σ(last 195 frame energies)
    │
    ▼ [marker_detector.c:148-216] State machine
    │   IDLE → IN_MARKER → COOLDOWN
    │
    ▼ [marker_detector.c:181-208] Callback + CSV output
```

## Math

### FFT Parameters
**Citation:** `marker_detector.h:21-28`

```
FFT_SIZE = 256
SAMPLE_RATE = 50,000 Hz
FRAME_DURATION = 5.12 ms
HZ_PER_BIN = 195.3 Hz
WINDOW_FRAMES = 195 (~1000 ms)
```

### Bucket Energy (same as tick detector)
**Citation:** `marker_detector.c:105-126`

```
Target: 1000 Hz ± 200 Hz (wider than tick)
Center bin = 5
Bin span = round(200 / 195.3) = 1
```

### Sliding Window Accumulator
**Citation:** `marker_detector.c:139-146`

```
energy_history[195] = circular buffer of frame energies

Each frame:
    accumulated_energy -= energy_history[oldest]
    energy_history[current] = new_energy
    accumulated_energy += new_energy
```

### Self-Tracking Baseline
**Citation:** `marker_detector.c:167-172`

```
During IDLE state:
    baseline_energy += 0.001 × (accumulated_energy - baseline_energy)
    threshold = baseline_energy × 3.0
```

## Output
- `marker_event_t` callback for each valid marker (500-5000ms duration)
- CSV: `wwv_markers.csv`
- Debug CSV: `wwv_debug_marker.csv`

---

# 3. SLOW MARKER DETECTOR CHAIN

## Purpose
Independent marker detection on high-resolution 12 kHz display path for cross-validation.

## Signal Path

```
SDRplay RSP2 (2 MHz S16)
    │
    ▼ [waterfall.c:679-680] Lowpass filter @ 6 kHz
    │   disp_i = lowpass_process(&g_display_lowpass_i, i_raw)
    │   disp_q = lowpass_process(&g_display_lowpass_q, q_raw)
    │
    ▼ [waterfall.c:682-695] Decimation 166:1 → 12 kHz
    │
    ▼ [waterfall.c:723-726] Apply Blackman-Harris window + FFT (2048-pt)
    │
    ▼ [waterfall.c:729-731] Feed to slow_marker_detector
    │   slow_marker_detector_process_fft(g_slow_marker, fft_out, timestamp)
    │
    ▼ [slow_marker_detector.c:51-90] Extract signal + noise energy
    │
    ▼ [slow_marker_detector.c:92-107] Update 10-frame accumulator
    │
    ▼ [slow_marker_detector.c:109-128] Threshold check + callback
```

## Math

### FFT Parameters
**Citation:** `slow_marker_detector.h:16-20`

```
FFT_SIZE = 2048
SAMPLE_RATE = 12,000 Hz
HZ_PER_BIN = 12000 / 2048 = 5.86 Hz
FRAME_EFFECTIVE = 85.3 ms (50% overlap)
ACCUM_FRAMES = 10 (~850 ms)
```

### Signal Bucket Extraction
**Citation:** `slow_marker_detector.c:51-67`

```
Target: 1000 Hz ± 50 Hz (tight bucket)
center_bin = round(1000 / 5.86) = 170
bin_span = round(50 / 5.86) = 8

signal_energy = Σ |fft_out[bins 162..178]| / FFT_SIZE
```

### Noise Estimation
**Citation:** `slow_marker_detector.c:69-82`

```
Adjacent bands: 800-900 Hz (bins ~137-153) and 1100-1200 Hz (bins ~187-204)
noise_energy = Σ |fft_out[noise_bins]| / FFT_SIZE / count
```

### Threshold
**Citation:** `slow_marker_detector.c:109-111`

```
threshold = noise_floor × 2.0 × ACCUM_FRAMES
above_threshold = (accumulated_energy > threshold)
```

## Output
- `slow_marker_frame_t` callback every ~85 ms
- Used by `marker_correlator` for fast/slow fusion

---

# 4. TONE TRACKER CHAIN (×3 instances)

## Purpose
Precision frequency measurement of carrier (DC) and subcarriers (500/600 Hz) for receiver characterization.

## Instances
| Instance | Nominal Hz | Purpose |
|----------|-----------|---------|
| g_tone_carrier | 0 (DC) | LO offset → PPM error |
| g_tone_500 | 500 | Subcarrier presence/accuracy |
| g_tone_600 | 600 | Subcarrier presence/accuracy |

## Signal Path

```
SDRplay RSP2 (2 MHz S16)
    │
    ▼ [waterfall.c:679-695] Decimation to 12 kHz (same as display path)
    │
    ▼ [waterfall.c:691-693] Feed tone trackers
    │   tone_tracker_process_sample(g_tone_XXX, disp_i, disp_q)
    │
    ▼ [tone_tracker.c:174-186] Circular buffer fill
    │
    ▼ [tone_tracker.c:188-193] Apply Blackman-Harris window + FFT (4096-pt)
    │
    ▼ [tone_tracker.c:96-136] Find USB/LSB peaks + parabolic interpolation
    │
    ▼ [tone_tracker.c:138-145] Calculate offset + PPM
```

## Math

### FFT Parameters
**Citation:** `tone_tracker.h:16-20`

```
FFT_SIZE = 4096
SAMPLE_RATE = 12,000 Hz
HZ_PER_BIN = 12000 / 4096 = 2.93 Hz
FRAME_DURATION = 4096 / 12000 = 341 ms
```

### Blackman-Harris Window
**Citation:** `tone_tracker.c:40-51`

```
a₀ = 0.35875, a₁ = 0.48829, a₂ = 0.14128, a₃ = 0.01168

window[i] = a₀ - a₁×cos(2πn) + a₂×cos(4πn) - a₃×cos(6πn)
where n = i / (N-1)
```

### Peak Finding with Parabolic Interpolation
**Citation:** `tone_tracker.c:57-72`

```
Given peak at bin k with neighbors α=mag[k-1], β=mag[k], γ=mag[k+1]:

    p = 0.5 × (α - γ) / (α - 2β + γ)
    
    fractional_bin = k + p
    frequency = fractional_bin × HZ_PER_BIN
```

### Dual-Sideband Measurement
**Citation:** `tone_tracker.c:117-131`

```
For tone at f_nominal (e.g., 500 Hz):
    USB bin = round(f_nominal / HZ_PER_BIN)
    LSB bin = FFT_SIZE - USB bin

Find peaks in both:
    usb_hz = usb_peak_frac × HZ_PER_BIN
    lsb_hz = (FFT_SIZE - lsb_peak_frac) × HZ_PER_BIN

Average:
    measured_hz = (usb_hz + lsb_hz) / 2
    offset_hz = measured_hz - f_nominal
    offset_ppm = (offset_hz / f_nominal) × (10MHz / 1e6)
```

### SNR Calculation
**Citation:** `tone_tracker.c:133-135`

```
peak_mag = max(usb_mag, lsb_mag)
noise_floor = average of non-signal bins
SNR_dB = 20 × log₁₀(peak_mag / noise_floor)

Valid if SNR_dB ≥ 10 dB
```

## Output
- Real-time: `measured_hz`, `offset_hz`, `offset_ppm`, `snr_db`, `valid`
- CSV: `wwv_carrier.csv`, `wwv_tone_500.csv`, `wwv_tone_600.csv`

---

# 5. TICK CORRELATOR CHAIN

## Purpose
Group consecutive ticks into "chains" to track timing stability and drift.

## Signal Path

```
tick_detector (tick_event_t)
    │
    ▼ [waterfall.c:513-523] on_tick_event callback
    │
    ▼ [tick_correlator.c:96-155] tick_correlator_add_tick()
    │
    ├─▼ [tick_correlator.c:105-107] Interval check
    │       correlates = (900ms ≤ interval ≤ 1050ms)
    │
    ├─▼ [tick_correlator.c:109-114] Chain management
    │       if (!correlates) start_new_chain()
    │
    ├─▼ [tick_correlator.c:120-121] Drift tracking
    │       drift_this_tick = interval - 1000ms
    │       cumulative_drift += drift_this_tick
    │
    └─▼ [tick_correlator.c:139-149] CSV output
```

## Math

### Correlation Window
**Citation:** `tick_correlator.h:16-18`

```
CORR_MIN_INTERVAL = 900 ms
CORR_MAX_INTERVAL = 1050 ms
CORR_NOMINAL = 1000 ms

correlates = (900 ≤ interval ≤ 1050)
```

### Drift Calculation
**Citation:** `tick_correlator.c:118-121`

```
drift_this_tick = actual_interval - 1000 ms
cumulative_drift = Σ drift_this_tick (over chain)

Positive drift = ticks arriving late (receiver slow)
Negative drift = ticks arriving early (receiver fast)
```

### Chain Statistics
**Citation:** `tick_correlator.c:68-83`

```
For each chain:
    tick_count = number of consecutive ticks
    avg_interval = running mean of intervals
    min/max_interval = extremes
    total_drift = cumulative deviation from 1000ms
```

## Output
- `tick_record_t` with chain info (chain_id, position, drift)
- `chain_stats_t` summary per chain
- CSV: `wwv_tick_corr.csv`

---

# 6. SYNC DETECTOR CHAIN

## Purpose
Correlate minute markers from multiple sources to confirm with high confidence.

## Signal Path

```
tick_detector (tick_marker_event_t) ─────────┐
    │                                         │
    ▼ [waterfall.c:477-482] on_tick_marker   │
    │                                         │
    ▼ [sync_detector.c:108-125]              │
        sync_detector_tick_marker()           │
            │                                 │
            ├── Store as pending_tick         │
            └── try_correlate() ◄─────────────┤
                                              │
marker_detector (marker_event_t) ─────────────┤
    │                                         │
    ▼ [waterfall.c:495-502] on_marker_event  │
    │                                         │
    ▼ [sync_detector.c:127-147]              │
        sync_detector_marker_event()          │
            │                                 │
            ├── Store as pending_marker       │
            └── try_correlate() ◄─────────────┘
```

## Math

### Correlation Window
**Citation:** `sync_detector.c:18`

```
CORRELATION_WINDOW = 1500 ms

Both detectors must fire within 1500ms to correlate
```

### Interval Validation
**Citation:** `sync_detector.c:60-75`

```
interval = current_marker - last_confirmed

// Find how many 60-second periods
periods = round(interval / 60000)
expected = periods × 60000
error = |interval - expected|

valid = (error ≤ 5000 ms)  // Within ±5 seconds of minute multiple
```

### State Transitions
**Citation:** `sync_detector.c:77-85`

```
ACQUIRING → (first marker) → TENTATIVE
TENTATIVE → (2+ markers at ~60s) → LOCKED

State determines confidence level
```

## Output
- Confirmed markers with source attribution (TICK, MARK, or BOTH)
- sync_state_t: ACQUIRING, TENTATIVE, LOCKED
- CSV: `wwv_sync.csv`

---

# 7. MARKER CORRELATOR CHAIN

## Purpose
Fuse fast-path (50kHz) and slow-path (12kHz) marker detections for confidence scoring.

## Signal Path

```
marker_detector (50kHz path)
    │
    ▼ [waterfall.c:495-502] on_marker_event
    │
    ▼ [marker_correlator.c:53-61] marker_correlator_fast_event()
        │
        └── Store: fast_pending=true, fast_timestamp, fast_duration

slow_marker_detector (12kHz path)
    │
    ▼ [waterfall.c:483-493] on_slow_marker_frame
    │
    ▼ [marker_correlator.c:63-118] marker_correlator_slow_frame()
        │
        ├── Track slow state (triggered, peak_energy, peak_snr)
        │
        └── If fast_pending and 500ms elapsed:
            └── Emit correlated result with confidence
```

## Math

### Correlation Window
**Citation:** `marker_correlator.c:11`

```
CORRELATION_WINDOW = 500 ms
MIN_DURATION = 500 ms
```

### Confidence Scoring
**Citation:** `marker_correlator.c:85-99`

```
if (fast_duration ≥ 500ms AND slow_triggered):
    confidence = HIGH  // Both paths agree
else if (fast_duration ≥ 500ms):
    confidence = LOW   // Fast only
else if (slow_triggered):
    confidence = LOW   // Slow only
else:
    confidence = NONE
```

## Output
- `correlated_marker_t` with confidence level
- CSV: `wwv_markers_corr.csv`

---

# SUMMARY: FFT CONFIGURATIONS

| Detector | Sample Rate | FFT Size | Hz/bin | Frame Time | Window |
|----------|-------------|----------|--------|------------|--------|
| tick_detector | 50 kHz | 256 | 195.3 | 5.12 ms | Hann |
| marker_detector | 50 kHz | 256 | 195.3 | 5.12 ms | Hann |
| slow_marker | 12 kHz | 2048 | 5.86 | 85.3 ms* | Blackman-Harris |
| tone_tracker | 12 kHz | 4096 | 2.93 | 341 ms | Blackman-Harris |
| display (waterfall) | 12 kHz | 2048 | 5.86 | 85.3 ms* | Blackman-Harris |

*With 50% overlap

---

# SUMMARY: DECIMATION PATHS

| Path | Input | Decimation | Output | Filter |
|------|-------|------------|--------|--------|
| Detector | 2 MHz | 40:1 | 50 kHz | 5 kHz LP |
| Display | 2 MHz | 166:1 | 12 kHz | 6 kHz LP |

**Citations:**
- Detector path: `waterfall.c:666-673`
- Display path: `waterfall.c:679-695`
- Filter init: `waterfall.c:636-641`
