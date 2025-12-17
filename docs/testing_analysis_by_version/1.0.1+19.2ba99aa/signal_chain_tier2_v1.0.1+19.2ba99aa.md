# Phoenix SDR v1.0.1+19.2ba99aa Signal Chain - TIER 2: Detector Processing

## INPUT FROM TIER 1

```
det_i, det_q @ 50 kHz (from detector path decimation)
    │
    ├──► tick_detector_process_sample()
    └──► marker_detector_process_sample()
```

Both detectors receive the **same** 50 kHz I/Q stream, but process it independently.

---

## TICK DETECTOR (tick_detector.c)

### Purpose
Detect 5ms WWV tick pulses at 1000 Hz, occurring once per second.

### Processing Pipeline

```
det_i, det_q @ 50 kHz
    │
    ├──► MATCHED FILTER PATH (sample-by-sample)
    │    │
    │    ├── Circular buffer: 512 samples (corr_buf_i, corr_buf_q)
    │    │
    │    ├── Template: 250 samples of Hann-windowed 1000 Hz tone
    │    │   ├── template_i[i] = cos(2π × 1000 × t) × hann(i)
    │    │   └── template_q[i] = sin(2π × 1000 × t) × hann(i)
    │    │
    │    └── Correlation (every 8th sample for efficiency):
    │        corr = |Σ (sig × conj(template))|
    │             = sqrt(sum_i² + sum_q²)
    │
    └──► FFT PATH (every 256 samples = 5.12 ms)
         │
         ├── Buffer: 256 samples (i_buffer, q_buffer)
         │
         ├── Window: Hann (256 points)
         │   window[i] = 0.5 × (1 - cos(2πi/255))
         │
         ├── FFT: 256-point complex
         │   fft_in[i].r = i_buffer[i] × window[i]
         │   fft_in[i].i = q_buffer[i] × window[i]
         │   kiss_fft(fft_in → fft_out)
         │
         └── Bucket Energy Extraction:
             ├── Target: 1000 Hz
             ├── Bandwidth: ±100 Hz
             ├── Center bin: 1000 / 195.3 = 5.12 → bin 5
             ├── Bin span: 100 / 195.3 = 0.51 → ±1 bin
             │
             └── Energy calculation:
                 for bins 4,5,6 (positive) and 250,251,252 (negative):
                     energy += |fft_out[bin]| / FFT_SIZE
```

### FFT Characteristics

| Parameter | Value | Calculation |
|-----------|-------|-------------|
| FFT Size | 256 | Fixed |
| Sample Rate | 50,000 Hz | From Tier 1 |
| Frame Duration | **5.12 ms** | 256 / 50000 × 1000 |
| Hz per Bin | **195.3 Hz** | 50000 / 256 |
| Nyquist | 25,000 Hz | 50000 / 2 |
| 1000 Hz Bin | 5.12 → **bin 5** | 1000 / 195.3 |
| ±100 Hz span | **±0.5 bins** | 100 / 195.3 |

### Detection State Machine

```
                    energy > threshold_high
        ┌───────────────────────────────────────────┐
        │                                           │
        ▼                                           │
   ┌─────────┐    energy < threshold_low    ┌──────┴─────┐
   │  IDLE   │◄─────────────────────────────│  IN_TICK   │
   └────┬────┘                              └──────┬─────┘
        │                                          │
        │                                          │ classify by duration
        │                                          ▼
        │                               ┌──────────────────────┐
        │                               │ 2-50ms → TICK        │
        │                               │ 600-1500ms → MARKER  │
        │                               │ else → REJECTED      │
        │                               └──────────┬───────────┘
        │                                          │
        │         cooldown expires                 │
        └──────────────────────────────────────────┘
                     STATE_COOLDOWN (500ms)
```

### Adaptive Thresholding

