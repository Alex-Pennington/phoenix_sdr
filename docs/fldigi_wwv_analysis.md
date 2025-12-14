# FLDIGI WWV Implementation Analysis

## Executive Summary

After researching FLDIGI's WWV implementation, I've discovered that **FLDIGI's WWV mode is NOT an automatic tick detection system**. It's a **visual sound card calibration tool** that displays the tick as a FAX-like scan line and relies on human observation to detect sample rate errors.

This means there's no sophisticated tick detection algorithm to port. However, the signal processing approach they use provides valuable insights for our implementation.

## Implementation Progress (2025-12-13)

### What Was Implemented

**Edge Detection Approach** in `wwv_scan.c`:
1. **Moving Average Filter** (48 samples = 1ms at 48kHz)
   - Smooths envelope to preserve tick edges while reducing noise
   - `ma_process()` function with circular buffer

2. **Derivative-Based Edge Detection**
   ```c
   float derivative = envelope_smoothed - g_prev_envelope;
   if (derivative > g_edge_threshold && envelope_smoothed > g_noise_floor * 3.0f) {
       // Found rising edge - record position
       g_tick_sample_pos = adjusted_pos;
       g_tick_detected = true;
   }
   ```

3. **Noise Floor Estimation**
   - Updated from samples in 200-800ms window (between ticks)
   - Slow IIR filter: `g_noise_floor = 0.999f * g_noise_floor + 0.001f * envelope_smoothed`

4. **Display Shows Edge Position**
   - `E` = edge-detected position
   - `#` = max energy bucket (for comparison)
   - Shows tick position in milliseconds

5. **Threshold Adjustment Keys**
   - `[` = decrease threshold (more sensitive)
   - `]` = increase threshold (less sensitive)
   - Range: 1e-7 to 1e-3

### Bugs Found and Fixed

| Bug | Symptom | Root Cause | Fix |
|-----|---------|------------|-----|
| Display copy race | `mx0.0000` always | Values reset before display read them | Added `_display` copy variables, save before reset |
| Printf format | `mx0.0000` for valid data | `%.4f` rounds `0.000007` to `0.0000` | Changed to `%.2e` (scientific notation) |
| Wrong threshold scale | Never triggers | Default `0.002` but derivatives ~1e-5 | Changed default to `5e-6` |

### Current Diagnostic Output

Added comprehensive diagnostics to understand edge detection failures:
```
15.0MHz G G40 O-440 T5e-06 ||#------------------| s05   NO-EDGE mx1.23e-05 H 0% C 0%
  [EDGE-DBG] maxD@250ms env=5.00e-04 nf=1.00e-03 nf*3=3.00e-03 thresh=5.00e-06 | deriv:PASS env:FAIL win:FAIL
```

This shows:
- `maxD@250ms` - where max derivative occurred (should be 0-100ms for tick window)
- `env=` - envelope value when max derivative occurred  
- `nf=` - noise floor estimate
- `nf*3=` - envelope threshold (envelope must exceed this)
- `deriv:PASS/FAIL` - did derivative exceed threshold?
- `env:PASS/FAIL` - did envelope exceed noise_floor * 3?
- `win:PASS/FAIL` - was max derivative in first 100ms?

### Current Blocking Issue

**Build system not rebuilding `wwv_scan.c`** - running `build.ps1 -Target tools` only builds `wwv_tick_detect` and `wwv_tick_detect2`, skipping other tools including `wwv_scan`. 

To fix:
```powershell
Remove-Item .\build\wwv_scan.o -ErrorAction SilentlyContinue
.\build.ps1 -Target tools
.\bin\wwv_scan.exe -i
```

### Next Steps (Once Build Works)

1. **Run diagnostics** - see which condition is blocking edge detection
2. **Likely issues to investigate**:
   - Noise floor may be too high (envelope < nf*3)
   - Max derivative may occur outside tick window (200-800ms instead of 0-100ms)
   - Noise floor may need initialization or different update rate
3. **Adjust algorithm** based on diagnostic findings

---

## FLDIGI WWV Signal Flow

```
Audio Input (44.1/48 kHz native)
         │
         ▼
┌─────────────────────────────────┐
│  Bandpass Filter (around 1kHz)  │  ← Isolate tick tone
└─────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────┐
│  Decimation to 1000 Hz          │  ← ~48:1 reduction
└─────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────┐
│  Power Detection (envelope²)    │  ← AM demodulation
└─────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────┐
│  Moving Average Filter          │  ← Smooth for edge detection
└─────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────┐
│  FAX-style Display              │  ← 1 scan line = 1 second
│  (Human identifies tick slope)  │
└─────────────────────────────────┘
```

## Key Technical Details from Documentation

### Sample Rate and Decimation
- Native sound card: 44100 or 48000 Hz
- Decimated to: **1000 Hz** (1000 samples per second)
- This gives 1ms resolution for tick timing
- Decimation factor: ~44-48x

### Detection Method
From FLDIGI documentation:
> "The sampled signal is filtered and reduced to a sample rate of 1000 by a process called decimation in time. The resulting signal is then power detected and low pass filtered with a filter called a **moving average filter**. The moving average is very good at detecting the edge of a pulse such as the 1 second tick transmitted by WWV."

### Display Method
- FAX-style scan: Each horizontal line = 1 second of data
- Tick appears as **bright white vertical line**
- Sample rate error shows as **slope** in the line
- User manually adjusts PPM until line is perfectly vertical

### Calibration Procedure
1. Tune to WWV on AM mode
2. Select WWV modem
3. Open floating scope (displays the FAX-style output)
4. Observe slope of tick line
5. Adjust Rx PPM setting until line is vertical
6. +1000 ppm → observe slope direction
7. -1000 ppm → observe opposite slope
8. Interpolate to find zero-slope value

