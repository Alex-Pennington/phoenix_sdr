# BCD Robust Detection - Implementation Roadmap

## Document Info
- **Date:** 2024-12-18
- **Project:** Phoenix SDR WWV Decoder
- **Purpose:** Add redundant time-domain and frequency-domain FFT paths for BCD symbol detection

---

## Implementation Status

| Phase | Module | Status | Notes |
|-------|--------|--------|-------|
| 1 | `bcd_time_detector.c/h` | ✅ Done | Pattern: tick_detector FFT path |
| 2 | `bcd_freq_detector.c/h` | ✅ Done | Pattern: marker_detector with larger FFT |
| 3 | `bcd_correlator.c/h` | ✅ Done | Pattern: sync_detector state machine |
| 4 | Wire into `waterfall.c` | ✅ Done | Parallel IQ feed, P2 isolation, wrapper callbacks |
| 5 | Update `build.ps1` | ✅ Done | Added build objects + linker entries |
| 6 | Test & tune thresholds | ⬜ Not Started | May need field adjustment |
| 7 | Deprecate old `bcd_decoder.c` | ⬜ Not Started | After new path validated |

---

## Technical Specifications

### Sample Rate
- **Input:** 50kHz (decimated from 2MHz)
- **Note:** 50kHz used instead of 48kHz due to sample rate mismatch causing dropped samples

### FFT Library
- **Library:** kiss_fft (following existing tick_detector/marker_detector pattern)

### Telemetry
- **Method:** UDP packets (existing pattern)
- **Symbol packet:** Contains decoded symbol data
- **Envelope packet:** Contains envelope data

### Integration Point
- **File:** `tools/waterfall.c` (NOT sdr_server.c)
- **Pattern:** Parallel IQ feed to each detector
- **Isolation:** Per P2 rules - raw `i_raw`, `q_raw` only shared data

### Build System
- **File:** `build.ps1` (NOT CMakeLists.txt)

---

## Current Signal Architecture (Established Pattern)

```
                              ┌─────────────────────┐
                              │   SDR IQ Source     │
                              │   (2MHz, decimated  │
                              │    to 50kHz)        │
                              └──────────┬──────────┘
                                         │
                    ┌────────────────────┼────────────────────┐
                    │                    │                    │
                    ▼                    ▼                    ▼
           ┌───────────────┐    ┌───────────────┐    ┌───────────────┐
           │ tick_detector │    │marker_detector│    │ bcd_envelope  │
           │   (1000Hz)    │    │   (1000Hz)    │    │   (100Hz)     │
           └───────┬───────┘    └───────┬───────┘    └───────┬───────┘
                   │                    │                    │
            ┌──────┴──────┐             │                    ▼
            │             │             │            ┌───────────────┐
            ▼             ▼             │            │  bcd_decoder  │
      [Time FFT]   [Freq FFT]           │            │  (single path)│
            │             │             │            └───────┬───────┘
            └──────┬──────┘             │                    │
                   │                    │                    ▼
                   ▼                    ▼              [SYMBOLS OUT]
           ┌─────────────────────────────┐            (unreliable)
           │      sync_detector          │
           │  (correlates tick+marker)   │
           └─────────────────────────────┘
                         │
                         ▼
                  [MINUTE SYNC OUT]
                    (reliable)
```

### Why This Pattern Works

The tick and marker detectors achieve reliability through **redundant parallel FFT paths**:

| Path | FFT Configuration | Strength | Weakness |
|------|-------------------|----------|----------|
| Time-domain FFT | Small FFT, fast frames | Precise edge timing | Poor frequency selectivity |
| Frequency-domain FFT | Large FFT, slow frames | Strong frequency isolation | Smeared timing |

**Both paths start at raw IQ** - no signal degradation. When both agree, confidence is high.

---

## Proposed BCD Signal Architecture

```
                              ┌─────────────────────┐
                              │   SDR IQ Source     │
                              │      (50kHz)        │
                              └──────────┬──────────┘
                                         │
         ┌───────────────┬───────────────┼───────────────┬───────────────┐
         │               │               │               │               │
         ▼               ▼               ▼               ▼               ▼
    tick_detector  marker_detector  bcd_time_det   bcd_freq_det    (existing)
      (1000Hz)       (1000Hz)         (100Hz)        (100Hz)
         │               │               │               │
         │               │          [Time FFT]     [Freq FFT]
         │               │               │               │
         ▼               ▼               └───────┬───────┘
    ┌─────────────────────┐                      │
    │    sync_detector    │                      ▼
    │  (minute boundary)  │              ┌───────────────┐
    └──────────┬──────────┘              │bcd_correlator │
               │                         │(symbol decision)│
               │                         └───────┬───────┘
               │                                 │
               │                                 ▼
               │                          [SYMBOLS OUT]
               │                           (reliable)
               │                                 │
               └───────────────┬─────────────────┘
                               │
                               ▼
                      ┌─────────────────┐
                      │  Controller via │
                      │  UDP telemetry  │
                      └─────────────────┘
```

