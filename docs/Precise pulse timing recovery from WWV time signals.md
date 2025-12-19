# Precise pulse timing recovery from WWV time signals

**The leading edge of WWV's audio tone defines the exact on-time marker**, and recovering precise timestamps requires systematic compensation for filter group delay, threshold crossing offsets, and integration latency. For a detector reporting "pulse ended at time T with duration D," the true start time is calculated by subtracting the known duration, filter delays, and threshold-dependent offsets from the detected timestamp. With proper calibration and averaging, sub-millisecond timing accuracy is achievable from WWV's 800ms minute marker despite propagation delays of 5-25ms.

## WWV's timing reference is the tone's leading edge

NIST's official specification is unambiguous: **the start of the audio tone pulse is the on-time marker**. For WWV (Fort Collins, Colorado), the minute marker is an **800ms burst of 1000 Hz** at 100% modulation, while WWVH (Kauai, Hawaii) uses 1200 Hz. Regular second markers are 5ms pulses—exactly 5 cycles at 1000 Hz for WWV.

The signal structure provides a critical design feature for edge detection: each second pulse is preceded by **10ms of silence** and followed by **25ms of silence**, creating a 40ms protected zone. This quiet gap before the tone makes leading-edge detection significantly easier, as there's no audio content that could confuse threshold-based detectors.

| Marker Type | Duration | Frequency (WWV) | On-Time Reference |
|-------------|----------|-----------------|-------------------|
| Regular second | 5 ms | 1000 Hz | Leading edge |
| Minute marker | 800 ms | 1000 Hz | Leading edge |
| Hour marker | 800 ms | 1500 Hz | Leading edge |

The 800ms minute marker provides the best timing reference because its extended duration enables matched filtering with high processing gain, and the distinct 1000/1500 Hz frequencies allow unambiguous identification.

## Envelope detection introduces predictable delays

Envelope detection extracts the amplitude modulation from WWV's AM signal, producing a baseband representation of the pulse shape. Three primary methods exist, each with distinct timing characteristics that must be compensated.

**Hilbert transform envelope detection** creates an analytic signal z(t) = x(t) + jH{x(t)}, where the envelope equals |z(t)|. This method provides the cleanest envelope estimate without carrier-frequency ripple. The timing delay equals the group delay of the Hilbert transformer implementation—for an N-tap FIR Hilbert filter, the delay is exactly **(N-1)/(2×Fs)** seconds. This constant, frequency-independent delay makes compensation straightforward.

**RC envelope detection** (diode rectifier with lowpass filter) introduces delay approximately equal to the RC time constant. The 10%-90% rise time is **2.2×RC**, and a 50% threshold crossing occurs at approximately **t_start + 0.69×RC** after the true pulse beginning. The threshold crossing time depends directly on where you set the detection threshold relative to the final amplitude.

**Square-law detection** squares the input signal before lowpass filtering, producing an output proportional to signal power. The delay characteristics depend entirely on the lowpass filter design, with the same group delay formulas applying to the smoothing filter.

For any envelope detector with a threshold set at fraction α of peak amplitude and rise time τ_rise:

```
T_threshold_offset = α × τ_rise    (linear rise)
T_threshold_offset = -τ_rise × ln(1-α)    (exponential rise)
```

## Matched filters peak at pulse end, not beginning

A matched filter maximizes signal-to-noise ratio for detecting a known waveform in noise, making it ideal for WWV's precisely specified tone bursts. However, its timing characteristics require careful understanding: **the matched filter output peaks when the end of the received pulse aligns with the filter**, not when the pulse begins.

For a rectangular pulse of duration T, the matched filter output is triangular, reaching peak amplitude (equal to the pulse energy **A²×T**) at time **t_start + T**. This means for WWV's 800ms minute marker, the matched filter peak occurs 800ms after the true on-time marker.

The back-calculation formula for matched filter detection is:

```
T_true_start = T_peak - T_pulse_duration - T_group_delay
```

