# SDR Waterfall Display and AM Demodulation

## Overview

This document describes the proper methods for processing I/Q data from a Software Defined Radio (SDR) to create waterfall spectrum displays and perform AM demodulation. These are two distinct operations with different processing chains.

---

## Part 1: I/Q Signal Fundamentals

### What I/Q Data Represents

SDRs using direct-conversion (Zero-IF) architecture produce two output streams:
- **I (In-phase)**: The signal mixed with a local oscillator at the tuned frequency
- **Q (Quadrature)**: The signal mixed with the same oscillator, but phase-shifted by 90°

Together, I and Q form a **complex baseband signal**:

$$s(t) = I(t) + jQ(t)$$

This complex signal is also called an **analytic signal**. It has a crucial property: it contains no negative frequency components in its original RF form. When converted to baseband:
- **DC (0 Hz)** = The carrier frequency you tuned to
- **Positive frequencies** = RF signals above the tuned frequency
- **Negative frequencies** = RF signals below the tuned frequency

### Why Two Components?

A single real-valued signal cannot distinguish between frequencies above and below the carrier—they would alias onto each other. The quadrature (90° shifted) component provides the phase information needed to distinguish them.

Example: If you tune to 15.000 MHz:
- A signal at 15.001 MHz appears at +1000 Hz in baseband
- A signal at 14.999 MHz appears at -1000 Hz in baseband
- With only I (real), both would appear at 1000 Hz and be indistinguishable

### DC Offset Issue

Direct-conversion receivers are known to have DC offset problems. The local oscillator can leak into the signal path, creating a spurious signal right at DC. This is why many SDR displays show a spike at the center frequency, and why DC blocking filters are commonly used.

---

## Part 2: Waterfall Spectrum Display

### Purpose

A waterfall display shows the **RF spectrum** around the tuned frequency over time:
- **X-axis**: Frequency (typically centered on the tuned frequency)
- **Y-axis**: Time (scrolling downward)
- **Color**: Signal magnitude/power at each frequency

### Correct Processing Chain

```
┌─────────────────────────────────────────────────────────────────┐
│                    SDR WATERFALL DISPLAY                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  I/Q from SDR ──► Lowpass Filter I ──┬──► Decimate I ──┐        │
│      (2 MHz)     Lowpass Filter Q ──┘    Decimate Q ──┤        │
│                                                        │        │
│                                                        ▼        │
│                                              ┌─────────────┐    │
│                                              │ Complex FFT │    │
│                                              │  I + jQ     │    │
│                                              └──────┬──────┘    │
│                                                     │           │
│                                                     ▼           │
│                                              ┌─────────────┐    │
│                                              │  FFT Shift  │    │
│                                              │ (DC→center) │    │
│                                              └──────┬──────┘    │
│                                                     │           │
│                                                     ▼           │
│                                              ┌─────────────┐    │
│                                              │ |bin|² or   │    │
│                                              │ 20log₁₀|bin││    │
│                                              └──────┬──────┘    │
│                                                     │           │
│                                                     ▼           │
│                                              Color mapping      │
│                                              & display          │
└─────────────────────────────────────────────────────────────────┘
```

### Key Steps Explained

#### 1. Lowpass Filtering (Anti-Alias)
Before decimation, lowpass filter both I and Q channels to prevent aliasing. The cutoff frequency should be less than half the target sample rate.

```c
// Filter BOTH components separately
float i_filtered = lowpass_process(&lpf_i, i_raw);
float q_filtered = lowpass_process(&lpf_q, q_raw);
```

#### 2. Decimation
Reduce sample rate while preserving the complex nature of the signal. **Keep I and Q separate**—do not combine them yet.

```c
// Decimate BOTH components
if (++decim_counter >= decim_factor) {
    decim_counter = 0;
    i_decimated[idx] = i_filtered;
    q_decimated[idx] = q_filtered;
    idx++;
}
```

#### 3. Complex FFT
Feed the complex signal (I + jQ) to the FFT. Most FFT libraries support complex input directly.

```c
// Pack into complex FFT input
for (int i = 0; i < FFT_SIZE; i++) {
    fft_in[i].r = i_decimated[i] * window[i];  // I = real
    fft_in[i].i = q_decimated[i] * window[i];  // Q = imaginary
}
kiss_fft(cfg, fft_in, fft_out);
```

#### 4. FFT Shift
The FFT output has DC at bin 0, positive frequencies in bins 1 to N/2-1, and negative frequencies in bins N/2 to N-1. To display with DC in the center:

```
Before shift: [DC, +1, +2, ... +N/2-1, -N/2, -N/2+1, ... -1]
After shift:  [-N/2, -N/2+1, ... -1, DC, +1, +2, ... +N/2-1]
```

