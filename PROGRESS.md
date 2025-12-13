# Phoenix SDR - Progress & Status

**Last Updated:** 2025-12-13 ~11:45 EST

## Current Status: 🔴 BLOCKED

**Blocking Issue:** wwv_scan shows 0 dB SNR, SDRuno shows 35 dB SNR on same hardware.

## What We're Trying To Do

### Immediate Goal
1. Scan WWV frequencies (2.5, 5, 10, 15, 20, 25 MHz)
2. Find which has best signal
3. Record IQ data on that frequency
4. Validate our SDR capture chain works

### Why This Matters
This is a smoke test. If we can detect WWV ticks reliably, we know:
- SDR hardware interface works
- Our signal processing works
- Our timing (GPS) works
- We can proceed to build the actual HF modem

## What's Working ✅

| Component | Status | Notes |
|-----------|--------|-------|
| SDRplay API | ✅ | Opens device, configures, streams |
| GPS Serial | ✅ | Reads time from NEO-6M |
| Decimator | ✅ | 2MHz → 48kHz |
| DSP Math | ✅ | Unit tests pass 10/10 |
| Build System | ✅ | PowerShell + MinGW |
| Version Header | ✅ | v0.2.0 |

## What's Broken ❌

| Component | Status | Notes |
|-----------|--------|-------|
| WWV Detection | ❌ | 0 dB SNR instead of 35 dB |
| Signal Extraction | ❌ | Not seeing 1000 Hz modulation |

## The Bug

### Symptoms
```
SDRuno:     15 MHz WWV → 35.7 dB SNR ✓
wwv_scan:   15 MHz WWV → 0.7 dB SNR ✗
```

### Root Cause (Unknown)
The DSP math is correct (proven by unit tests). The signal is there (SDRuno sees it). Something in our real-signal pipeline is wrong.

### Suspects
1. Raw samples not what we expect?
2. Decimator losing signal?
3. Envelope not extracting modulation?
4. SDR API config issue?

## Next Action

**Debug wwv_scan.c `on_samples()` function:**

Add printf at each processing stage:
```c
printf("RAW: xi=%d xq=%d\n", xi[0], xq[0]);
printf("DECIM: I=%.2f Q=%.2f\n", buf[0].i, buf[0].q);
printf("MAG: %.4f\n", mag);
printf("AC: %.6f\n", ac);
printf("FILT: %.6f\n", filtered);
```

Find where the 1000 Hz modulation disappears.

## Files Changed Today

- `include/version.h` - Created (v0.2.0)
- `tools/wwv_scan.c` - Added 25 MHz, DC blocker, version output
- `test/test_dsp.c` - Created unit tests
- `src/sdr_stream.c` - Added DC/IQ correction enable
- `src/gps_serial.c` - Fixed DWORD format warning
- `README.md` - Full project documentation
- `.vscode/` - VS Code workspace and debug configs
- `.github/copilot-instructions.md` - AI assistant context

## Build Commands

```powershell
cd D:\claude_sandbox\phoenix_sdr
.\build.ps1 -Clean
.\build.ps1
.\build.ps1 -Target tools
.\build.ps1 -Target test
.\bin\test_test_dsp.exe      # Should show 10/10 pass
.\bin\wwv_scan.exe -scantime 5
```

## Hardware

- **SDR:** SDRplay RSP2 Pro (Serial: 1717046711)
- **GPS:** NEO-6M on Arduino (COM6, 115200 baud)
- **Antenna:** HF antenna on Hi-Z port

## Session Notes

- SDRuno works in both Low-IF and Zero-IF mode
- 15 MHz showing strong signal today (35+ dB)
- 25 MHz also a valid WWV frequency (was missing from our scan)
- Unit tests confirm biquad and DC blocker math is correct
- Problem is upstream of the DSP - somewhere in sample acquisition
