# Phoenix SDR v1.0.1+19.2ba99aa Signal Chain Analysis

## TIER 0: SOURCE (sdr_server.c)

```
SDRplay RSP2 Pro
    │
    ├── Center Freq: 15.000 MHz (WWV)
    ├── Sample Rate: 2.000 MHz (native)
    ├── Format: S16 (16-bit signed I/Q pairs)
    ├── Bandwidth: ±1 MHz around center
    │
    └──► TCP Port 4536 (PHXI protocol)
         ├── Header: magic, rate, format, freq
         ├── Frames: IQDQ magic + sample count + raw bytes
         └── Metadata: META magic for gain/freq changes
```

**Data Rate:** 2M samples/sec × 4 bytes/sample = **8 MB/sec**

---

## TIER 1: TCP RECEPTION & FORMAT CONVERSION (waterfall.c main loop)

```
TCP Socket (localhost:4536)
    │
    ├── Protocol Parsing
    │   ├── MAGIC_PHXI (0x50485849): Stream header
    │   ├── MAGIC_IQDQ (0x49514451): Data frame
    │   └── MAGIC_META (0x4D455441): Metadata update
    │
    └── Format Conversion (per sample)
        │
        ├── IQ_FORMAT_S16: int16_t → float (cast)
        │   i_raw = (float)samples[s * 2];
        │   q_raw = (float)samples[s * 2 + 1];
        │
        ├── IQ_FORMAT_F32: float → float (passthrough)
        │   i_raw = samples[s * 2];
        │   q_raw = samples[s * 2 + 1];
        │
        └── IQ_FORMAT_U8: uint8_t → float (centered)
            i_raw = (float)(iq_buffer[s * 2] - 128);
            q_raw = (float)(iq_buffer[s * 2 + 1] - 128);
```

**Output:** Raw I/Q floats at 2 MHz, no normalization

**ISSUE IDENTIFIED:** S16 conversion does NOT normalize to [-1, 1]. Raw int16 values (-32768 to +32767) become float (-32768.0 to +32767.0). This affects absolute energy levels but not relative detection.

---

## TIER 2: DUAL-PATH DECIMATION (waterfall.c)

The raw 2 MHz stream splits into TWO independent processing paths:

```
                        i_raw, q_raw (2 MHz)
                              │
            ┌─────────────────┴─────────────────┐
            │                                   │
            ▼                                   ▼
    ┌───────────────────┐             ┌───────────────────┐
    │   DETECTOR PATH   │             │   DISPLAY PATH    │
    └───────────────────┘             └───────────────────┘
            │                                   │
            ▼                                   ▼
```

### DETECTOR PATH

```
Purpose: Feed tick/marker detectors (optimized for pulse detection)

i_raw, q_raw (2 MHz)
    │
    ├── Biquad Lowpass Filter
    │   ├── Type: 2nd-order IIR (Butterworth, Q=0.7071)
    │   ├── Cutoff: 5000 Hz
    │   ├── Operating Rate: 2 MHz (filter runs at input rate!)
    │   ├── Instances: g_detector_lowpass_i, g_detector_lowpass_q
    │   └── Purpose: Anti-alias before decimation
    │
    ├── Decimation: 40:1
    │   ├── Input Rate: 2,000,000 Hz
    │   ├── Output Rate: 50,000 Hz
    │   ├── Counter: g_detector_decim_counter
    │   └── Method: Simple drop (keep every 40th sample)
    │
    └──► det_i, det_q @ 50 kHz
         │
         ├──► tick_detector_process_sample()
         └──► marker_detector_process_sample()
```

**Filter Analysis:**
- Cutoff 5 kHz at 2 MHz input → normalized freq = 5000/2000000 = 0.0025
- This is VERY low - the filter is working at an extreme edge
- Effective -3dB point is correct, but transition band is very wide at this ratio
- 50 kHz output Nyquist = 25 kHz, signal content within ±5 kHz → safe

### DISPLAY PATH

```
Purpose: High-resolution waterfall + tone tracking

i_raw, q_raw (2 MHz)
    │
    ├── Biquad Lowpass Filter
    │   ├── Type: 2nd-order IIR (Butterworth, Q=0.7071)
    │   ├── Cutoff: 6000 Hz
    │   ├── Operating Rate: 2 MHz
    │   ├── Instances: g_display_lowpass_i, g_display_lowpass_q
    │   └── Purpose: Anti-alias before decimation
    │
    ├── Decimation: 166:1 (2M/12k = 166.67, truncated)
    │   ├── Input Rate: 2,000,000 Hz
    │   ├── Output Rate: 12,000 Hz (approximate)
    │   ├── Counter: g_display_decim_counter
    │   └── Method: Simple drop (keep every 166th sample)
    │
    └──► disp_i, disp_q @ 12 kHz
         │
         ├──► g_display_buffer (circular, 2048 samples)
         ├──► tone_tracker_process_sample() × 3 (carrier, 500Hz, 600Hz)
         │
         └── When 1024 new samples accumulated:
             └──► Display FFT triggered
```

