# Phoenix Nest Modem — I/Q Input Interface Design

## Overview

This document describes how to extend the Phoenix Nest MIL-STD-188-110A modem to accept raw I/Q samples from SDR sources (specifically SDRplay RSP2 Pro via phoenix_sdr) in addition to existing 48kHz audio input.

**Goal:** Accept complex I/Q samples directly, eliminating the Hilbert transform reconstruction currently needed for audio input.

---

## Current Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     CURRENT RX PATH                         │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Audio Device ──► 48kHz Real Samples ──► Hilbert Transform  │
│       or                                        │           │
│  PCM File                                       ▼           │
│                                         Complex Baseband    │
│                                                │            │
│                                                ▼            │
│                                    Demodulator/Equalizer    │
│                                                │            │
│                                                ▼            │
│                                          Data Output        │
└─────────────────────────────────────────────────────────────┘
```

**Problem:** The Hilbert transform reconstructs the complex signal that the SDR already had before SSB demodulation. We're throwing away information and then spending CPU cycles to approximate it back.

---

## Proposed Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     NEW RX PATH                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────┐      ┌─────────────┐                       │
│  │ Audio Input │      │  I/Q Input  │                       │
│  │ (48kHz real)│      │ (SDR/file)  │                       │
│  └──────┬──────┘      └──────┬──────┘                       │
│         │                    │                              │
│         ▼                    ▼                              │
│  ┌─────────────┐      ┌─────────────┐                       │
│  │  Hilbert    │      │  Decimate   │                       │
│  │  Transform  │      │  Resample   │                       │
│  └──────┬──────┘      └──────┬──────┘                       │
│         │                    │                              │
│         └────────┬───────────┘                              │
│                  ▼                                          │
│         ┌───────────────┐                                   │
│         │    Complex    │  ◄── Both paths produce this      │
│         │   Baseband    │      (48kHz std::complex<float>)  │
│         │   48kHz       │                                   │
│         └───────┬───────┘                                   │
│                 ▼                                           │
│         Demodulator/Equalizer (unchanged)                   │
│                 ▼                                           │
│            Data Output                                      │
└─────────────────────────────────────────────────────────────┘
```

**Key insight:** Everything downstream of "Complex Baseband 48kHz" stays exactly the same. We're only adding an alternative front-end.

---

## Interface Specification

### Abstract Input Interface

```cpp
// sample_source.h

#pragma once
#include <complex>
#include <cstddef>

namespace m110a {

/**
 * Abstract sample source for demodulator input.
 * 
 * All implementations deliver complex float samples at 48kHz.
 * The demodulator doesn't know or care about the upstream source.
 */
class SampleSource {
public:
    virtual ~SampleSource() = default;
    
    /**
     * Read complex baseband samples.
     * 
     * @param out     Buffer to receive samples
     * @param count   Maximum samples to read
     * @return        Actual samples read (0 = EOF or no data available)
     */
    virtual size_t read(std::complex<float>* out, size_t count) = 0;
    
    /**
     * Get output sample rate (always 48000 for this modem).
     */
    virtual double sample_rate() const { return 48000.0; }
    
    /**
     * Check if source has more data available.
     */
    virtual bool has_data() const = 0;
    
    /**
     * Get source type for logging/debugging.
     */
    virtual const char* source_type() const = 0;
};

} // namespace m110a
```

### Audio Input Implementation (Existing Path, Wrapped)

```cpp
// audio_source.h

#pragma once
#include "sample_source.h"
#include <vector>

namespace m110a {

/**
 * Audio input source - wraps existing 48kHz real sample path.
 * Applies Hilbert transform to produce complex output.
 */
class AudioSource : public SampleSource {
public:
    /**
     * Construct from audio device or PCM file.
     * 
     * @param device_or_path  Audio device name or PCM file path
     */
    explicit AudioSource(const std::string& device_or_path);
    ~AudioSource() override;
    
    size_t read(std::complex<float>* out, size_t count) override;
    bool has_data() const override;
    const char* source_type() const override { return "audio"; }
    
private:
    // Existing audio handling code goes here
    // Hilbert transform state
    std::vector<float> hilbert_coeffs_;
    std::vector<float> delay_line_;
    // ... etc
};

} // namespace m110a
```