---

## New Modules

### Module 1: `bcd_time_detector.c/h`

**Purpose:** Time-domain focused FFT for precise pulse edge detection at 100Hz

**Signal Path:**
```
IQ samples (50kHz)
    │
    ▼
┌──────────────────────────────────┐
│  Sample buffer (small FFT size)  │
│  256 samples = 5.12ms frames     │
└──────────────────────────────────┘
    │
    ▼ (buffer full)
┌──────────────────────────────────┐
│  Hann window + FFT               │
│  Small FFT = fast time response  │
└──────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────┐
│  Extract 100Hz bucket energy     │
│  (coarse frequency, ~195 Hz/bin) │
└──────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────┐
│  Adaptive noise floor            │
│  Threshold with hysteresis       │
└──────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────┐
│  State machine:                  │
│  IDLE → IN_PULSE → COOLDOWN      │
│  Precise edge timing (±5ms)      │
│  Measures pulse duration         │
└──────────────────────────────────┘
    │
    ▼
Callback: (pulse_start_ms, pulse_end_ms, duration_ms, peak_energy)
```

**FFT Configuration Rationale:**
- **FFT Size:** 256 (matches tick_detector pattern)
- **Frame Duration:** 5.12ms at 50kHz
- **Frequency Resolution:** ~195 Hz/bin (coarse, but adequate to see 100Hz energy)
- **Time Resolution:** Excellent - can pinpoint edges within 5ms

**Key Parameters:**
```c
#define BCD_TIME_FFT_SIZE        256
#define BCD_TIME_SAMPLE_RATE     50000
#define BCD_TIME_TARGET_FREQ_HZ  100
#define BCD_TIME_BANDWIDTH_HZ    50      /* Wider bucket for coarse resolution */
#define BCD_TIME_THRESHOLD_MULT  2.0f
#define BCD_TIME_HYSTERESIS      0.7f
```

**Location:** `tools/bcd_time_detector.c/h`

---

### Module 2: `bcd_freq_detector.c/h`

**Purpose:** Frequency-domain focused FFT for confident 100Hz presence detection

**Signal Path:**
```
IQ samples (50kHz)
    │
    ▼
┌──────────────────────────────────┐
│  Sample buffer (large FFT size)  │
│  2048 samples = 40.96ms frames   │
└──────────────────────────────────┘
    │
    ▼ (buffer full)
┌──────────────────────────────────┐
│  Hann window + FFT               │
│  Large FFT = fine freq resolution│
└──────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────┐
│  Extract 100Hz bucket energy     │
│  (precise: ~24 Hz/bin)           │
│  Can reject adjacent frequencies │
└──────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────┐
│  Sliding window accumulator      │
│  (matches marker_detector)       │
│  1-second window for 800ms pulse │
└──────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────┐
│  Adaptive baseline (self-track)  │
│  Threshold = baseline × mult     │
└──────────────────────────────────┘
    │
    ▼
┌──────────────────────────────────┐
│  State machine:                  │
│  IDLE → IN_PULSE → COOLDOWN      │
│  Confirms 100Hz is truly present │
└──────────────────────────────────┘
    │
    ▼
Callback: (timestamp_ms, duration_ms, accumulated_energy, snr_db)
```

**FFT Configuration Rationale:**
- **FFT Size:** 2048 (larger for frequency precision)
- **Frame Duration:** 40.96ms at 50kHz
- **Frequency Resolution:** ~24 Hz/bin (can cleanly isolate 100Hz)
- **Time Resolution:** Coarse - but that's what bcd_time_detector is for

**Key Parameters:**
```c
#define BCD_FREQ_FFT_SIZE        2048
#define BCD_FREQ_SAMPLE_RATE     50000
#define BCD_FREQ_TARGET_FREQ_HZ  100
#define BCD_FREQ_BANDWIDTH_HZ    15      /* Narrow bucket, precise isolation */
#define BCD_FREQ_WINDOW_MS       1000.0f /* Sliding window for accumulation */
#define BCD_FREQ_THRESHOLD_MULT  3.0f    /* Match marker_detector */
#define BCD_FREQ_NOISE_ADAPT     0.001f  /* Slow baseline adaptation */
```