```c
// During IDLE when energy < threshold:
if (energy < noise_floor) {
    noise_floor += 0.002 × (energy - noise_floor);  // Fast down
} else {
    noise_floor += 0.0002 × (energy - noise_floor); // Slow up
}
threshold_high = noise_floor × 2.0;
threshold_low = threshold_high × 0.7;  // Hysteresis
```

### Matched Filter Details

```
Template: 250 samples = 5ms at 50kHz
    ├── Frequency: 1000 Hz (WWV tick tone)
    ├── Window: Hann (smooth edges)
    └── Duration: Matches WWV tick pulse

Correlation computed every 8 samples:
    ├── Complex multiply-accumulate over 250 samples
    ├── Output: correlation magnitude
    └── Purpose: Confirm FFT detection, measure pulse shape

Correlation noise floor tracking:
    ├── Adapts during IDLE state
    └── Used for corr_ratio = peak / noise_floor
```

---

## MARKER DETECTOR (marker_detector.c)

### Purpose
Detect 800ms WWV minute markers at 1000 Hz, occurring once per minute.

### Processing Pipeline

```
det_i, det_q @ 50 kHz
    │
    └──► FFT PATH (every 256 samples = 5.12 ms)
         │
         ├── Buffer: 256 samples (i_buffer, q_buffer)
         │
         ├── Window: Hann (256 points)
         │
         ├── FFT: 256-point complex
         │
         ├── Bucket Energy Extraction:
         │   ├── Target: 1000 Hz
         │   ├── Bandwidth: ±200 Hz (wider than tick detector)
         │   ├── Center bin: 5
         │   └── Bin span: ±1 bin
         │
         └── SLIDING WINDOW ACCUMULATOR
             │
             ├── Window: 195 frames ≈ 1000 ms
             │   (catches 800ms marker fully)
             │
             ├── Circular buffer: energy_history[195]
             │
             └── accumulated_energy = Σ energy_history
```

### FFT Characteristics (same as tick detector)

| Parameter | Value |
|-----------|-------|
| FFT Size | 256 |
| Sample Rate | 50,000 Hz |
| Frame Duration | 5.12 ms |
| Hz per Bin | 195.3 Hz |
| 1000 Hz Bin | bin 5 |

### Sliding Window Accumulator

```
Purpose: Integrate energy over ~1 second to detect 800ms pulse

Operation (every 5.12ms frame):
    1. Remove oldest energy from accumulator
    2. Add new energy to accumulator
    3. Compare accumulated vs threshold

    accumulated_energy = Σ(last 195 frames of bucket energy)
    
    ┌─────────────────────────────────────────┐
    │ frame energies: [e0, e1, e2, ... e194]  │
    │                  ▲                       │
    │                  │ circular write        │
    │                  │                       │
    │ accumulated = e0 + e1 + ... + e194      │
    └─────────────────────────────────────────┘
```

### Detection State Machine

```
                accumulated > threshold (3× baseline)
        ┌──────────────────────────────────────────────┐
        │                                              │
        ▼                                              │
   ┌─────────┐    accumulated < threshold      ┌───────┴──────┐
   │  IDLE   │◄────────────────────────────────│  IN_MARKER   │
   └────┬────┘                                 └───────┬──────┘
        │                                              │
        │ baseline adapts                              │ 500ms-5000ms
        │ slowly here                                  │ duration check
        │                                              ▼
        │                               ┌─────────────────────────┐
        │                               │ Valid: 500-5000ms       │
        │                               │ Timeout: >5000ms reset  │
        │                               └──────────┬──────────────┘
        │                                          │
        │         30 second cooldown               │
        └──────────────────────────────────────────┘
                     STATE_COOLDOWN
```

### Self-Tracking Baseline

```c
// During IDLE state only:
baseline_energy += 0.001 × (accumulated_energy - baseline_energy);
threshold = baseline_energy × 3.0;

// Key insight: baseline tracks ACCUMULATED energy (sum of 195 frames)
// NOT individual frame energy
```