For a symmetric N-tap FIR matched filter sampled at Fs, the group delay is **(N-1)/(2×Fs)**. A matched filter for the 800ms pulse at 8 kHz sampling would have approximately 6400 taps, contributing **399.9ms** of group delay. Combined with the 800ms pulse duration, the total offset from peak to true start is approximately **1.2 seconds**.

Alternative timing extraction methods from matched filter output include detecting the 50% threshold crossing on the rising edge (which occurs at approximately **t_start + T/2**) or finding the maximum derivative of the output (which aligns with pulse start). The derivative method can provide better leading-edge localization but amplifies noise.

## Energy accumulators integrate over the pulse duration

Energy accumulation computes running signal power over a sliding window: E[n] = Σ|x[n-k]|² for k=0 to W-1. The output begins rising immediately when the pulse arrives, reaches half maximum at **t_start + W/2**, and achieves full amplitude at **t_start + W**.

For a threshold set at fraction β of the expected steady-state energy with window length W:

```
T_true_start = T_trigger - β × W
```

The fundamental tradeoff is integration window length: **longer windows improve SNR (proportional to √W) but worsen timing resolution (uncertainty ≈ W/2)**. For the 800ms WWV minute marker, an optimal balance uses windows of **200-400ms**—long enough for good noise rejection but short enough for reasonable edge localization.

Energy detectors are particularly useful when signal amplitude varies due to fading, as they respond to total energy rather than instantaneous amplitude. However, they cannot distinguish between a strong short pulse and a weak long pulse with equal energy.

## Group delay compensation requires filter-specific analysis

Every filter in the signal processing chain introduces group delay that shifts detected timestamps. Linear-phase FIR filters provide the simplest compensation: the delay is exactly **(N-1)/(2×Fs)** seconds and is constant across all frequencies. This predictability makes FIR filters strongly preferred for timing-critical applications.

For IIR filters (Butterworth, Chebyshev, elliptic), group delay varies with frequency and must be evaluated at the signal's center frequency. Butterworth filters offer relatively flat delay across the passband, while Chebyshev and elliptic designs exhibit **3-4× more delay variation**, causing pulse distortion near cutoff frequencies.

| Filter Type | Group Delay Formula | Compensation Complexity |
|-------------|---------------------|------------------------|
| FIR (symmetric) | (N-1)/(2×Fs) | Subtract constant |
| Moving average | (L-1)/(2×Fs) | Subtract constant |
| CIC decimator | (R×M×N-1)/(2×Fs_in) | Subtract constant |
| Butterworth IIR | Varies with frequency | Evaluate at carrier |
| Bessel IIR | Nearly constant | Can use average |

For cascaded filter chains, **total group delay is the sum of individual delays** when expressed in consistent time units. In multirate systems with decimation, express each filter's delay in seconds (not samples) before summing, since sample periods change between stages.

Empirical verification uses cross-correlation between input and output: inject a known pulse, cross-correlate, and the correlation peak position indicates total system delay. This captures all delays including those from non-ideal filter implementations.

## Practical back-calculation from trailing edge detection

When detecting pulse end rather than beginning—often more reliable due to higher SNR at full amplitude—the calculation requires subtracting the known duration:

```
T_true_start = T_trailing - D_nominal - T_filter_delay + T_hysteresis_correction
```

Hysteresis in threshold comparators affects timing: a hysteresis band H causes the rising-edge trigger to be delayed by **H/(2×slew_rate)** and the falling-edge trigger to occur early by the same amount. For a detector with 10% hysteresis band and 5ms rise time, this correction is approximately **0.5ms**.

**Threshold hysteresis is essential for noise immunity** but must be characterized. The timing correction depends on signal slope at the threshold crossing—faster edges are less affected than slow edges.

**Note for energy-based detectors**: Phoenix SDR uses FFT energy detection rather than threshold-crossing comparators. The hysteresis constant in the code (0.7 ratio) prevents chattering but does not affect timestamp calculation. For energy-based FFT detectors, omit the hysteresis term: `T_true = T_trailing - D_known - T_filter_delay`.

## Complete timing compensation formula