**Location:** `tools/bcd_freq_detector.c/h`

---

### Module 3: `bcd_correlator.c/h`

**Purpose:** Correlates time-domain and freq-domain detections, emits high-confidence symbols

**Confidence Model (matches marker detection pattern):**

```
┌─────────────────────┐    ┌─────────────────────┐
│  bcd_time_detector  │    │  bcd_freq_detector  │
│  (precise timing)   │    │  (confident 100Hz)  │
└──────────┬──────────┘    └──────────┬──────────┘
           │                          │
           ▼                          ▼
      ┌─────────────────────────────────────┐
      │       Event Correlation Window      │
      │  Time detector fires at T1          │
      │  Freq detector fires at T2          │
      │  |T1 - T2| < CORRELATION_WINDOW?    │
      └─────────────────────────────────────┘
                       │
          ┌────────────┴────────────┐
          │                         │
          ▼                         ▼
    [CORRELATED]              [UNCORRELATED]
    Both fired within             Only one fired
    window, durations             or timing mismatch
    agree (±tolerance)
          │                         │
          ▼                         ▼
    ┌───────────┐            ┌───────────┐
    │ CONFIRMED │            │ REJECTED  │
    │  Symbol   │            │  (logged) │
    └─────┬─────┘            └───────────┘
          │
          ▼
    ┌─────────────────────────────────────┐
    │        Symbol Classification        │
    │  Duration 100-350ms → '0'           │
    │  Duration 350-650ms → '1'           │
    │  Duration 650-900ms → 'P'           │
    └─────────────────────────────────────┘
          │
          ▼
    Callback: (symbol, timestamp_ms, width_ms, confidence)
```

**State Machine (mirrors sync_detector):**
```
ACQUIRING ──────► TENTATIVE ──────► TRACKING
    │                 │                 │
    │ First           │ Second          │ Ongoing
    │ correlated      │ correlated      │ correlated
    │ symbol          │ symbol          │ symbols
    │                 │ (~1s apart)     │
    ▼                 ▼                 ▼
 No output        Low confidence    High confidence
                  symbols           symbols
```

**Key Parameters:**
```c
#define BCD_CORR_TIME_WINDOW_MS       100.0f  /* Detections within 100ms */
#define BCD_CORR_DURATION_TOLERANCE   0.20f   /* 20% duration agreement */
#define BCD_CORR_LOCKOUT_MS           200.0f  /* Prevent duplicate symbols */
#define BCD_CORR_MIN_INTERVAL_MS      800.0f  /* Min time between symbols */

/* Symbol width thresholds (same as existing) */
#define BCD_SYMBOL_ZERO_MAX_MS        350.0f
#define BCD_SYMBOL_ONE_MAX_MS         650.0f
#define BCD_SYMBOL_MARKER_MAX_MS      900.0f
```

**Location:** `tools/bcd_correlator.c/h`

---

## Surgical Edits Required

### 1. build.ps1

**File:** `build.ps1`

**Locate:** Source file list for tools build

**Add:**
```powershell
tools/bcd_time_detector.c
tools/bcd_freq_detector.c
tools/bcd_correlator.c
```

---

### 2. Header Includes - waterfall.c

**File:** `tools/waterfall.c`

**Locate:** Include section at top of file

**Add:**
```c
#include "bcd_time_detector.h"
#include "bcd_freq_detector.h"
#include "bcd_correlator.h"
```

---

### 3. Global Declarations - waterfall.c

**File:** `tools/waterfall.c`

**Locate:** Where existing detector instances are declared

**Add:**
```c
static bcd_time_detector_t *bcd_time_det = NULL;
static bcd_freq_detector_t *bcd_freq_det = NULL;
static bcd_correlator_t *bcd_corr = NULL;
```

---

### 4. Detector Creation - waterfall.c

**File:** `tools/waterfall.c`

**Locate:** Where existing detectors are created (tick_detector_create, marker_detector_create)

**Add:**
```c
/* Create BCD dual-path detectors */
bcd_time_det = bcd_time_detector_create("logs/wwv_bcd_time.csv");
bcd_freq_det = bcd_freq_detector_create("logs/wwv_bcd_freq.csv");
bcd_corr = bcd_correlator_create("logs/wwv_bcd_corr.csv");

/* Wire detector callbacks to correlator */
bcd_time_detector_set_callback(bcd_time_det, on_bcd_time_event, bcd_corr);
bcd_freq_detector_set_callback(bcd_freq_det, on_bcd_freq_event, bcd_corr);

/* Wire correlator output to UDP telemetry */
bcd_correlator_set_symbol_callback(bcd_corr, on_bcd_symbol, NULL);
```