```c
// Map frequency to FFT bin (with DC at center of display)
float freq = (pixel / width - 0.5f) * 2.0f * max_freq;  // -max to +max
int bin;
if (freq >= 0) {
    bin = (int)(freq / bin_hz);           // Positive: bins 0 to N/2-1
} else {
    bin = FFT_SIZE + (int)(freq / bin_hz); // Negative: bins N/2 to N-1
}
```

#### 5. Magnitude Calculation
For spectrum display, compute the magnitude (or power) of each complex bin:

$$\text{magnitude} = \sqrt{\text{re}^2 + \text{im}^2}$$

$$\text{power (dB)} = 10 \log_{10}(\text{re}^2 + \text{im}^2) = 20 \log_{10}(\text{magnitude})$$

### What You See

With a proper complex FFT waterfall tuned to WWV at 15 MHz:
- **DC (center)**: The carrier (strong constant signal)
- **±1000 Hz**: The tick sidebands (appear during the 5ms pulse each second)
- **±500 Hz, ±600 Hz**: Other modulation tones
- **Noise floor**: Visible across the spectrum

---

## Part 3: AM Demodulation (Envelope Detection)

### Purpose

AM (Amplitude Modulation) encodes information in the **envelope** (amplitude) of the carrier. To recover the audio:

$$\text{envelope}(t) = |s(t)| = \sqrt{I(t)^2 + Q(t)^2}$$

This is called **envelope detection** or **magnitude detection**.

### Correct Processing Chain

```
┌─────────────────────────────────────────────────────────────────┐
│                      AM DEMODULATION                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  I/Q from SDR ──► Lowpass Filter I ──┬──► Envelope Detection    │
│      (2 MHz)     Lowpass Filter Q ──┘    √(I² + Q²)            │
│                                              │                  │
│                                              ▼                  │
│                                         Decimation              │
│                                         (to audio rate)         │
│                                              │                  │
│                                              ▼                  │
│                                         DC Blocking             │
│                                         (remove carrier)        │
│                                              │                  │
│                                              ▼                  │
│                                         Audio Output            │
│                                         (real signal)           │
└─────────────────────────────────────────────────────────────────┘
```

### Key Steps Explained

#### 1. Lowpass Filtering
Same as for waterfall—filter I and Q to the desired audio bandwidth before decimation.

#### 2. Envelope Detection
Compute the magnitude at **every sample** (before decimation):

```c
float envelope = sqrtf(i_filt * i_filt + q_filt * q_filt);
```

**Important**: Do this at the full sample rate, before decimation. The envelope itself is a real-valued signal that can then be decimated normally.

#### 3. Decimation
After envelope detection, you have a real (not complex) signal. Decimate to the target audio rate (e.g., 48 kHz).

#### 4. DC Blocking
The envelope contains a DC component equal to the carrier amplitude. For audio, remove this:

```c
float audio = dc_block_process(&dc_blocker, envelope);
```

#### 5. Output
The result is a real-valued audio signal containing the AM modulation (e.g., the 1000 Hz WWV tick tone).

### What You Get

For WWV, the demodulated audio contains:
- 1000 Hz tick pulses (5ms duration, once per second)
- 500 Hz and 600 Hz tones (continuous during parts of each minute)
- Voice announcements (at certain times)

---

## Part 4: Audio Spectrum Display (Real FFT)

### Purpose

If you want to display the **spectrum of the demodulated audio** (not the RF spectrum), you perform an FFT on the envelope signal.

### Processing Chain

```
┌─────────────────────────────────────────────────────────────────┐
│                   AUDIO SPECTRUM DISPLAY                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Demodulated Audio ──► Window Function ──► Real FFT             │
│  (from envelope)           (Hann, etc.)        │                │
│                                                ▼                │
│                                           Magnitude             │
│                                           |bin|                 │
│                                                │                │
│                                                ▼                │
│                                     Display bins 0 to N/2       │
│                                     (0 Hz to Nyquist)           │
└─────────────────────────────────────────────────────────────────┘
```

### Key Difference from RF Spectrum

For a **real-valued signal** (like demodulated audio), the FFT output has a special property:
- Bins 0 to N/2 contain the positive frequencies (0 Hz to Nyquist)
- Bins N/2+1 to N-1 are **complex conjugates** of the positive bins
- The negative frequency bins contain **no new information**

Therefore, for audio spectrum display:
- **Only display bins 0 to N/2**
- **DC is at the left edge**, not the center
- **No FFT shift needed**

```c
// For real signal FFT - only use positive frequencies
for (int x = 0; x < display_width; x++) {
    float freq = (float)x / display_width * (sample_rate / 2.0f);  // 0 to Nyquist
    int bin = (int)(freq / bin_hz);
    if (bin >= FFT_SIZE / 2) bin = FFT_SIZE / 2 - 1;

    float mag = sqrtf(fft_out[bin].r * fft_out[bin].r +
                      fft_out[bin].i * fft_out[bin].i);
    // ... display
}
```

### What You See

