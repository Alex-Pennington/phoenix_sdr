# Phoenix SDR - Copilot Instructions

## P0 - CRITICAL RULES

1. **NO unauthorized additions.** Do not add features, flags, modes, or files without explicit instruction. ASK FIRST.
2. **Minimal scope.** Fix only what's requested. One change at a time.
3. **Suggest before acting.** Explain your plan, wait for confirmation.
4. **Verify before modifying.** Run `git status`, read files before editing. Never run destructive commands blindly.

---

## Architecture Overview

**Phoenix SDR** is a Windows C application for receiving WWV/WWVH time signals via SDRplay RSP2 Pro hardware.

### Signal Flow
```
SDR Hardware (2 MHz I/Q) → sdr_stream.c → TCP → waterfall.c
                                                     │
                    ┌────────────────────────────────┴────────────────────────────────┐
                    │                          i_raw, q_raw                            │
                    ▼                                                                  ▼
            DETECTOR PATH (50 kHz)                                          DISPLAY PATH (12 kHz)
            lowpass → decimate                                              lowpass → decimate
                    │                                                                  │
    ┌───────────────┼───────────────┐                               ┌──────────────────┤
    ▼               ▼               ▼                               ▼                  ▼
tick_detector  marker_detector  bcd_*_detector              tone_tracker(s)     FFT waterfall
    │               │               │                               │
    ▼               ▼               ▼                               ▼
 callbacks →   CSV logging  +  UDP telemetry
```

### Key Components
| Path | Purpose |
|------|---------|
| `src/` | Core SDR interface (`sdr_*.c`), decimation, I/Q recording |
| `tools/` | Executable tools and detector modules (`.c` + `.h` pairs) |
| `include/` | Public headers, `phoenix_sdr.h` is main API |
| `test/` | Unit tests using `test_framework.h` |

---

## Build & Test

```powershell
.\build.ps1                    # Debug build
.\build.ps1 -Release           # Optimized build  
.\build.ps1 -Target tools      # Tools only
.\build.ps1 -Clean             # Clean artifacts
.\run_tests.ps1                # All unit tests
.\run_tests.ps1 -Filter "dsp"  # Specific tests
```

**Outputs:** `bin/*.exe`, `bin/test/*.exe`

### Quick Run
```powershell
cmd /c ".\bin\simple_am_receiver.exe -f 5.000450 -g 59 -l 0 -o | .\bin\waterfall.exe"
```

---

## Critical Patterns

### P1 - Signal Path Divergence
All signal processors receive samples from the SAME divergence point in `waterfall.c`:
```c
// Raw samples normalized to [-1, 1]
float i_raw = (float)samples[s * 2] / 32768.0f;
float q_raw = (float)samples[s * 2 + 1] / 32768.0f;

// DETECTOR PATH (50 kHz) - for pulse detection
float det_i = lowpass_process(&g_detector_lowpass_i, i_raw);
float det_q = lowpass_process(&g_detector_lowpass_q, i_raw);
// → tick_detector, marker_detector, bcd_*_detector

// DISPLAY PATH (12 kHz) - for visualization
float disp_i = lowpass_process(&g_display_lowpass_i, i_raw);
float disp_q = lowpass_process(&g_display_lowpass_q, q_raw);
// → tone_tracker, bcd_envelope, FFT waterfall
```
**Never cross paths.** Detectors use `det_i/det_q`. Display uses `disp_i/disp_q`.

### P2 - Detector Module Pattern
All detectors follow the same structure (see `tick_detector.h/.c` as template):
```c
// Header pattern
typedef struct xxx_detector xxx_detector_t;           // Opaque type
typedef void (*xxx_callback_fn)(const xxx_event_t *event, void *user_data);

xxx_detector_t *xxx_detector_create(const char *csv_path);  // NULL disables CSV
void xxx_detector_destroy(xxx_detector_t *det);
void xxx_detector_set_callback(xxx_detector_t *det, xxx_callback_fn cb, void *user_data);
bool xxx_detector_process_sample(xxx_detector_t *det, float i, float q);
```
Each detector owns: own FFT, own sample buffer, own state machine.

### P3 - CSV/UDP Telemetry Pattern
Detectors support dual output:
1. **CSV file** - passed to `_create(csv_path)`, logged with `fprintf()` + `fflush()`
2. **UDP telemetry** - via `telem_sendf(TELEM_CHANNEL, "format", ...)` from `waterfall_telemetry.h`

