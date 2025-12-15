# Phoenix SDR - Copilot Instructions

## P0 - CRITICAL RULES (READ BEFORE DOING ANYTHING)

1. **DO NOT add features, flags, modes, or functionality without explicit user instruction.**
   - No new command-line flags (like `-n`, `--debug`, etc.)
   - No new operating modes (like "no hardware mode", "test mode", etc.)
   - No new files unless explicitly requested
   - If something seems needed but wasn't requested, **ASK FIRST**

2. **When in doubt, ASK.** Do not assume. Do not "improve" things proactively.

3. **Stick to the exact scope of the request.** If asked to fix X, fix X only.

---

## READ THIS FIRST

You are helping debug a **WWV signal detection problem**. This is part of a larger project (MIL-STD-188-110A HF modem), but right now the ONLY goal is:

**Find why our code sees 0 dB SNR when SDRuno sees 35 dB SNR on the same hardware.**

## The Immediate Problem

### What Should Happen
1. wwv_scan tunes to WWV (e.g., 15 MHz)
2. WWV broadcasts a 5ms pulse of 1000 Hz tone every second
3. We measure energy during the tick (0-50ms) vs between ticks (200-800ms)
4. SNR should be 20-35 dB (tick is MUCH louder than noise)

### What Actually Happens
- SNR ≈ 0 dB (tick energy ≈ noise energy)
- This means we're NOT detecting the 1000 Hz modulation

### What's Proven Working
- DSP math (unit tests pass in test/test_dsp.c)
- SDRuno on same hardware sees the signal fine
- GPS timing works

## Your Task

**Find where the signal disappears in the processing chain.**

The chain is in `tools/wwv_scan.c`, function `on_samples()`:

```c
// STEP 1: Raw samples from SDR
xi[0], xq[0]  // Should be non-zero, varying int16 values

// STEP 2: Decimation (2MHz → 48kHz)
g_decim_buffer[i].i, .q  // Should be non-zero floats

// STEP 3: Envelope detection
float mag = sqrtf(I*I + Q*Q);  // Should be > 0

// STEP 4: DC blocking (remove carrier, keep modulation)
float ac = dc_block_process(&g_dc_block, mag);
// During tick: should show 1000 Hz oscillation
// Between ticks: should be ~0

// STEP 5: Bandpass filter (extract 1000 Hz)
float filtered = biquad_process(&g_bp_1000hz, ac);
// During tick: should be large
// Between ticks: should be ~0

// STEP 6: Energy accumulation
// tick_energy should be >> noise_energy
```

**Add debug printf at each step to find where it breaks.**

## Key Files

| File | What It Does | Debug Priority |
|------|--------------|----------------|
| `tools/wwv_scan.c` | Main scanner, has the bug | **HIGH** |
| `src/decimator.c` | Sample rate conversion | Medium |
| `src/sdr_stream.c` | SDR configuration | Medium |
| `test/test_dsp.c` | Unit tests (all pass) | Reference |

## Quick Debug Code

Add this to `on_samples()` in wwv_scan.c:

```c
static int debug_samples = 0;
if (debug_samples++ < 100 && debug_samples % 10 == 0) {
    // Check raw samples
    printf("RAW[%d]: xi=%d xq=%d count=%u\n",
           debug_samples, xi[0], xq[0], count);
}

// After decimation, inside the for loop:
static int debug_decim = 0;
if (debug_decim++ < 20) {
    printf("DECIM: I=%.1f Q=%.1f mag=%.4f ac=%.6f filt=%.6f\n",
           g_decim_buffer[i].i, g_decim_buffer[i].q,
           mag, ac, filtered);
}
```

## What Good Values Look Like

```
# Raw samples (int16, should vary)
RAW: xi=-1234 xq=567 count=65536

# After decimation (floats, should be non-trivial)
DECIM: I=0.0234 Q=-0.0156

# Envelope (always positive)
mag=0.0280

# After DC block (oscillates during tick, ~0 otherwise)
ac=0.000123  # or during tick: ac=0.015 (varying)

# After bandpass (big during tick, tiny otherwise)
filt=0.000001  # noise
filt=0.012345  # during 1000 Hz tick
```

## Build & Test

```powershell
.\build.ps1 -Target tools
.\bin\wwv_scan.exe -scantime 5
```

## DO NOT

- Rewrite the whole architecture
- Change the DSP algorithms (they're proven correct)
- Add new features

## DO

- Add debug output to find where signal is lost
- Check if raw samples look valid
- Check if decimator output is reasonable
- Find the exact step where modulation disappears

## Context You Need

- **WWV** = NIST time station, broadcasts 1000 Hz tick every second
- **Zero-IF** = Signal is centered at 0 Hz (DC), carrier shows as magnitude
- **AM demodulation** = sqrt(I² + Q²) gives envelope
- **The tick** = 5ms burst of 1000 Hz tone amplitude-modulated on carrier
- **GPS sync** = We know exactly when each second starts