### I/Q Input Implementation (New Path)

```cpp
// iq_source.h

#pragma once
#include "sample_source.h"
#include <vector>
#include <cstdint>

namespace m110a {

/**
 * I/Q input source - accepts complex samples from SDR or file.
 * Handles format conversion, decimation, and resampling to 48kHz.
 */
class IQSource : public SampleSource {
public:
    /**
     * Input format specification
     */
    enum class Format {
        INT16_PLANAR,       // Separate int16_t I and Q arrays (phoenix_sdr native)
        INT16_INTERLEAVED,  // Interleaved int16_t I,Q,I,Q,... (.iqr files)
        FLOAT32_PLANAR,     // Separate float I and Q arrays
        FLOAT32_INTERLEAVED // Interleaved float I,Q,I,Q,... (GNU Radio)
    };
    
    /**
     * Construct I/Q source with specified input parameters.
     * 
     * @param input_rate_hz   Input sample rate (e.g., 2000000 for 2 MSPS)
     * @param format          Sample format
     * @param output_rate_hz  Target output rate (default 48000)
     */
    IQSource(double input_rate_hz, Format format, double output_rate_hz = 48000.0);
    ~IQSource() override;
    
    /**
     * Push raw I/Q samples into the source (streaming mode).
     * Called from SDR callback or file reader.
     * 
     * For planar formats:
     * @param xi    I (real) samples
     * @param xq    Q (imaginary) samples  
     * @param count Number of samples
     */
    void push_samples_planar(const int16_t* xi, const int16_t* xq, size_t count);
    void push_samples_planar(const float* xi, const float* xq, size_t count);
    
    /**
     * Push raw I/Q samples (interleaved mode).
     * @param iq    Interleaved I,Q,I,Q,... samples
     * @param count Number of sample PAIRS
     */
    void push_samples_interleaved(const int16_t* iq, size_t count);
    void push_samples_interleaved(const float* iq, size_t count);
    
    // SampleSource interface
    size_t read(std::complex<float>* out, size_t count) override;
    bool has_data() const override;
    const char* source_type() const override { return "iq"; }
    
    /**
     * Get current center frequency (for display/logging).
     * Set via set_metadata().
     */
    double center_frequency() const { return center_freq_hz_; }
    
    /**
     * Set metadata from SDR (optional, for logging).
     */
    void set_metadata(double center_freq_hz, double bandwidth_hz);
    
private:
    // Configuration
    double input_rate_hz_;
    double output_rate_hz_;
    Format format_;
    double center_freq_hz_ = 0.0;
    double bandwidth_hz_ = 0.0;
    
    // Decimation/resampling state
    std::vector<float> decim_filter_coeffs_;
    std::vector<std::complex<float>> decim_state_;
    int decimation_factor_;
    
    // Polyphase resampler for final rate adjustment
    // (if decimated rate != exactly 48kHz)
    
    // Output buffer (decimated, resampled, ready for modem)
    std::vector<std::complex<float>> output_buffer_;
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
    
    // Internal methods
    void process_input_buffer();
    void decimate(const std::complex<float>* in, size_t count);
};

} // namespace m110a
```

---

## Signal Flow Details

### From phoenix_sdr to Modem