**Filter Analysis:**
- Cutoff 6 kHz at 2 MHz input → normalized freq = 0.003
- Same concern: filter operates at extreme ratio
- 12 kHz output Nyquist = 6 kHz, signal content within ±6 kHz → marginal

---

## LOWPASS FILTER IMPLEMENTATION

```c
typedef struct {
    float x1, x2;       // Input history (z^-1, z^-2)
    float y1, y2;       // Output history
    float b0, b1, b2;   // Feedforward coefficients
    float a1, a2;       // Feedback coefficients (a0 normalized to 1)
} lowpass_t;

// Butterworth 2nd-order with Q = 0.7071 (maximally flat)
void lowpass_init(lowpass_t *lp, float cutoff_hz, float sample_rate) {
    float w0 = 2π × cutoff_hz / sample_rate;
    float alpha = sin(w0) / (2 × 0.7071);  // Q = 0.7071
    float cos_w0 = cos(w0);
    
    float a0 = 1 + alpha;
    lp->b0 = (1 - cos_w0) / 2 / a0;
    lp->b1 = (1 - cos_w0) / a0;
    lp->b2 = (1 - cos_w0) / 2 / a0;
    lp->a1 = -2 × cos_w0 / a0;
    lp->a2 = (1 - alpha) / a0;
}

// Direct Form I
float lowpass_process(lowpass_t *lp, float x) {
    float y = b0×x + b1×x1 + b2×x2 - a1×y1 - a2×y2;
    // Update history...
    return y;
}
```

**Coefficients at 5 kHz cutoff, 2 MHz sample rate:**
```
w0 = 2π × 5000 / 2000000 = 0.01571 rad
alpha = sin(0.01571) / 1.4142 = 0.01111
cos_w0 = 0.99988

a0 = 1.01111
b0 = 0.000006 / a0 ≈ 6.0e-6
b1 = 0.000012 / a0 ≈ 1.2e-5
b2 = 0.000006 / a0 ≈ 6.0e-6
a1 = -1.99976 / a0 ≈ -1.9778
a2 = 0.98889 / a0 ≈ 0.9780
```

**Issue:** Very small b coefficients mean the filter is barely affecting the signal per sample. This is mathematically correct but may have numerical precision concerns with float32.

---

## DECIMATION METHOD

```c
g_detector_decim_counter++;
if (g_detector_decim_counter >= g_detector_decimation) {
    g_detector_decim_counter = 0;
    // Process decimated sample
}
```

**Type:** Simple sample dropping (no averaging, no CIC)

**Pros:**
- Zero latency added
- Computationally trivial
- Preserves instantaneous values

**Cons:**
- Relies entirely on anti-alias filter
- No additional noise reduction
- Slight aliasing if filter rolloff insufficient

---

## SUMMARY: TIER 1 OUTPUTS

| Output | Rate | Bandwidth | Filter | Decimation | Consumers |
|--------|------|-----------|--------|------------|-----------|
| det_i, det_q | 50 kHz | ±5 kHz | 5 kHz LP | 40:1 | tick_detector, marker_detector |
| disp_i, disp_q | 12 kHz | ±6 kHz | 6 kHz LP | 166:1 | display FFT, tone_tracker ×3 |

---

## POTENTIAL ISSUES IDENTIFIED

1. **No S16 Normalization**
   - Raw int16 values used as-is
   - Energy values are in range 0 to ~1e9 instead of 0 to 1
   - Doesn't affect detection (relative), but confusing for debugging

2. **Filter/Decimation Ratio**
   - 2 MHz → 50 kHz with 5 kHz cutoff: safe (10× margin)
   - 2 MHz → 12 kHz with 6 kHz cutoff: marginal (Nyquist exactly at cutoff)
   - Display path may have slight aliasing at band edges

3. **Single Biquad Stage**
   - Only -12 dB/octave rolloff
   - For 40:1 decimation, should ideally have steeper filter
   - Works because WWV signal is narrowband (most energy within ±2 kHz)

4. **Independent Filter Paths**
   - Two separate filters processing same input
   - Doubles CPU for filtering (could share if needed)
   - BUT: Different cutoffs, so legitimate design choice

---

## NEXT TIER: What happens to det_i, det_q?

The 50 kHz detector path feeds:
- `tick_detector.c`: 256-pt FFT (5.12 ms frames)
- `marker_detector.c`: 256-pt FFT (5.12 ms frames)

Both run independent FFTs on the same samples.
