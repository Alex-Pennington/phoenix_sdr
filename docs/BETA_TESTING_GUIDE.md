# Phoenix Nest Modem - Beta Testing Guide

**For Amateur Radio Operators and MARS Members**

---

## What This Is

Phoenix Nest is an open-source MIL-STD-188-110A HF data modem. This guide shows you how to test the **receive chain** using an SDRplay receiver and any standard 110A transmitter.

## What You Need

### Hardware
| Item | Notes |
|------|-------|
| SDRplay RSP2 Pro (or compatible) | Other SDRplay models may work |
| HF antenna | Connected to SDR |
| Computer running Windows 10/11 | Linux support planned |

### Software
| Item | Source |
|------|--------|
| Phoenix Nest Modem | [GitHub releases](https://github.com/Alex-Pennington/pennington_m110a_demod/releases) |
| phoenix_sdr | [GitHub releases](https://github.com/Alex-Pennington/phoenix_sdr/releases) |
| SDRplay API 3.x | [SDRplay downloads](https://www.sdrplay.com/downloads/) |

### Signal Source (any one of these)
| Option | Description |
|--------|-------------|
| MSDMT | Windows MIL-STD-188-110A modem (freeware) |
| QTMSDMT | Cross-platform version of MSDMT |
| Another 110A station | Over-the-air signals from MARS, military, etc. |
| Test recordings | .iqr files shared by other testers |

---

## Quick Start

### Step 1: Install SDRplay API

1. Download SDRplay API 3.x from [sdrplay.com/downloads](https://www.sdrplay.com/downloads/)
2. Run the installer
3. Connect your RSP2 Pro via USB
4. Verify it shows up in SDRplay's test utility

### Step 2: Download Phoenix Nest

```
Option A: Download release binaries (recommended for testing)
Option B: Build from source (see BUILD.md)
```

### Step 3: Capture a Test Signal

**Using a local transmitter (MSDMT):**

1. Set up MSDMT to transmit on a test frequency
2. Run phoenix_sdr to capture:
   ```
   phoenix_sdr.exe -f 7.074 -o capture -d 30
   ```
3. This captures 30 seconds of I/Q data at the specified frequency

**Using over-the-air signals:**

1. Tune to a known 110A frequency (MARS nets, etc.)
2. Capture when you hear activity:
   ```
   phoenix_sdr.exe -f 14.074 -o ota_capture -d 60
   ```

### Step 4: Decode the Capture

```
phoenix_modem.exe --iq-input capture.iqr --mode 75S
```

If successful, you'll see decoded data in the output.

---

## Supported Modes

Phoenix Nest supports all 12 MIL-STD-188-110A modes:

| Mode | Data Rate | Interleaver | Best For |
|------|-----------|-------------|----------|
| 75S | 75 bps | Short | Weak signals |
| 75L | 75 bps | Long | Very weak signals |
| 150S | 150 bps | Short | Poor conditions |
| 150L | 150 bps | Long | Very poor conditions |
| 300S | 300 bps | Short | Moderate conditions |
| 300L | 300 bps | Long | Moderate-poor |
| 600S | 600 bps | Short | Good conditions |
| 600L | 600 bps | Long | Good-moderate |
| 1200S | 1200 bps | Short | Very good conditions |
| 1200L | 1200 bps | Long | Good conditions |
| 2400S | 2400 bps | Short | Excellent conditions |
| 2400L | 2400 bps | Long | Very good conditions |

**Tip:** Start with 75S or 150S for initial testing - they're most forgiving.

---

## SDR Settings

### Recommended RSP2 Pro Configuration

| Parameter | Value | Notes |
|-----------|-------|-------|
| Sample Rate | 2 MSPS | Required |
| Center Frequency | Your target freq | e.g., 7074000 for 7.074 MHz |
| Bandwidth | 200 kHz | Narrowest available |
| IF Mode | Zero IF | Baseband I/Q |
| AGC | OFF | Manual gain recommended |
| Gain Reduction | 40 dB | Adjust for ~-20 dBFS peaks |
| LNA State | 4 | Mid-range, adjust as needed |

### Frequency Examples

| Band | Frequency | Typical Use |
|------|-----------|-------------|
| 40m | 7.074 MHz | FT8 (test signals) |
| 20m | 14.074 MHz | FT8 (test signals) |

---

## Testing Scenarios

### Scenario 1: Local Loopback (Easiest)

Test without RF - uses sound card or virtual audio cable.

1. Run MSDMT transmitting to a WAV file or VAC
2. Configure Phoenix Nest to receive from same source
3. Send test message, verify decode

### Scenario 2: RF Loopback (Recommended)

Transmit and receive on same computer with RF isolation.

1. Connect MSDMT to a transmitter (low power, dummy load)
2. Have SDR pick up the signal (antenna or coupled)
3. Capture with phoenix_sdr
4. Decode with Phoenix Nest

### Scenario 3: Over-the-Air (Real World)

Receive actual 110A signals from other stations.

1. Monitor known 110A frequencies
2. Capture when you detect activity
3. Try to decode - note the mode if known

---

## Troubleshooting

### "No signal detected"

- Check SDR is receiving (use SDRuno or similar to verify)
- Verify frequency is correct (110A signals are ~3 kHz wide)
- Try adjusting gain - too much or too little causes problems
- Ensure the signal is actually MIL-STD-188-110A (not ALE, STANAG 4285, etc.)

### "Decoding fails"

- Try a different mode (signal might not be the mode you selected)
- Check signal strength - very weak signals need 75S/75L
- Verify the capture file isn't corrupted

### "SDR not found"

- Reinstall SDRplay API
- Check USB connection
- Ensure no other program is using the SDR

---

## Sharing Test Results

Help improve Phoenix Nest by sharing your results!

### What to Report

1. **Success reports:** Mode, frequency, signal conditions, SNR if known
2. **Failure reports:** Same info plus the .iqr capture file if possible
3. **Suggestions:** What would make testing easier?

### Where to Report

- GitHub Issues: [pennington_m110a_demod/issues](https://github.com/Alex-Pennington/pennington_m110a_demod/issues)
- Include "Beta Test" in the title

### Sharing .iqr Files

Small captures (<50MB) can be attached to GitHub issues. Larger files can be shared via cloud storage links.

---

## Legal Notes

- **Amateur Radio:** 110A is legal on amateur bands in many jurisdictions for experimentation. Check your local regulations.
- **MARS/Military:** If you're a MARS member, you already know the rules.
- **Receive-Only:** Anyone can receive and decode - no license required for listening.

---

## Getting Help

- **GitHub Discussions:** Ask questions, share experiences
- **Issues:** Report bugs or problems
- **Documentation:** Check the `/docs` folder for technical details

---

## Version History

| Version | Date | Notes |
|---------|------|-------|
| 0.1-beta | Dec 2025 | Initial beta testing release |

---

*Thank you for testing Phoenix Nest! Your feedback helps make open-source HF data modems better for everyone.*