## What FLDIGI Does NOT Do

1. **No automatic tick detection** - Human identifies the tick visually
2. **No autonomous timing discipline** - Just displays for calibration
3. **No threshold-based detection** - No SNR calculation for ticks
4. **No phase tracking/PLL** - No feedback loop
5. **No minute marker handling** - Not relevant for calibration

## Relevance to Our Implementation

### What We Can Use
1. **Decimation target rate**: 1000 Hz is a good choice
   - 1ms resolution
   - Matches WWV tick duration (5ms)
   - Low computational load

2. **Moving average filter**: Good for edge detection
   - Smooths noise
   - Preserves pulse edges
   - Simple to implement

3. **Power detection**: Envelope squared is correct
   - We're already doing this

### What We Need to Do Differently

Since FLDIGI relies on human visual observation, we need to build our own **automatic detection and tracking** system. Here's a proposed approach:

## Proposed Phoenix Nest Detection Algorithm

### Stage 1: Decimation (Already Done)
```
2 MHz → 48 kHz (42x decimation via our polyphase filter)
```
This is actually better than FLDIGI's 44-48x since we start at 2 MHz.

### Stage 2: 1000 Hz Bandpass (Already Done)
```c
biquad_init_bp(&g_bp_1000hz, 48000.0f, 1000.0f, Q);
```
Consider increasing Q for narrower bandwidth.

### Stage 3: Add Moving Average (NEW)
```c
#define MA_LENGTH 48  // 1ms at 48kHz

float moving_average_process(float *buffer, int *index, float new_sample) {
    buffer[*index] = new_sample;
    *index = (*index + 1) % MA_LENGTH;
    
    float sum = 0;
    for (int i = 0; i < MA_LENGTH; i++) {
        sum += buffer[i];
    }
    return sum / MA_LENGTH;
}
```

### Stage 4: Edge Detection (NEW)
Instead of buckets, detect the **rising edge** of the tick:
```c
// Detect rising edge using derivative
float derivative = current_envelope - previous_envelope;

if (derivative > threshold && current_envelope > noise_floor * 3) {
    // Rising edge detected - record sample position
    tick_position = g_samples_in_second;
}
```

### Stage 5: Phase Tracking (NEW)
Use detected tick position to discipline timing:
```c
// Expected tick at sample 0
// If detected at sample N, we're N samples late
int error_samples = tick_position;

// Wrap for early detection
if (error_samples > SAMPLES_PER_SECOND / 2) {
    error_samples -= SAMPLES_PER_SECOND;
}

// Apply P-controller correction (25%)
int correction = -error_samples / 4;
g_resync_value += correction;
```

## Alternative: Matched Filter Approach

For better SNR in noisy conditions, consider a **matched filter**:

```c
// WWV tick is 5ms of 1000 Hz tone
// Create template of expected tick waveform
float tick_template[240];  // 5ms at 48kHz

for (int i = 0; i < 240; i++) {
    tick_template[i] = sinf(2 * M_PI * 1000 * i / 48000.0f);
}

// Cross-correlate with incoming signal
float correlation = 0;
for (int i = 0; i < 240; i++) {
    correlation += signal[i] * tick_template[i];
}

// Peak correlation indicates tick position
```

This provides **optimal detection in Gaussian noise** and is used in many timing recovery systems.

## Implementation Plan

### Phase 1: Add Moving Average (Quick Fix)
1. Add MA filter after bandpass
2. Test if hit rate improves
3. ~20 lines of code

### Phase 2: Edge Detection
1. Replace bucket system with edge detector
2. Record sample position of rising edge
3. Use for discipline feedback
4. ~50 lines of code

### Phase 3: Matched Filter (If Needed)
1. Implement cross-correlation
2. Use peak position for timing
3. More computation but better SNR
4. ~100 lines of code

## Recommended Next Steps

1. **Quick test**: Add 48-sample moving average to current pipeline
2. **Measure improvement**: Check if hit rate increases
3. **If still <70%**: Implement edge detection
4. **If still <90%**: Implement matched filter

## Key Insight

FLDIGI's success comes from **human visual processing** which is extremely good at pattern recognition in noise. For autonomous operation, we need to replicate this with:
- Averaging (MA filter)
- Template matching (matched filter)
- Feedback loop (PLL-style correction)

The bucket approach we're using is a crude approximation that loses information. Edge detection or matched filtering would be more effective.

## Our Current Problem

Looking at our implementation, the issue is clear:

**We're using 50ms buckets** (20 per second) which gives poor resolution:
- WWV tick is 5ms
- Bucket is 50ms (10x wider than tick!)
- Tick energy gets diluted across the bucket

**FLDIGI uses 1ms resolution** (1000 samples/sec):
- Tick spans 5 samples
- Much more precise localization
- Moving average smooths without losing position

## Concrete Fix: Increase Bucket Resolution

Quick improvement without changing architecture:
```c
// Instead of 20 buckets × 50ms = 1000ms
#define NUM_BUCKETS 100  // 100 buckets × 10ms = 1000ms
#define BUCKET_MS   10   // 10ms per bucket

// Or even better:
#define NUM_BUCKETS 200  // 200 buckets × 5ms = 1000ms  
#define BUCKET_MS   5    // 5ms matches tick width exactly!
```

This alone should significantly improve detection accuracy.

## References

- FLDIGI Manual: http://www.w1hkj.com/FldigiHelp/digiscope_display_wwv_mode.html
- Source code: https://sourceforge.net/p/fldigi/fldigi/ci/master/tree/src/wwv/
- Wikipedia: https://en.wikipedia.org/wiki/Fldigi
