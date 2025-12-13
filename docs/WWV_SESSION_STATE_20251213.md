# WWV Timing System - Comprehensive State Analysis
# Phoenix Nest MARS Suite
# Session Export: 2025-12-13 ~09:30 EST
# Purpose: Context preservation across PC reboot / MCP restart

---

## EXECUTIVE SUMMARY

**What Works:**
- Minute marker detection: ~40ms accuracy (EXCEEDS 500ms MARS target)
- GPS timing integration: ~10-20ms after latency compensation  
- NTP sync: ~4ms precision
- Chain detection: Reliably finds 4+ consecutive minute markers
- Sec 29/59 silence detection: Working (WWV signature validation)

**What Doesn't Work Yet:**
- BCD time decode: Unreliable pulse width measurement due to weak/faded signal
- Cannot reliably determine WHICH minute we're looking at (only WHERE minute boundaries are)

**Why We're Blocked:**
- Test recordings have insufficient signal quality for BCD pulse width discrimination
- Need fresh recordings with verified good RF conditions

---

## SYSTEM ARCHITECTURE

### Hardware Stack
```
[SDRplay RSP2 Pro] → [phoenix_sdr.exe] → [.iqr files]
                                              ↓
[Arduino NEO-6M GPS] → [COM6] → [gps_time.exe] → timing reference
                                              ↓
                                    [wwv_sync.exe] → minute detection
                                              ↓
                                    [wwv_gps_verify.exe] → validation
```

### Timing Sources & Precision
| Source | Measured Precision | Notes |
|--------|-------------------|-------|
| NTP (time.nist.gov) | ~4ms | Used for recording timestamps |
| GPS PPS (NEO-6M) | ~10-20ms | 302ms serial latency compensated |
| WWV minute detection | ~40ms | Relative to recording start |
| **Combined** | **<100ms** | **Well under 500ms MARS target** |

### Key Files
```
D:\claude_sandbox\phoenix_sdr\
├── bin\
│   ├── phoenix_sdr.exe      # Main SDR recorder
│   ├── wwv_sync.exe         # WWV analysis (minute detection + BCD)
│   ├── wwv_gps_verify.exe   # GPS-WWV cross-validation
│   ├── gps_time.exe         # GPS serial reader
│   └── *.exe                # Other tools
├── tools\
│   ├── wwv_sync.c           # ~1700 lines, main WWV analyzer
│   ├── wwv_gps_verify.c     # GPS verification tool
│   └── gps_time.c           # GPS reader
├── src\
│   ├── main.c               # phoenix_sdr main app
│   ├── ntp_time.c           # NTP client
│   └── *.c                  # SDR/decimator/audio modules
├── docs\
│   ├── WWV_TRACKER.md       # Development progress tracker
│   └── WWV_SESSION_STATE_20251213.md  # THIS FILE
└── *.iqr                    # Test recordings
```

---

## WHAT'S WORKING: MINUTE MARKER DETECTION

### Algorithm (wwv_sync.c)
1. **Envelope extraction** at 1ms resolution (biquad bandpass OR Goertzel)
2. **Baseline computation** with trailing window (avoids self-pollution)
3. **Adaptive threshold** (mean + 1.5σ from reference window)
4. **Candidate detection** for 400-1100ms duration rises
5. **Chain building** - groups candidates with 60s spacing
6. **Refined edge detection** - gradient analysis for sub-ms precision
7. **Weighted averaging** across chain members
8. **Validation** - checks sec 29/59 silence (WWV signature)

### Typical Output (Good Detection)
```
BEST CHAIN SELECTED (Chain 1 of 1)
  Chain length: 4 markers
  Score: 81.7 / 100
  Sec 59 silent: 4/4 (100%)
  Sec 29 silent: 3/4 (75%)
  Avg duration error: 11 ms (from 800ms)
  
TIMING RESULT
  First minute boundary: 21.351 sec into recording
  Offset from file start to minute: 21351.0 ms
  Quality: EXCELLENT - High confidence lock
```

### Detection Modes
| Mode | Flag | Best For |
|------|------|----------|
| Biquad bandpass | (default) | Strong signals |
| Goertzel DFT | `-g` | Sharper edge detection |
| Wideband | `-w` | Very strong signals |
| Custom freq | `-f 600` | 5MHz (600Hz tone) |

---

## WHAT'S NOT WORKING: BCD TIME DECODE

### The Problem
WWV encodes time in pulse widths:
- **200ms** = binary 0
- **500ms** = binary 1  
- **800ms** = position marker
- **Silent** = sec 29, 59

Our recordings show:
```
Pulse durations (ms):
  Sec 0: dur=849   ← Should be ~800ms marker ✓
  Sec 1: dur=850   ← Should be 200 or 500ms ✗ (wrong!)
  Sec 2: dur=850   ← Should be 200 or 500ms ✗ (wrong!)
  ...
  (almost everything reads as ~850ms)
```

### Root Cause Analysis

**Issue 1: Envelope Never Settles**
The biquad envelope follower has slow decay. Between pulses, it never drops to true baseline. Result: backward scan from 850ms finds "signal" all the way back to 0ms.