For demodulated WWV audio spectrum:
- **0 Hz**: DC component (should be near zero after DC blocking)
- **500 Hz**: 500 Hz tone (when active)
- **600 Hz**: 600 Hz tone (when active)
- **1000 Hz**: Tick tone (during 5ms pulse each second)
- **Harmonics**: Possibly visible at 2000 Hz, 3000 Hz, etc.

---

## Part 5: Comparison Summary

| Aspect | RF Spectrum (Complex FFT) | Audio Spectrum (Real FFT) |
|--------|---------------------------|---------------------------|
| **Input** | Complex I/Q | Real envelope |
| **FFT Type** | Complex FFT | Real FFT (or complex with Q=0) |
| **Frequency Range** | -Fs/2 to +Fs/2 | 0 to Fs/2 |
| **DC Location** | Center (after shift) | Left edge |
| **Negative Freqs** | Real information | Mirror of positive (redundant) |
| **Shows** | RF signals around tuned freq | Audio content after demodulation |
| **WWV 1000 Hz** | Two peaks at ±1000 Hz | One peak at 1000 Hz |

---

## Part 6: Common Mistakes

### Mistake 1: Envelope Before Complex FFT
```c
// WRONG for RF spectrum display
float mag = sqrtf(i*i + q*q);  // Lost phase information!
fft_in[n].r = mag;
fft_in[n].i = 0;  // This is now a real FFT, not complex
```

This destroys the frequency sign information. You can no longer distinguish +1000 Hz from -1000 Hz.

### Mistake 2: FFT Shift on Real Signal
```c
// WRONG for audio spectrum
// Displaying negative frequencies for a real signal shows redundant data
float freq = (pixel / width - 0.5f) * sample_rate;  // -Fs/2 to +Fs/2
```

For real signals, negative frequencies are mirrors of positive. Showing both wastes half the display on redundant data.

### Mistake 3: Decimating Before Envelope
```c
// PROBLEMATIC
if (++counter >= decim_factor) {
    float mag = sqrtf(i*i + q*q);  // Only computed on decimated samples
}
```

This can cause aliasing of the envelope. The envelope should be computed at full rate, then decimated.

### Mistake 4: Forgetting DC Block for Audio
```c
// WRONG for audio output
float audio = sqrtf(i*i + q*q);  // Contains large DC offset!
output(audio);  // Will clip or be inaudible
```

The envelope has a DC component equal to the carrier amplitude. For audio, you must remove it.

---

## Part 7: Implementation Recommendations

### For Tick Detection (WWV)

**Recommended approach**: Use the demodulated audio path with a bandpass filter around 1000 Hz.

```c
// 1. Envelope detection at full rate
float envelope = sqrtf(i_filt*i_filt + q_filt*q_filt);

// 2. Decimate to audio rate
if (++counter >= decim_factor) {
    counter = 0;

    // 3. DC block
    float audio = dc_block_process(&dc_block, envelope);

    // 4. Bandpass around 1000 Hz
    float tick_signal = bandpass_1000hz_process(&bp, audio);

    // 5. Energy detection
    tick_energy += tick_signal * tick_signal;
}
```

### For RF Spectrum Waterfall

**Recommended approach**: Keep I/Q separate, complex FFT, proper bin mapping.

```c
// 1. Keep I and Q separate through decimation
if (++counter >= decim_factor) {
    counter = 0;
    iq_buffer[idx].i = i_filt;
    iq_buffer[idx].q = q_filt;
    idx++;
}

// 2. When buffer full, complex FFT
for (int n = 0; n < FFT_SIZE; n++) {
    fft_in[n].r = iq_buffer[n].i * window[n];  // I = real
    fft_in[n].i = iq_buffer[n].q * window[n];  // Q = imaginary
}
kiss_fft(cfg, fft_in, fft_out);

// 3. FFT shift for display (DC in center)
for (int x = 0; x < width; x++) {
    float freq = (x / (float)width - 0.5f) * sample_rate;
    int bin = freq >= 0 ? (int)(freq/bin_hz) : FFT_SIZE + (int)(freq/bin_hz);
    // ... compute magnitude and display
}
```

---

## References

1. Wikipedia: [Spectrogram](https://en.wikipedia.org/wiki/Spectrogram) - "computed the squared magnitude of the short-time Fourier transform (STFT)"
2. Wikipedia: [Direct-conversion receiver](https://en.wikipedia.org/wiki/Direct-conversion_receiver) - Zero-IF architecture and I/Q quadrature
3. Wikipedia: [In-phase and quadrature components](https://en.wikipedia.org/wiki/In-phase_and_quadrature_components) - "I/Q data is used to represent the modulations of some carrier"
4. Wikipedia: [Analytic signal](https://en.wikipedia.org/wiki/Analytic_signal) - "instantaneous amplitude or envelope = |s_a(t)|"

---

*Document created: December 15, 2025*
*Project: Phoenix SDR*