---

## COMPARISON: TICK vs MARKER DETECTOR

| Aspect | Tick Detector | Marker Detector |
|--------|---------------|-----------------|
| **Target Signal** | 5ms pulse | 800ms pulse |
| **FFT Size** | 256 | 256 |
| **Frame Rate** | 195 Hz (5.12ms) | 195 Hz (5.12ms) |
| **Bandwidth** | ±100 Hz | ±200 Hz |
| **Detection Method** | Single-frame threshold | Sliding window accumulator |
| **Window Length** | 1 frame (5.12ms) | 195 frames (1000ms) |
| **Matched Filter** | Yes (250 samples) | No |
| **Baseline Tracking** | Per-frame energy | Accumulated energy |
| **Cooldown** | 500ms | 30,000ms |

---

## ENERGY CALCULATION DETAIL

Both detectors use the same bucket energy calculation:

```c
float calculate_bucket_energy(fft_out, target_hz, bandwidth_hz) {
    int center_bin = round(target_hz / HZ_PER_BIN);  // 1000/195.3 = 5
    int bin_span = round(bandwidth_hz / HZ_PER_BIN); // 100/195.3 = 1
    
    float energy = 0;
    
    // Positive frequency bins
    for (b = -bin_span to +bin_span) {
        bin = center_bin + b;  // bins 4, 5, 6
        energy += magnitude(fft_out[bin]) / FFT_SIZE;
    }
    
    // Negative frequency bins (conjugate symmetry)
    for (b = -bin_span to +bin_span) {
        bin = FFT_SIZE - center_bin + b;  // bins 250, 251, 252
        energy += magnitude(fft_out[bin]) / FFT_SIZE;
    }
    
    return energy;
}

// Where magnitude = sqrt(re² + im²)
// Division by FFT_SIZE normalizes for FFT scaling
```

### Energy Units

The energy value is in **arbitrary units** depending on:
- Input signal amplitude (raw S16 values, not normalized)
- FFT size normalization (/256)
- Number of bins summed (typically 6)
- Hann window gain factor (~0.5)

Typical observed values:
- Noise floor: ~0.01 to 0.1
- Tick pulse peak: ~0.5 to 5.0
- Marker accumulated (195 frames): ~1000 to 3000

---

## TIMING ANALYSIS

```
Input: 50,000 samples/sec

Tick Detector:
├── FFT every 256 samples = every 5.12 ms
├── 195.3 FFT frames per second
├── 5ms tick spans ~1 FFT frame
└── Good temporal resolution for 5ms pulses

Marker Detector:
├── FFT every 256 samples = every 5.12 ms
├── Accumulates 195 frames = 1000 ms window
├── 800ms marker fills ~80% of window
└── Smooths over short-term variations
```

---

## POTENTIAL ISSUES IDENTIFIED

1. **Coarse Frequency Resolution**
   - 195 Hz/bin means 1000 Hz target is only ~5 bins from DC
   - Limited ability to distinguish nearby frequencies
   - OK for WWV (clean signal at exactly 1000 Hz)

2. **No Overlap in FFT**
   - Each FFT uses fresh 256 samples (no overlap)
   - Could miss pulse edges that straddle frame boundaries
   - Matched filter compensates for tick detector

3. **Same FFT for Both Detectors**
   - tick_detector and marker_detector do identical FFTs
   - Could share FFT output to save CPU
   - Currently independent (cleaner code, more CPU)

4. **Hann Window Gain**
   - Hann window has coherent gain of 0.5
   - Energy values are lower than actual signal
   - Doesn't affect detection (relative thresholds)

---

## NEXT TIER: What happens after detection?

Detected events feed into:
- `tick_correlator.c`: Correlates ticks with expected WWV timing
- `marker_correlator.c`: Correlates markers with expected minute boundaries
- `sync_detector.c`: Determines overall sync status
- Callbacks to `waterfall.c` for UI/logging
