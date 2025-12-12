# Phoenix SDR ↔ Modem Integration Handoff

**Date:** 2025-12-12  
**From:** Claude (claude.ai chat session)  
**To:** Claude (VS Code coding agent)  
**Subject:** phoenix_sdr is ready for integration with IQSource

---

## Summary

I built the **phoenix_sdr** library in a separate chat session while you were working on the modem. Our interfaces match perfectly - no adapter code needed!

---

## What I Built (phoenix_sdr repo)

**Repository:** https://github.com/Alex-Pennington/phoenix_sdr

### Key Files:
| File | Purpose |
|------|---------|
| `include/phoenix_sdr.h` | Main SDR API - device init, streaming |
| `include/iq_recorder.h` | .iqr file format - read/write I/Q captures |
| `include/decimator.h` | Decimation (OPTIONAL - you handle this modem-side) |
| `src/main.c` | Example usage with callbacks |
| `docs/IQ_INPUT_DESIGN.md` | Design doc I wrote for you |
| `docs/SDR_INTEGRATION_ANALYSIS.md` | Analysis of your IQSource implementation |

### Callback Signature (what phoenix_sdr produces):
```c
typedef void (*sample_callback_t)(
    const int16_t* xi,      // I samples (real)
    const int16_t* xq,      // Q samples (imaginary)  
    uint32_t num_samples,   // Sample count
    bool reset,             // True on first callback after start
    void* user_context      // User data
);
```

### .iqr File Format:
- 64-byte header (magic, sample rate, center freq, etc.)
- Data: interleaved int16 pairs (I0,Q0,I1,Q1,...)
- 2 MSPS default sample rate

---

## What You Built (IQSource)

**Branch:** `feature/sdrplay-integration` on `pennington_m110a_demod`

### Your Interface (what IQSource accepts):
```cpp
void push_samples_planar(const int16_t* xi, const int16_t* xq, size_t count);
void push_samples_interleaved(const int16_t* iq, size_t count);
```

### The Match 🎯
```
phoenix_sdr callback:  (int16_t* xi, int16_t* xq, uint32_t count, ...)
IQSource accepts:      (int16_t* xi, int16_t* xq, size_t count)

DIRECT MATCH - just cast uint32_t → size_t
```

---

## Integration Code

### For Live SDR Streaming:
```cpp
#include "api/iq_source.h"

// Global IQSource (or pass via user_context)
m110a::IQSource* g_iq_source = nullptr;

// Bridge callback
extern "C" void on_samples(const int16_t* xi, const int16_t* xq,
                           uint32_t count, bool reset, void* ctx) {
    if (reset) {
        g_iq_source->reset();
    }
    g_iq_source->push_samples_planar(xi, xq, static_cast<size_t>(count));
}
```

### For .iqr File Playback:
```cpp
#include "api/iq_source.h"
#include <cstdio>

m110a::IQSource source(2000000.0, m110a::IQSource::Format::INT16_INTERLEAVED, 48000.0);

FILE* f = fopen("capture.iqr", "rb");
fseek(f, 64, SEEK_SET);  // Skip header

int16_t buffer[8192];  // Interleaved I,Q pairs
while (size_t n = fread(buffer, sizeof(int16_t), 8192, f)) {
    source.push_samples_interleaved(buffer, n / 2);  // n/2 = sample pairs
}
fclose(f);
```

---

## Decimation Status

| Component | Status |
|-----------|--------|
| phoenix_sdr decimator.c | Built but OPTIONAL |
| IQSource decimation | ✅ Working (10/10 tests pass) |

**Recommendation:** Use YOUR decimation (IQSource). It's tested and has better filter taps. phoenix_sdr will output raw 2 MSPS.

---

## Hardware Info

| Parameter | Value |
|-----------|-------|
| Device | SDRplay RSP2 Pro |
| Serial | 1717046711 |
| Sample Rate | 2 MSPS |
| Output Format | int16 planar (xi, xq arrays) |
| Bandwidth | 200 kHz (configurable) |

---

## Next Steps

1. **File-based test first:** Use a .iqr capture through IQSource
2. **Then live SDR:** Connect phoenix_sdr callback to IQSource
3. **Verify decode:** Capture known 110A signal, confirm it decodes

---

## Git Commands for You

```bash
# In modem repo - your branch is already published
cd path/to/pennington_m110a_demod
git checkout feature/sdrplay-integration
git pull origin feature/sdrplay-integration

# To see phoenix_sdr (separate repo)
git clone https://github.com/Alex-Pennington/phoenix_sdr.git
```

---

*Two AI agents, two repos, one perfect interface match. Let's decode some signals! 📡*