```c
// CSV header pattern (written in _create)
fprintf(csv_file, "# Phoenix SDR %s Log v%s\n", detector_name, PHOENIX_VERSION_FULL);
fprintf(csv_file, "time,timestamp_ms,field1,field2,...\n");

// UDP telemetry pattern (from callbacks)
telem_sendf(TELEM_TICKS, "%s,%.1f,T%d,%d,...", time_str, timestamp_ms, tick_num, ...);
```
Channel prefixes: `TICK`, `MARK`, `SYNC`, `BCDS`, `CARR`, `T500`, `T600`

### P4 - Display/Audio Isolation
Display and audio paths in `waterfall.c` must NEVER share variables or filters:
- Display: `g_display_dsp`
- Audio: `g_audio_dsp` (in `waterfall_audio.c`)

### P5 - DSP Filter Pattern
Filters use inline structs. Reference: `test/test_dsp.c`
```c
biquad_t bp; biquad_init_bp(&bp, 48000.0f, 1000.0f, 5.0f);  // bandpass
dc_block_t dc; dc_block_init(&dc, 0.9999f);                  // DC blocker
```

### P6 - Test Framework
Tests use `test/test_framework.h`:
```c
TEST(my_test) { ASSERT(condition, "message"); PASS(); }
```

### P7 - Callback Event Structs
Each detector defines an event struct passed to callbacks:
```c
typedef struct {
    float timestamp_ms;     // When event occurred
    float duration_ms;      // Pulse/event duration
    float peak_energy;      // Signal strength
    // ... detector-specific fields
} xxx_event_t;
```
Examples: `tick_event_t`, `marker_event_t`, `bcd_time_event_t`, `bcd_symbol_event_t`

### P8 - UI Flash Feedback Pattern
Detectors provide flash state for visual feedback via `waterfall_flash.h`:
```c
int xxx_detector_get_flash_frames(xxx_detector_t *det);   // Frames remaining (0 = not flashing)
void xxx_detector_decrement_flash(xxx_detector_t *det);   // Call once per display frame
```
Flash sources register with `flash_source_register()` for waterfall band markers and bar panel flashes.

### P9 - WWV Broadcast Clock
`wwv_clock.h` provides WWV/WWVH broadcast schedule knowledge:
- Knows tick/marker/silence schedule (seconds 29, 59 have no tick)
- Predicts expected events from system time
- Station-aware (WWV=1000Hz, WWVH=1200Hz)
```c
wwv_clock_t *clk = wwv_clock_create(WWV_STATION_WWV);
wwv_time_t now = wwv_clock_now(clk);  // {second, minute, hour, expected_event, tick_expected}
```

---

## Frozen Files

| File | Reason |
|------|--------|
| `tools/waterfall.c` (signal chain) | Display path, detector path, FFT, decimation - working |
| `tools/waterfall_dsp.c` | Lowpass filters, decimation logic - working |
| `tools/marker_detector.c` | Minute marker detection - working |
| `tools/marker_detector.h` | Minute marker API - stable |
| `tools/marker_correlator.c` | Marker correlation - working |
| `tools/marker_correlator.h` | Marker correlator API - stable |
| `tools/sync_detector.c` | Sync state machine - working |
| `tools/sync_detector.h` | Sync detector API - stable |

---

## Domain Knowledge

- **WWV/WWVH:** NIST time stations at 5/10/15/20 MHz
- **Tick:** 5ms pulse of 1000 Hz tone every second (AM modulated)
- **Minute marker:** 800ms pulse at second 0 of each minute
- **BCD time code:** 100 Hz subcarrier with binary time data
- **DC hole:** Zero-IF receivers have DC offset; tune 450 Hz off-center

---

## Dependencies

| Library | Location | Purpose |
|---------|----------|---------|
| SDRplay API 3.x | `C:\Program Files\SDRplay\API\` | Hardware access |
| SDL2 2.30.9 | `libs/SDL2/` | Graphics/audio |
| KissFFT | `src/kiss_fft.c` | FFT processing |
| MinGW-w64 | winget install | GCC compiler |
- **DC hole:** Zero-IF receivers have DC offset; tune 450 Hz off-center

---

## Dependencies

| Library | Location | Purpose |
|---------|----------|---------|
| SDRplay API 3.x | `C:\Program Files\SDRplay\API\` | Hardware access |
| SDL2 2.30.9 | `libs/SDL2/` | Graphics/audio |
| KissFFT | `src/kiss_fft.c` | FFT processing |
| MinGW-w64 | winget install | GCC compiler |
