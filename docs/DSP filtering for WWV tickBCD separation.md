# DSP filtering for WWV tick/BCD separation: solving the 1000 Hz harmonic collision

The WWV receiver problem you've encountered—false 497ms tick detections caused by the 100 Hz BCD subcarrier's 10th harmonic bleeding into your 1000 Hz tick detection band—has a well-documented solution. David Mills' NTP driver36 implementation, combined with NIST signal specifications, provides a proven architecture using **parallel filter banks, matched filtering, and timing-based gating** that achieves sub-millisecond accuracy even under marginal signal conditions.

The critical insight is that NIST designed the WWV signal format with this exact problem in mind: a **40ms protected zone** around each tick completely suppresses all modulation including the 100 Hz subcarrier, providing an interference-free detection window that your current implementation likely isn't exploiting.

## NIST built inherent separation into the signal format

The WWV broadcast format contains crucial timing relationships that enable clean tick/BCD separation. Understanding these specifications is essential before designing any filtering strategy.

**Modulation depth disparity provides amplitude headroom.** The 1000 Hz tick is broadcast at **100% modulation** while the 100 Hz BCD subcarrier operates at only **−10 to −15 dB** below this reference level (approximately 18-31% modulation). This 10+ dB amplitude advantage means the tick signal should dominate any harmonic energy from the subcarrier, providing roughly **3:1 voltage ratio** or **10:1 power ratio** separation before any filtering occurs.

**The protected zone eliminates interference at the critical moment.** NIST suppresses all modulation for exactly 40ms around each second marker: 10ms of silence preceding the tick, the 5ms tick pulse itself (5 cycles of 1000 Hz), followed by 25ms of silence. The BCD subcarrier's leading edge doesn't begin until **30ms after** the UTC second. This means during the tick detection window (approximately 10-15ms into each second), the 100 Hz subcarrier and all its harmonics are guaranteed absent.

**Timing structure enables gating-based separation:**

| Time offset from second | Signal present |
|------------------------|----------------|
| 0-10 ms | Silence (modulation suppressed) |
| 10-15 ms | **1000 Hz tick pulse only** |
| 15-30 ms | Silence (modulation suppressed) |
| 30-200/500/800 ms | 100 Hz BCD subcarrier active |
| Remainder of second | Varies by audio tone schedule |

The 5ms tick duration versus the 170-770ms BCD pulse duration provides another discrimination axis: any "tick" detection lasting hundreds of milliseconds cannot be a genuine tick.

## The NTP driver36 architecture solves this problem directly

The most thoroughly documented solution comes from David Mills' University of Delaware implementation, used in the NTP reference clock driver36. This architecture has operated successfully for decades under real-world propagation conditions.

**Parallel filter banks achieve frequency-domain separation.** The design splits the audio signal into two completely independent processing chains immediately after digitization at 8 kHz:

The **sync channel** uses a 4th-order elliptic IIR bandpass filter spanning **800-1400 Hz** (600 Hz bandwidth centered at 1100 Hz). This passband captures both the 1000 Hz WWV tick and 1200 Hz WWVH tick while rejecting the 100 Hz subcarrier and its harmonics up to 700 Hz (7th harmonic), plus providing substantial attenuation of the 8th and 9th harmonics before they enter the passband. The elliptic design achieves **0.2 dB passband ripple with −50 dB stopband attenuation**.

The **data channel** uses a 4th-order IIR lowpass filter with **150 Hz cutoff**, extracting only the 100 Hz subcarrier envelope while rejecting the 1000 Hz tick by approximately 40-50 dB.

```
Filter specifications (MATLAB syntax):
Sync BPF: ellip(4, 0.2, 50, [800 1400]/4000)
Data LPF: ellip(4, 0.2, 50, 150/4000)
```

**Matched filtering provides optimal SNR for tick detection.** After bandpass filtering, the tick signal passes through a **5ms FIR matched filter** designed to correlate with exactly 5 cycles of 1000 Hz (or 6 cycles of 1200 Hz for WWVH). This matched filter provides maximum signal-to-noise ratio for detecting the known tick waveform and strongly rejects signals that don't match this template—including any residual continuous-wave interference from BCD harmonics.

The matched filter achieves approximately **27 dB processing gain** for the tick signal. Any continuous harmonic energy from the BCD subcarrier, being a sustained rather than pulsed signal, will not correlate efficiently with the 5ms pulse template.

**Comb filtering exploits the 1-second periodicity.** An 8000-stage comb filter (at 8 kHz sample rate, this represents exactly one second) exponentially averages the matched filter output across multiple seconds. The time constant starts at 8 seconds and adaptively increases to 1024 seconds under stable conditions. This provides additional **processing gain exceeding 30 dB** while strongly rejecting non-periodic interference.

## Your specific problem likely stems from missing the gating window

Your symptom—497ms false tick detections—corresponds almost exactly to the 500ms BCD_ONE pulse duration (470ms after the 30ms suppression period). This strongly suggests your receiver is detecting BCD harmonic energy because it's **looking for ticks during the BCD transmission window** rather than only during the protected zone.

**Implementing timing-based gating eliminates the interference.** Once your receiver achieves initial second-epoch synchronization (which can be done via the minute marker or by detecting any tick), constrain tick detection to only the **5-25ms window** after each predicted second boundary. During this window:

- The genuine tick occurs at 10-15ms (5ms duration)
- The 100 Hz subcarrier is guaranteed silent
- Any detection is necessarily the tick or noise, never BCD harmonics

**Pulse width validation provides a secondary check.** Even without strict gating, rejecting any "tick" detection exceeding approximately 20ms duration would eliminate the 497ms false detections. The genuine tick is 5ms; even with filter ringing and AGC settling, no legitimate tick should exceed perhaps 15-20ms.

## Alternative DSP approaches trade complexity for performance

Several techniques from related time-signal systems offer different implementation tradeoffs:

**Goertzel algorithm for efficient dual-tone detection.** Rather than full-bandwidth filtering, Goertzel filters compute single-bin DFT magnitudes at 100 Hz and 1000 Hz with minimal computation. At 8 kHz sample rate, an N=80 Goertzel filter provides approximately 100 Hz resolution, enabling independent monitoring of both tone channels. This approach is particularly efficient on resource-constrained embedded systems and achieves only **~5% CPU utilization on ARM7-class processors**.

**Correlation-based template matching from WWVB and DCF77 receivers.** Cross-correlation with known pulse templates achieves optimal detection under noisy conditions. The WWVB decoder by jremington implements unweighted cross-correlation yielding correlation scores from +100 (perfect match) to -100 (perfect mismatch). For WWV, correlating against templates for the 5-cycle 1000 Hz tick, the 200ms BCD_ZERO envelope, the 500ms BCD_ONE envelope, and the 800ms marker envelope enables maximum-likelihood classification of each second's content.

**I/Q demodulation for phase-insensitive envelope detection.** The NTP driver36 uses dual 170ms synchronous matched filters in quadrature (I and Q channels) for BCD subcarrier demodulation. The I-channel provides envelope magnitude; the Q-channel feeds a Type-1 phase-locked loop that tracks subcarrier phase despite unpredictable radio receiver audio response at 100 Hz. This coherent detection approach rejects noise efficiently and enables the receiver to function even when the 100 Hz tone phase is distorted by the receiver's audio highpass filter.

## Implementation recommendations for your specific problem

Based on the research findings, here's a prioritized approach to eliminate your 497ms false tick detections:

**First priority: Implement timing-based gating.** Once you have any rough second-epoch estimate, gate your tick detection to only sample during the 5-30ms window after each predicted second. This single change will eliminate BCD harmonic interference entirely since the subcarrier is suppressed during this window. Use a relatively wide initial gate (perhaps 0-50ms) that narrows as epoch confidence increases.

**Second priority: Add pulse-width validation.** Reject any tick detection lasting more than 20ms. Your 497ms false detections are nearly 100x the expected 5ms duration—trivial to reject with a simple comparator.

**Third priority: Implement parallel filter banks.** If gating alone proves insufficient (perhaps due to propagation-induced timing variations), add the frequency-domain separation:

- Sync channel: 4th-order Butterworth or elliptic bandpass, 800-1400 Hz
- Data channel: 4th-order lowpass, 150 Hz cutoff
- Process tick detection only on the sync channel output

**Fourth priority: Add matched filtering.** For maximum robustness, implement a 5ms FIR matched filter correlating with 5 cycles of 1000 Hz sinusoid. This provides optimal detection under low SNR while rejecting continuous-wave interference. The matched filter output will show a sharp peak at the tick location rather than elevated levels throughout the BCD pulse.

**Fifth priority: Implement comb filtering for epoch tracking.** An 8000-stage comb filter (or equivalent at your sample rate) provides extraordinary processing gain by coherently averaging tick detections across many seconds. This enables reliable epoch detection even when individual ticks are buried in noise or interference.

## Documented filter specifications from working implementations

The following parameters come from the NTP driver36 and related implementations:

| Parameter | Value | Purpose |
|-----------|-------|---------|
| Sample rate | 8000 Hz | Standard audio, matches comb filter stages |
| Sync BPF passband | 800-1400 Hz | Captures 1000/1200 Hz ticks, rejects BCD |
| Sync BPF order | 4th-order elliptic | Sharp rolloff, −50 dB stopband |
| Data LPF cutoff | 150 Hz | Extracts 100 Hz subcarrier only |
| Tick matched filter | 5 ms FIR | Correlates with tick template |
| Minute matched filter | 800 ms synchronous | Detects minute marker |
| BCD matched filter | 170 ms I/Q pair | Coherent subcarrier demod |
| Comb filter stages | 8000 | One second at 8 kHz sample rate |
| Comb time constant | 8s → 1024s | Adaptive averaging period |

The minimum SNR requirements from Mills' analysis: **−21.1 dB** for second sync detection (with comb filter gain), **−9.3 dB** for BCD subcarrier demodulation. Under these marginal conditions, the system achieves timing accuracy within **125 μs**.

## Conclusion

The WWV 1000 Hz tick / 100 Hz BCD harmonic interference problem has been thoroughly solved in existing implementations. Your 497ms false tick detections indicate the receiver is processing during the BCD transmission window rather than exploiting the 40ms protected zone that NIST designed specifically to prevent this interference. 

Implementing timing-based gating—constraining tick detection to the 5-30ms window after each second—will eliminate the problem immediately. For additional robustness, add the NTP driver36's parallel filter architecture (800-1400 Hz bandpass for sync, 150 Hz lowpass for data) combined with matched filtering and comb filtering. This combination achieves sub-millisecond timing accuracy under propagation conditions where the signal is barely audible.