The master equation for true pulse start time incorporates all error sources:

```
T_true = T_detected - T_filter_delay - T_threshold_offset - T_integration_delay 
         - T_propagation + T_hysteresis_correction ± T_quantization/2
```

For specific detector types, this simplifies to practical formulas:

**Envelope detector with threshold:**
```
T_true = T_detect - τ_filter - (α × τ_rise) - T_propagation
```

**Matched filter peak detection:**
```
T_true = T_peak - (N-1)/(2×Fs) - (D_pulse/2) - T_propagation
```

**Energy accumulator trigger:**
```
T_true = T_trigger - (β × T_window) - T_propagation
```

**Trailing edge with known duration:**
```
T_true = T_trailing - D_known - T_filter_delay + T_hysteresis - T_propagation
```

Propagation delay for HF skywave signals ranges from **5-25ms** depending on distance and ionospheric conditions. For a 1500km path via F2-layer reflection at 300km height, the path length is approximately 1616km, giving **5.4ms** propagation delay. This must be calibrated for each receiving location and can vary by several milliseconds with ionospheric conditions.

## Error budget and achievable accuracy

Quantization error at 8 kHz sampling is **±62.5μs**. Filter group delay, once characterized, contributes zero error—it's a known constant that's subtracted exactly. The dominant error sources are propagation variation (±2-10ms for multipath), threshold timing jitter (±1-2ms depending on SNR), and ionospheric variation.

| Error Source | Typical Magnitude | Reducible By |
|--------------|-------------------|--------------|
| Propagation | 5-25 ms | Path calibration |
| Multipath variation | ±2-10 ms | Averaging |
| Threshold jitter | ±1-2 ms | Higher SNR |
| Sample quantization | ±0.0625 ms | Higher sample rate |

Averaging multiple pulses reduces random errors by **1/√N**. Weighting measurements by SNR² provides optimal combination. With 60 seconds of averaging at good SNR, **sub-millisecond accuracy is achievable** after calibrating the propagation path.

The NTP WWV driver implementation demonstrates these principles: it uses matched filters for both the 800ms minute pulse and 5ms second pulses, applies maximum-likelihood decoding, and requires SNR thresholds before accepting timing. The driver provides calibration parameters (fudge time1, time2) for site-specific propagation delay adjustment.

## Phoenix SDR reference implementation

Phoenix SDR implements the trailing-edge formula for energy-based FFT detection:

```c
T_true = T_trailing - D_known - T_filter_delay
```

Where:
- `T_trailing` = timestamp when FFT energy drops below threshold (milliseconds)
- `D_known` = measured pulse duration (milliseconds)
- `T_filter_delay` = 3.0 ms total group delay:
  - 0.32 ms: 2nd order Butterworth IIR lowpass (fc=5kHz, evaluated at f=1kHz)
  - 2.55 ms: Hann window on 256-point FFT (fs=50kHz)
  - 0.13 ms: 3-stage decimation FIR cascade (2MHz → 50kHz)

Hysteresis correction is **not applicable** because Phoenix uses energy-based detection, not threshold-crossing edge detection. The 0.7 hysteresis ratio in the code prevents state chattering but doesn't affect timestamp calculation.

This implementation achieves sub-millisecond timing accuracy for WWV minute marker synchronization, providing a precise epoch for the tick timing gate.

## Conclusion

Precise timing recovery from WWV requires understanding that the **leading edge is the defined reference**, that different detectors introduce different—but predictable—timing offsets, and that all filter delays must be compensated. Matched filters provide optimal detection sensitivity but peak at pulse end, requiring duration subtraction. Envelope detectors require threshold-dependent corrections. Energy accumulators require integration-time compensation.

The key insight is that **all delays are deterministic and compensable** once the detection system is characterized. Build your timing system with linear-phase FIR filters where possible for constant, frequency-independent group delay. Calibrate total system delay empirically using cross-correlation. Account for propagation delay based on your location. Average multiple pulses weighted by signal quality. With these practices, WWV's 800ms minute marker provides a robust timing reference accurate to better than 1ms.