```
phoenix_sdr                          IQSource                         Demod
    │                                    │                               │
    │  on_samples(xi, xq, count)         │                               │
    ├───────────────────────────────────►│                               │
    │  (int16_t planar, 2 MSPS)          │                               │
    │                                    │                               │
    │                          ┌─────────┴─────────┐                     │
    │                          │ 1. Convert to     │                     │
    │                          │    complex<float> │                     │
    │                          │ 2. Scale to ±1.0  │                     │
    │                          │ 3. Decimate 2M→48k│                     │
    │                          │ 4. Buffer output  │                     │
    │                          └─────────┬─────────┘                     │
    │                                    │                               │
    │                                    │  read(buf, count)             │
    │                                    │◄──────────────────────────────┤
    │                                    │                               │
    │                                    │  complex<float> @ 48kHz       │
    │                                    ├──────────────────────────────►│
    │                                    │                               │
```

### Decimation Strategy

Input: 2,000,000 Hz
Output: 48,000 Hz
Ratio: 41.667:1

**Recommended approach:** Multi-stage decimation

```
Stage 1: 2,000,000 → 250,000 Hz  (decimate by 8)
Stage 2:   250,000 →  50,000 Hz  (decimate by 5)
Stage 3:    50,000 →  48,000 Hz  (polyphase resample 48/50)
```

Each decimation stage:
1. Apply lowpass FIR filter (cutoff = output_rate / 2)
2. Keep every Nth sample

**Alternative:** If we adjust SDR sample rate to 1,920,000 Hz:
```
1,920,000 → 48,000 Hz  (decimate by 40, exact integer ratio)
```

This is cleaner but requires SDR rate flexibility.

---

## Integration Points

### Server Startup

Modify `main.cpp` to accept I/Q source specification:

```cpp
// New command line options:
//   --iq-input <source>     Enable I/Q input mode
//   --iq-rate <hz>          Input sample rate (default: 2000000)
//   --iq-format <fmt>       Format: int16p, int16i, float32p, float32i

// Example:
//   m110a_server --iq-input tcp://localhost:5000 --iq-rate 2000000
//   m110a_server --iq-input file://capture.iqr
//   m110a_server --iq-input sdr://rsp2
```

### Source Selection

```cpp
std::unique_ptr<SampleSource> create_source(const ServerConfig& config) {
    if (config.iq_input.empty()) {
        // Legacy audio mode
        return std::make_unique<AudioSource>(config.audio_device);
    } else {
        // I/Q mode
        auto src = std::make_unique<IQSource>(
            config.iq_rate, 
            config.iq_format
        );
        // Connect to SDR, file, or TCP source based on URI
        // ...
        return src;
    }
}
```

### Demodulator Changes

**None required.** The demodulator already works with `std::complex<float>` at 48kHz. It just needs to receive samples from `SampleSource::read()` instead of directly from audio code.

---

## Data Flow Options

### Option A: Callback-Based (Real-time SDR)

phoenix_sdr runs in its own process/thread, calls back with samples:

```cpp
// In SDR callback thread:
void on_sdr_samples(const int16_t* xi, const int16_t* xq, uint32_t count, ...) {
    iq_source->push_samples_planar(xi, xq, count);
}

// In modem thread:
while (running) {
    size_t n = source->read(buffer, BLOCK_SIZE);
    if (n > 0) {
        demodulator->process(buffer, n);
    }
}
```

### Option B: File-Based (Offline Testing)

Read from .iqr recording:

```cpp
IQFileSource file_source("capture.iqr");  // Wraps iqr_reader

while (file_source.has_data()) {
    size_t n = file_source.read(buffer, BLOCK_SIZE);
    demodulator->process(buffer, n);
}
```

### Option C: TCP Stream (Remote SDR)

phoenix_sdr streams over TCP, modem receives:

```cpp
// phoenix_sdr side:
// Open TCP connection, send header with rate/format, stream samples

// Modem side:
TCPIQSource tcp_source("localhost", 5000);
// Reads TCP, parses header, feeds to IQSource
```

---

## Format Conversion Reference

### int16 Planar to complex<float>

