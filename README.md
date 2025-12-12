# Phoenix SDR - RSP2 Pro Integration

SDRplay RSP2 Pro integration library for Phoenix Nest MARS Suite MIL-STD-188-110A modem testing.

## Requirements

- Windows 10/11 (64-bit)
- Visual Studio 2019+ Build Tools (or MinGW-w64)
- SDRplay API 3.x installed: https://www.sdrplay.com/api/
- SDRplay RSP2 Pro hardware

## Building

```powershell
# Debug build
.\build.ps1

# Release build
.\build.ps1 -Release

# Clean
.\build.ps1 -Clean

# Build and run tests
.\build.ps1 -Target test
```

## Project Structure

```
phoenix_sdr/
├── build.ps1              # PowerShell build script (MSVC/MinGW)
├── README.md              # This file
├── include/
│   ├── phoenix_sdr.h      # SDR device API
│   └── iq_recorder.h      # I/Q recording API
├── src/
│   ├── main.c             # Test harness
│   ├── sdr_device.c       # Device enumeration, open, close
│   ├── sdr_stream.c       # Streaming, callbacks, runtime updates
│   └── iq_recorder.c      # I/Q file recording/playback
└── test/
    └── (future unit tests)
```

---

## SDR API Usage

```c
#include "phoenix_sdr.h"

// Enumerate devices
psdr_device_info_t devices[8];
size_t num_devices;
psdr_enumerate(devices, 8, &num_devices);

// Open device
psdr_context_t *ctx;
psdr_open(&ctx, 0);

// Configure for HF narrowband
psdr_config_t config;
psdr_config_defaults(&config);
config.freq_hz = 7074000.0;  // 40m FT8
psdr_configure(ctx, &config);

// Set up callbacks
psdr_callbacks_t cb = {
    .on_samples = my_sample_handler,
    .on_overload = my_overload_handler,
    .user_ctx = my_data
};

// Start streaming
psdr_start(ctx, &cb);

// ... process samples in callback ...

// Cleanup
psdr_stop(ctx);
psdr_close(ctx);
```

### Sample Callback

```c
void my_sample_handler(
    const int16_t *xi,      // I samples (real)
    const int16_t *xq,      // Q samples (imaginary)  
    uint32_t count,         // Number of samples
    bool reset,             // True if buffers should be flushed
    void *user_ctx          // Your context pointer
) {
    // Process I/Q samples here
    // WARNING: Called from API thread - keep fast or copy & defer
}
```

### Default Configuration

| Parameter      | Default Value | Notes |
|----------------|---------------|-------|
| Sample Rate    | 2 MSPS        | 14-bit mode |
| Bandwidth      | 200 kHz       | Narrowest available |
| IF Mode        | Zero IF       | Baseband I/Q |
| AGC            | Disabled      | Manual gain for modem work |
| Gain Reduction | 40 dB         | Moderate |
| LNA State      | 4             | Mid-range |

---

## I/Q Recording API

Record raw I/Q samples to disk for offline analysis, regression testing, and modem development without live RF.

### Recording

```c
#include "iq_recorder.h"

// Create recorder
iqr_recorder_t *rec;
iqr_create(&rec, 0);  // 0 = default 64K sample buffer

// Start recording
iqr_start(rec, 
    "capture_40m.iqr",    // Output filename
    2000000.0,            // Sample rate Hz
    7074000.0,            // Center frequency Hz
    200,                  // Bandwidth kHz
    40,                   // Gain reduction dB
    4                     // LNA state
);

// In your sample callback:
void on_samples(const int16_t *xi, const int16_t *xq, 
                uint32_t count, bool reset, void *ctx) {
    iqr_recorder_t *rec = (iqr_recorder_t *)ctx;
    iqr_write(rec, xi, xq, count);
}

// Stop recording
iqr_stop(rec);
iqr_destroy(rec);
```

### Playback

```c
#include "iq_recorder.h"

// Open recording
iqr_reader_t *reader;
iqr_open(&reader, "capture_40m.iqr");

// Get metadata
const iqr_header_t *hdr = iqr_get_header(reader);
printf("Sample rate: %.0f Hz\n", hdr->sample_rate_hz);
printf("Duration: %.2f sec\n", 
       (double)hdr->sample_count / hdr->sample_rate_hz);

// Read samples
int16_t xi[4096], xq[4096];
uint32_t num_read;

while (iqr_read(reader, xi, xq, 4096, &num_read) == IQR_OK && num_read > 0) {
    // Process samples - feed to modem, etc.
    process_iq_samples(xi, xq, num_read);
}

// Cleanup
iqr_close(reader);
```

### I/Q File Format (.iqr)

Binary format optimized for streaming and random access:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | Magic | "IQR1" |
| 4 | 4 | Version | Format version (1) |
| 8 | 8 | Sample Rate | Hz (double) |
| 16 | 8 | Center Freq | Hz (double) |
| 24 | 4 | Bandwidth | kHz (uint32) |
| 28 | 4 | Gain Reduction | dB (int32) |
| 32 | 4 | LNA State | 0-8 (uint32) |
| 36 | 8 | Start Time | Unix µs (int64) |
| 44 | 8 | Sample Count | Total samples (uint64) |
| 52 | 4 | Flags | Reserved |
| 56 | 8 | Reserved | Padding |
| **64** | ... | **Data** | **Interleaved I/Q (int16 pairs)** |

Data section contains interleaved samples: `I0, Q0, I1, Q1, I2, Q2, ...`

Each sample is a signed 16-bit integer, little-endian. File size = 64 + (sample_count × 4) bytes.

---

## Typical Workflow

1. **Live Capture**: Connect RSP2 Pro, run test harness with recording enabled
2. **Collect Samples**: Capture various propagation conditions, signal levels
3. **Offline Development**: Play back recordings through modem code
4. **Regression Testing**: Re-run saved captures after code changes
5. **Integration**: Feed live I/Q stream to modem receive chain

---

## License

Copyright (c) 2024 Phoenix Nest LLC
