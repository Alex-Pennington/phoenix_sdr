# SDR Integration Analysis

## What the Modem Team Built (via Copilot on `SDRPlay_Integration` branch)

### New Files in `api/`:
1. **`sample_source.h`** - Abstract base class for sample input
2. **`iq_source.h`** - I/Q input source with decimation (2 MSPS → 48 kHz)
3. **`audio_source.h`** - Wrapper for existing audio path

### Key Interface (`SampleSource`):
```cpp
namespace m110a {
class SampleSource {
public:
    virtual size_t read(std::complex<float>* out, size_t count) = 0;
    virtual double sample_rate() const { return 48000.0; }
    virtual bool has_data() const = 0;
    virtual const char* source_type() const = 0;
    virtual void reset() = 0;
};
}
```

### IQSource Features:
- Supports 4 formats: INT16_PLANAR, INT16_INTERLEAVED, FLOAT32_PLANAR, FLOAT32_INTERLEAVED
- Multi-stage decimation (8x → 5x → Nx) 
- Final polyphase resampling for non-integer ratios
- Thread-safe (mutex-protected)
- Metadata: center_frequency, bandwidth, input_rate

### Key Methods for phoenix_sdr Integration:
```cpp
// This is what phoenix_sdr should call from its callback:
void push_samples_planar(const int16_t* xi, const int16_t* xq, size_t count);

// Or for .iqr file playback:
void push_samples_interleaved(const int16_t* iq, size_t count);
```

### Tests (10/10 PASS):
1. IQSource int16 planar conversion
2. IQSource int16 interleaved conversion
3. IQSource float32 planar conversion
4. IQSource float32 interleaved conversion
5. IQSource decimation (2 MSPS -> 48 kHz)
6. AudioSource basic conversion
7. AudioSource PCM input
8. SampleSource polymorphism
9. IQSource reset
10. IQSource metadata

---

## Integration Path for phoenix_sdr

### Option A: phoenix_sdr as Library
phoenix_sdr could link to the modem and call IQSource directly:

```cpp
// In phoenix_sdr callback:
#include "api/iq_source.h"

m110a::IQSource* iq_source;  // Created by modem

void on_samples(const int16_t* xi, const int16_t* xq, 
                uint32_t count, bool reset, void* ctx) {
    if (reset) {
        iq_source->reset();
    }
    iq_source->push_samples_planar(xi, xq, count);
}
```

### Option B: TCP/IPC Streaming
phoenix_sdr streams I/Q to modem over network:

```
phoenix_sdr → TCP → modem_server
    │
    └─ Raw I/Q @ 2 MSPS (int16 planar or interleaved)
```

Protocol would need to include:
- Sample rate negotiation
- Format specification
- Metadata (center freq, bandwidth, gain)

### Option C: File-Based (for testing/development)
Use existing .iqr files:

```cpp
// modem loads .iqr file:
IQSource source(2000000.0, IQSource::Format::INT16_INTERLEAVED, 48000.0);

// Read .iqr file, push samples:
iqr_reader_t* reader;
iqr_open(&reader, "capture.iqr");
int16_t iq[8192];
uint32_t num_read;
while (iqr_read(reader, ...) == IQR_OK) {
    source.push_samples_interleaved(iq, num_read);
}
```

---

## What Needs to Happen Next

### Immediate (No Code Changes Required):
1. **Test .iqr playback** - Take a capture from phoenix_sdr, feed it to IQSource
2. **Verify decimation quality** - Check that 2M→48k preserves signal integrity

### Short-term Integration:
1. **Define TCP protocol** - If going Option B, need command/data framing
2. **Add phoenix_sdr as git submodule** - If going Option A
3. **Create bridge code** - Either way, need glue between phoenix_sdr callbacks and IQSource

### Validation:
1. **Over-the-air test** - Capture known signal, verify decode
2. **Loopback test** - TX through channel sim, receive via SDR
3. **Compare audio vs I/Q path** - Should get identical (or better) BER

---

## Format Compatibility

| phoenix_sdr Output | IQSource Input | Match |
|-------------------|----------------|-------|
| int16 planar (xi, xq) | INT16_PLANAR | ✅ Perfect |
| .iqr file (interleaved) | INT16_INTERLEAVED | ✅ Perfect |

The modem team built exactly what we proposed in IQ_INPUT_DESIGN.md!

---

## Decimation Comparison

| Stage | phoenix_sdr (decimator.c) | IQSource |
|-------|---------------------------|----------|
| 1 | 2M → 250k (÷8, 31-tap) | 2M → 250k (÷8, 63-tap) |
| 2 | 250k → 50k (÷5, 25-tap) | 250k → 50k (÷5, 63-tap) |
| 3 | 50k → 48k (polyphase) | Linear interpolation |

Key difference: IQSource uses more filter taps (better stopband rejection) but simpler final resampling. For HF data modem, both should work fine.

**Recommendation:** Use IQSource decimation (modem-side) since it's already tested. phoenix_sdr can output at native SDR rate.

---

## Summary

The modem team independently built an I/Q interface that:
- Matches our design document exactly
- Has 10/10 unit tests passing  
- Supports all the formats phoenix_sdr produces
- Handles decimation modem-side (we don't need phoenix_sdr's decimator)

**Next action:** Test with real .iqr captures from the RSP2 Pro.