---

### 5. IQ Split Point - waterfall.c

**File:** `tools/waterfall.c`

**Locate:** Sample processing loop where raw `i_raw`, `q_raw` are available

**Add (parallel to existing detector calls - per P2 isolation rules):**
```c
/* Existing detector calls receive raw IQ */
tick_detector_process_sample(tick_det, i_raw, q_raw);
marker_detector_process_sample(marker_det, i_raw, q_raw);

/* NEW: BCD dual-path detectors - same raw IQ, parallel paths */
bcd_time_detector_process_sample(bcd_time_det, i_raw, q_raw);
bcd_freq_detector_process_sample(bcd_freq_det, i_raw, q_raw);
```

**CRITICAL P2 COMPLIANCE:**
- New detectors receive `i_raw`, `q_raw` ONLY
- No access to `g_display_dsp` or `g_audio_dsp`
- No shared state with display or audio paths

---

### 6. Callback Handlers - waterfall.c

**File:** `tools/waterfall.c`

**Add callback functions:**
```c
/* Time detector event → correlator */
static void on_bcd_time_event(const bcd_time_event_t *event, void *user_data) {
    bcd_correlator_t *corr = (bcd_correlator_t *)user_data;
    bcd_correlator_time_event(corr, event->timestamp_ms,
                              event->duration_ms, event->peak_energy);
}

/* Freq detector event → correlator */
static void on_bcd_freq_event(const bcd_freq_event_t *event, void *user_data) {
    bcd_correlator_t *corr = (bcd_correlator_t *)user_data;
    bcd_correlator_freq_event(corr, event->timestamp_ms,
                              event->duration_ms, event->accumulated_energy);
}

/* Correlator symbol output → UDP telemetry */
static void on_bcd_symbol(const bcd_symbol_event_t *event, void *user_data) {
    (void)user_data;
    /* Send via UDP using existing pattern */
    /* Symbol packet format: matches existing bcd_decoder output */
}
```

---

### 7. Cleanup - waterfall.c

**File:** `tools/waterfall.c`

**Locate:** Shutdown/cleanup section

**Add:**
```c
bcd_time_detector_destroy(bcd_time_det);
bcd_freq_detector_destroy(bcd_freq_det);
bcd_correlator_destroy(bcd_corr);
```

---

## Summary: Time vs Frequency FFT Comparison

| Aspect | Time-Domain FFT | Frequency-Domain FFT |
|--------|-----------------|----------------------|
| **FFT Size** | 256 (small) | 2048 (large) |
| **Frame Duration** | 5.12ms | 40.96ms |
| **Freq Resolution** | ~195 Hz/bin (coarse) | ~24 Hz/bin (fine) |
| **Time Resolution** | Excellent (±5ms) | Poor (±40ms) |
| **Strength** | Precise pulse edges | Confident 100Hz ID |
| **Weakness** | May false-trigger on noise | Smeared timing |
| **Answers** | WHEN did pulse start/stop? | IS 100Hz truly present? |

**Together:** Time path provides precise timestamps. Freq path confirms it's real 100Hz (not noise/interference). Correlator requires both to agree before emitting symbol.

---

## Implementation Order

| Phase | Module | Effort | Notes |
|-------|--------|--------|-------|
| 1 | `bcd_time_detector.c/h` | Medium | Pattern: tick_detector FFT path |
| 2 | `bcd_freq_detector.c/h` | Medium | Pattern: marker_detector with larger FFT |
| 3 | `bcd_correlator.c/h` | Medium | Pattern: sync_detector state machine |
| 4 | Wire into `waterfall.c` | Low | Surgical edits listed above |
| 5 | Update `build.ps1` | Low | Add source files |
| 6 | Test & tune thresholds | Variable | May need field adjustment |
| 7 | Deprecate old `bcd_decoder.c` | Low | After new path validated |

---

## Files Created/Modified Summary

### New Files (create)
- `tools/bcd_time_detector.h`
- `tools/bcd_time_detector.c`
- `tools/bcd_freq_detector.h`
- `tools/bcd_freq_detector.c`
- `tools/bcd_correlator.h`
- `tools/bcd_correlator.c`

### Modified Files (surgical edits)
- `build.ps1` - add 3 source files
- `tools/waterfall.c` - includes, globals, create, process, callbacks, destroy

### Superseded (deprecate after validation)
- `tools/bcd_envelope.c/h` - old single-path envelope detection
- `tools/bcd_decoder.c/h` - old single-path symbol decoder
