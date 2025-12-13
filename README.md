# Phoenix Nest MARS Suite - SDR Component

## The Big Picture

This project is part of the **Phoenix Nest MARS Suite**, an open-source implementation of **MIL-STD-188-110A** - a military standard for HF (High Frequency) data modems. The goal is to enable digital communication over HF radio, specifically for MARS (Military Auxiliary Radio System) operations.

### Why This Matters
HF radio can communicate over thousands of miles without any infrastructure (no internet, no cell towers, no satellites). This makes it critical for:
- Emergency communications
- Remote operations
- Military auxiliary support
- When all else fails

### Project Components
```
Phoenix Nest MARS Suite
├── SDR Capture (THIS PROJECT) ← We are here
│   ├── Hardware interface (SDRplay RSP2 Pro)
│   ├── Signal capture and recording
│   ├── GPS timing synchronization
│   └── WWV validation (current focus)
│
├── MIL-STD-188-110A Modem (future)
│   ├── MELP-e voice codec
│   ├── PSK/QAM modulation
│   └── FEC encoding
│
└── MARS-ALE Integration (future)
    └── Automatic Link Establishment
```

## Current Focus: WWV Timing Validation

### What is WWV?
WWV is a time signal radio station operated by NIST in Fort Collins, Colorado. It broadcasts:
- Precise time (from atomic clocks)
- Standard frequencies (2.5, 5, 10, 15, 20, 25 MHz)
- A distinctive "tick" - 5ms pulse of 1000 Hz tone every second

### Why WWV for Testing?
WWV is the **perfect smoke test** for our SDR chain because:

1. **Always on** - Broadcasts 24/7/365
2. **Known signal** - We know exactly what it should look like
3. **Predictable timing** - GPS-synchronized, we can verify our timing
4. **Multiple frequencies** - Tests our tuning across HF band
5. **MARS funds WWV** - It's literally our reference standard

### The Validation Goal
```
If we can:
  1. Tune to WWV frequency ✓
  2. Detect the 1000 Hz tick with good SNR ← STUCK HERE
  3. Verify timing matches GPS
  4. Record clean IQ data

Then we know:
  - SDR hardware works
  - Our capture code works
  - Our timing is accurate
  - We're ready to build the modem
```

## Current Task: WWV Scanner

### What wwv_scan Does
1. Connects to GPS (for precise timing)
2. Opens SDR (RSP2 Pro)
3. Scans each WWV frequency (2.5, 5, 10, 15, 20, 25 MHz)
4. For each frequency:
   - Tunes SDR
   - Waits for GPS second boundary
   - Measures energy in "tick window" (0-50ms) 
   - Measures energy in "noise window" (200-800ms)
   - Calculates SNR = tick_energy / noise_energy
5. Reports best frequency
6. User can then record on that frequency

### What's Broken
**SDRuno** (commercial software) shows **35+ dB SNR** on 15 MHz WWV.
**Our wwv_scan** shows **~0 dB SNR** on all frequencies.

Same hardware, same antenna, same signal - different results.

### Signal Processing Chain
```
┌─────────────────────────────────────────────────────────────┐
│ SDRplay RSP2 Pro                                            │
│ - Tuned to WWV frequency (e.g., 15 MHz)                    │
│ - 2 MHz sample rate                                         │
│ - Zero-IF mode (signal centered at 0 Hz)                   │
│ - Outputs I/Q samples (int16)                              │
└─────────────────┬───────────────────────────────────────────┘
                  │ Raw I/Q @ 2 MHz
                  ▼
┌─────────────────────────────────────────────────────────────┐
│ Decimator                                                   │
│ - 2 MHz → 48 kHz (factor of ~41.67)                        │
│ - Anti-aliasing filter                                      │
│ - Output: float I/Q                                        │
└─────────────────┬───────────────────────────────────────────┘
                  │ I/Q @ 48 kHz
                  ▼
┌─────────────────────────────────────────────────────────────┐
│ AM Envelope Detection                                       │
│ - magnitude = sqrt(I² + Q²)                                │
│ - Extracts amplitude modulation                            │
│ - WWV tick is AM: carrier + 1000 Hz tone                   │
└─────────────────┬───────────────────────────────────────────┘
                  │ Envelope signal
                  ▼
┌─────────────────────────────────────────────────────────────┐
│ DC Blocking Filter                                          │
│ - Removes carrier (DC component of envelope)               │
│ - Passes AC (the 1000 Hz modulation)                       │
│ - y[n] = x[n] - x[n-1] + 0.995 * y[n-1]                   │
└─────────────────┬───────────────────────────────────────────┘
                  │ AC component only
                  ▼
┌─────────────────────────────────────────────────────────────┐
│ 1000 Hz Bandpass Filter                                     │
│ - Biquad IIR, Q=5                                          │
│ - Passes 1000 Hz tick tone                                 │
│ - Rejects other frequencies                                │
└─────────────────┬───────────────────────────────────────────┘
                  │ 1000 Hz component
                  ▼
┌─────────────────────────────────────────────────────────────┐
│ Energy Measurement (GPS-synchronized windows)               │
│                                                             │
│ Second boundary (from GPS)                                  │
│ ├── 0-50ms: TICK WINDOW - measure energy here              │
│ ├── 50-200ms: skip (filter settling)                       │
│ ├── 200-800ms: NOISE WINDOW - measure energy here          │
│ └── 800-1000ms: skip (next tick coming)                    │
│                                                             │
│ SNR = 10 * log10(tick_energy / noise_energy)               │
└─────────────────────────────────────────────────────────────┘
```

