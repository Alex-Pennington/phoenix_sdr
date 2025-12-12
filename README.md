# Phoenix SDR - RSP2 Pro Integration

SDRplay RSP2 Pro integration library for Phoenix Nest MARS Suite MIL-STD-188-110A modem testing.

## Status

| Component | Status |
|-----------|--------|
| SDR Interface | ✅ Working - tested with RSP2 Pro |
| I/Q Recording | ✅ Working - .iqr format verified |
| I/Q Playback | ✅ Working |
| Decimator | ✅ Working - 2 MSPS → 48 kHz |
| Modem Integration | ✅ Ready - IQSource validated (31/31 tests) |

## Documentation

- **[docs/IQ_INPUT_DESIGN.md](docs/IQ_INPUT_DESIGN.md)** - Design document for modem integration
- **[docs/BETA_TESTING_GUIDE.md](docs/BETA_TESTING_GUIDE.md)** - Guide for beta testers

## Requirements

- Windows 10/11 (64-bit)
- MinGW-w64 (via winget) or Visual Studio 2019+ Build Tools
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
```

---

## Quick Start

```powershell
# Build
.\build.ps1

# Run - FREQUENCY IS REQUIRED
.\bin\phoenix_sdr.exe -f 7.074

# Show help
.\bin\phoenix_sdr.exe -h
```

## Command-Line Usage

```
Usage: phoenix_sdr -f <freq_MHz> [options]

Required:
  -f, --freq <MHz>      Center frequency in MHz (e.g., 7.074, 14.074)

Optional:
  -d, --duration <sec>  Recording duration in seconds (default: 5)
  -o, --output <name>   Output filename prefix (default: "capture")
  -g, --gain <dB>       Gain reduction 20-59 dB (default: 40)
  -h, --help            Show this help message

Output Files:
  <name>_raw.iqr        Full-rate I/Q at 2 MSPS
  <name>_48k.iqr        Decimated I/Q at 48 kHz (modem-ready)

Examples:
  phoenix_sdr -f 7.074                    # Record 5 sec at 7.074 MHz (40m FT8)
  phoenix_sdr -f 14.074 -d 30             # Record 30 sec at 14.074 MHz (20m FT8)
  phoenix_sdr -f 7.074 -o ft8_capture     # Custom output filename
  phoenix_sdr -f 14.074 -g 30 -d 60       # Lower gain, 60 sec recording

Frequency Range: 0.001 - 2000 MHz (SDRplay RSP2 Pro)
```

---

## Project Structure

```
phoenix_sdr/
├── build.ps1              # PowerShell build script
├── README.md              # This file
├── docs/
│   ├── IQ_INPUT_DESIGN.md # Modem integration design doc
│   └── BETA_TESTING_GUIDE.md # Beta testing instructions
├── include/
│   ├── phoenix_sdr.h      # SDR device API
│   ├── iq_recorder.h      # I/Q recording API
│   └── decimator.h        # Sample rate conversion
├── src/
│   ├── main.c             # Command-line application
│   ├── sdr_device.c       # Device enumeration, open, close
│   ├── sdr_stream.c       # Streaming, callbacks, runtime updates
│   ├── iq_recorder.c      # I/Q file recording/playback
│   └── decimator.c        # 2 MSPS → 48 kHz conversion
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
config.freq_hz = 7074000.0;  // 7.074 MHz (40m FT8)
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
    "capture.iqr",        // Output filename
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
iqr_open(&reader, "capture.iqr");

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

## Decimator

The decimator converts 2 MSPS SDR output to 48 kHz for modem input:

```
2,000,000 Hz → 250,000 Hz → 50,000 Hz → 48,000 Hz
     (÷8)          (÷5)        (48/50 resample)
```

This is handled automatically when you run `phoenix_sdr` - both raw and decimated files are produced.

---

## Modem Integration

See **[docs/IQ_INPUT_DESIGN.md](docs/IQ_INPUT_DESIGN.md)** for the full design document describing:

- SampleSource abstraction for modem input
- AudioSource (existing 48kHz path with Hilbert transform)
- IQSource (new direct I/Q path)
- Format conversion and decimation details
- Integration options (callback, file, TCP)

The modem's I/Q pipeline has been validated with 31/31 tests passing. See the [pennington_m110a_demod](https://github.com/Alex-Pennington/pennington_m110a_demod) repository for modem-side integration.

---

## License

Copyright (c) 2024 Phoenix Nest LLC