**Issue 2: Low Signal-to-Noise**
5MHz WWV recordings show contrast ratios of 1.2-2.0x (marginal). Good decode needs 3-5x contrast. HF fading causes:
- Varying amplitude across the minute
- Some seconds completely washed out
- Edge detection finding noise, not pulse boundaries

**Issue 3: Wrong Approach for This Signal**
The current algorithm tries to find pulse EDGES (rising/falling). But with fading:
- Edges are smeared over 50-100ms
- Noise creates false edges
- Threshold crossings happen at wrong times

### Attempted Fixes (All Insufficient)
1. **Forward scan from tick** - finds noise spikes instead of pulse end
2. **Backward scan from 850ms** - never finds true baseline
3. **Energy ratio method** - better, but most seconds show as SILENT
4. **Goertzel mode** - sharper, but same fundamental issues
5. **Adaptive thresholds** - helps, but can't fix bad SNR

### What Would Help
| Approach | Why |
|----------|-----|
| Better RF input | Higher SNR = cleaner edges |
| 10 or 15 MHz | Better propagation than 5MHz |
| Higher gain | More signal (if not clipping) |
| Longer integration | Average across multiple minutes |
| Synchronous detection | Correlate against expected waveform |

---

## TEST RECORDINGS AVAILABLE

| File | Freq | Duration | Quality | Notes |
|------|------|----------|---------|-------|
| wwv_600_48k.iqr | 10MHz | 600s | FAIR | Main test file, some fading |
| wwv_5Mhz_g40_1234_48k.iqr | 5MHz | ? | POOR | Heavy fading, 600Hz tone |
| wwv_5MHz_gain30_48k.iqr | 5MHz | ? | POOR | Lower gain attempt |
| wwv_gain40_48k.iqr | ? | ? | ? | Higher gain |
| Various others | - | - | - | In project root |

**Problem:** None have verified "good" signal quality by ear confirmation.

---

## RECOMMENDED NEXT STEPS

### Immediate (After Reboot)
1. **Make fresh recording with verified good signal**
   - Tune SDRuno to 10 or 15 MHz WWV
   - Listen - confirm clear ticks audible
   - Check waterfall - clean carrier visible
   - Note exact UTC time when starting record
   - Run: `phoenix_sdr -f 10.0 -d 180 -o wwv_good -m`

2. **Test minute detection on new recording**
   ```
   wwv_sync wwv_good_48k.iqr 0 180
   ```

3. **If minute detection works, try BCD decode**
   - Look at pulse durations in output
   - Compare to expected WWV format
   - Note which seconds are readable vs garbage

### For BCD Decode Success
Need recordings where:
- [ ] Ticks clearly audible by ear
- [ ] Contrast ratio > 3x in most seconds  
- [ ] Position markers (sec 0,9,19,39,49) show ~800ms
- [ ] Data bits (sec 1-8, etc.) show 200ms OR 500ms, not 850ms
- [ ] Sec 29 and 59 show near-zero energy

### Alternative Approach (If BCD Stays Broken)
For MARS timing discipline, we may not NEED BCD decode:
- Minute marker detection gives us 40ms precision
- GPS gives us absolute time
- Combine: GPS tells us WHICH minute, WWV tells us WHERE the boundary is
- Result: Sub-100ms timing without BCD

---

## ITEMS TO REVISIT

1. **5-minute watchdog timer** - Re-implement for chat interactions
2. **Diversity update** - Rayven has info to share
3. **BCD algorithm** - May need complete redesign (synchronous detection?)
4. **Live testing workflow** - wwv_live_test.bat needs GPS integration

---

## CONTEXT FOR NEXT SESSION

### What Claude Was Doing
Working on BCD pulse width measurement in `wwv_sync.c`. Tried multiple approaches:
- Edge detection (rising/falling threshold crossings)
- Energy ratio (compare early/mid/late portions of each second)
- Backward scan from 850ms to find pulse end

All failed because test recordings have insufficient SNR.

### What Rayven Noted
- Has live receiver with good antenna available
- Can verify signal quality by ear and screenshot
- Multiple recordings exist but quality unknown
- This is Rayven's domain of expertise (15+ years MARS)
- Want to discuss approach before more blind testing

### Key Insight
We were iterating on algorithm when the real problem is **input signal quality**. Need to:
1. Get known-good recording
2. THEN tune algorithm
3. NOT: tune algorithm against bad data

### Build Command
```powershell
cd D:\claude_sandbox\phoenix_sdr
.\build.ps1 -Target tools
```

### Test Commands
```bash
# Minute detection (working)
wwv_sync wwv_good_48k.iqr 0 180

# With Goertzel
wwv_sync wwv_good_48k.iqr 0 180 -g

# GPS verification
wwv_gps_verify -t 2025-12-13T10:00:00 -o 21350.0 -p COM6

# GPS time check
gps_time COM6 30
```

---

## GIT STATUS

Branch: `feature/wwv-timing`

Recent work is in working tree, may need commit:
```bash
git status
git add -A
git commit -m "WWV BCD decode work in progress - needs better RF input"
```

---

*Document created for session continuity across PC reboot.*
*Resume by reading this file + WWV_TRACKER.md*