### The Mystery
Unit tests prove the DSP math is correct:
- Synthetic signal with 1000 Hz modulation → high SNR ✓
- Synthetic signal with no modulation → 0 dB SNR ✓

So either:
1. **Raw samples are wrong** - SDR not configured correctly
2. **Decimator loses the signal** - Bug in downsampling  
3. **Envelope doesn't show modulation** - Signal not where we expect
4. **Something else upstream** - We're missing something

### Debug Strategy
Add printf statements or breakpoints to see actual values:
```c
// In on_samples() callback:
printf("RAW: xi[0]=%d xq[0]=%d\n", xi[0], xq[0]);

// After decimation:
printf("DECIM: I=%.2f Q=%.2f\n", g_decim_buffer[0].i, g_decim_buffer[0].q);

// After envelope:
printf("MAG: %.4f\n", mag);

// After DC block:
printf("AC: %.6f\n", ac);

// After bandpass:
printf("FILT: %.6f\n", filtered);
```

Find where the signal disappears!

## File Organization

```
D:\claude_sandbox\phoenix_sdr\
│
├── include/
│   └── version.h            # Version: 0.2.0
│
├── src/
│   ├── phoenix_sdr.h        # Main header, types, API
│   ├── main.c               # Recording application
│   ├── sdr_device.c         # SDRplay open/close/enumerate
│   ├── sdr_stream.c         # Configure, start, stop, callbacks
│   ├── decimator.c          # 2MHz → 48kHz conversion
│   ├── gps_serial.c         # GPS NEO-6M serial interface
│   ├── iq_recorder.c        # IQ file writing
│   └── iqr_meta.c           # Recording metadata
│
├── tools/
│   ├── wwv_scan.c           # ← MAIN DEBUG TARGET
│   ├── wwv_sync.c           # Time sync from WWV
│   ├── wwv_analyze.c        # Analyze recorded IQ
│   ├── gps_time.c           # Display GPS time
│   └── ...                  # Other utilities
│
├── test/
│   └── test_dsp.c           # DSP unit tests (all pass)
│
├── build.ps1                # Build script
├── PROGRESS.md              # Status document
└── README.md                # This file
```

## Building

```powershell
# Full clean build
.\build.ps1 -Clean
.\build.ps1
.\build.ps1 -Target tools
.\build.ps1 -Target test

# Run tests
.\bin\test_test_dsp.exe

# Run scanner
.\bin\wwv_scan.exe -scantime 10
```

## Hardware Setup

### SDRplay RSP2 Pro
- USB connection
- Hi-Z antenna input (for HF)
- SDRplay API 3.15 installed

### GPS (NEO-6M on Arduino)
- COM6 @ 115200 baud
- Outputs: `2025-12-13T16:45:30.123 [VALID, SAT:8, NMEA:42, ms:123]`
- Used for precise second-boundary timing

### Antenna
- HF antenna connected to Hi-Z port
- Same antenna works great with SDRuno

## Key Contacts & References

- **Steve Hajducek (N2CKH)** - Created MS-DMT and MARS-ALE, 45+ years radio experience
- **Charles Brain (G4GUO)** - Created brain_core modem, PC-ALE, PC-HFDL
- **WWV** - NIST time station, Fort Collins CO, nist.gov
- **SDRplay API** - sdrplay.com/api

## Next Steps

1. **Debug wwv_scan** - Find where signal disappears
2. **Get working SNR** - Should see 20-35 dB like SDRuno
3. **Record WWV** - Capture IQ file on best frequency
4. **Validate recording** - Confirm tick detection in recorded file
5. **Move to modem** - Start MIL-STD-188-110A implementation