```cpp
void convert_int16_planar(const int16_t* xi, const int16_t* xq,
                          std::complex<float>* out, size_t n) {
    constexpr float scale = 1.0f / 32768.0f;
    for (size_t i = 0; i < n; i++) {
        out[i] = {xi[i] * scale, xq[i] * scale};
    }
}
```

### int16 Interleaved to complex<float>

```cpp
void convert_int16_interleaved(const int16_t* iq,
                               std::complex<float>* out, size_t n) {
    constexpr float scale = 1.0f / 32768.0f;
    for (size_t i = 0; i < n; i++) {
        out[i] = {iq[2*i] * scale, iq[2*i + 1] * scale};
    }
}
```

### float32 Already in Range

```cpp
void convert_float32_planar(const float* xi, const float* xq,
                            std::complex<float>* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        out[i] = {xi[i], xq[i]};
    }
}
```

---

## Testing Strategy

### Unit Tests

1. **Format conversion** - Verify all format converters produce correct output
2. **Decimation** - Verify frequency content preserved, aliases rejected
3. **Resampling** - Verify 50kHz→48kHz produces correct rate
4. **Round-trip** - TX→IQSource→Demod should decode correctly

### Integration Tests

1. **Loopback** - Generate TX I/Q, feed directly to RX I/Q input, verify decode
2. **File playback** - Record .iqr from SDR, play back, verify consistent results
3. **brain_core compatibility** - Both modems should decode same I/Q recording identically

### OTA Tests

1. **Live SDR** - Receive real signals via RSP2 Pro, verify decode
2. **Comparison** - Same signal via audio path vs I/Q path, compare BER

---

## Implementation Phases

### Phase 1: Interface Skeleton
- [ ] Create `SampleSource` abstract interface
- [ ] Wrap existing audio code as `AudioSource`
- [ ] Verify modem still works with wrapped audio

### Phase 2: I/Q Source Core
- [ ] Implement `IQSource` with format conversion
- [ ] Implement multi-stage decimation
- [ ] Implement resampler (if needed)
- [ ] Unit test all conversions

### Phase 3: File Input
- [ ] Create `IQFileSource` wrapping iqr_reader
- [ ] Test with recordings from phoenix_sdr
- [ ] Verify decode of known test signals

### Phase 4: Live SDR
- [ ] Create SDR callback → IQSource bridge
- [ ] Test real-time streaming
- [ ] Verify no buffer overruns at 2 MSPS

### Phase 5: TCP Streaming (Optional)
- [ ] Define TCP protocol for I/Q streaming
- [ ] Implement in phoenix_sdr (server) and modem (client)
- [ ] Test remote SDR operation

---

## Appendix: phoenix_sdr Output Format

The phoenix_sdr library (in `phoenix_sdr` repository) outputs:

| Parameter | Value |
|-----------|-------|
| Sample Rate | 2,000,000 Hz (2 MSPS) |
| Format | int16_t planar (separate I and Q arrays) |
| Range | -32768 to +32767 |
| Bandwidth | 200 kHz (configurable) |
| Center Freq | User-specified |

**Callback signature:**
```c
void on_samples(
    const int16_t *xi,      // I samples
    const int16_t *xq,      // Q samples
    uint32_t count,         // Sample count
    bool reset,             // Buffer flush flag
    void *user_ctx
);
```

**.iqr file format:**
- 64-byte header (see iq_recorder.h)
- Interleaved int16 samples: I0,Q0,I1,Q1,...
- Little-endian

---

## Summary

Adding I/Q input is a front-end change only. The modem's demodulator, equalizer, and decoder are unchanged. We're adding an alternative path that:

1. Accepts complex samples directly (no Hilbert reconstruction)
2. Decimates from SDR rate to modem rate
3. Delivers `std::complex<float>` at 48kHz

Both audio and I/Q inputs coexist — the modem selects based on configuration.

---

*Document version: 1.0*  
*Date: 2024-12-12*  
*Author: Phoenix Nest Development*
