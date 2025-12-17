# GPS Precision Timing for HF SDR Applications

## Complete Chat Reference Guide

---

## 1. KYCORS Real Time Network Subscription Details

The following subscription to the KYCORS Real Time Network Service has been activated. If there is no activity over the course of 365 days for this login, the subscription and the login will be deleted.

The KYCORS service provides real time corrections based on NAD83(2011) realization. Orthometric heights will be determined by the Geoid you select in your software.

### Connection Details

| Setting | Value |
|---------|-------|
| **NTRIP Caster** | KYCORS.KY.GOV |
| **Port** | 2101 |
| **Datum** | NAD83(2011) |

### Available Mountpoints

**VRS (GPS & GLONASS):**
- `VRS_CMRx`
- `VRS_CMRplus`
- `VRS_RTCM3`
- `VRS_RTCM3-2`

**RTX (GPS, GLONASS, GALILEO, & BEIDOU):**
- `RTX_CMRx`
- `RTX_RTCM3-2`

### Key Notes
- **No single base mountpoints** are provided
- Orthometric heights depend on the **geoid model** you select in your rover/software
- Account will be **deleted after 365 days of inactivity**

---

## 2. GNSS and HF Time-Frequency Analysis

### What KYCORS Actually Provides
- **RTK corrections** for precise positioning (cm-level accuracy)
- **NAD83(2011) datum** reference for surveying
- Not timing signals or frequency standards directly

### How GNSS *Can* Support HF Signal Analysis

| Application | How GNSS Helps |
|-------------|----------------|
| **Precise Timestamping** | GNSS receivers provide UTC time (±nanoseconds) to timestamp HF signal captures |
| **Frequency Reference** | GNSS-disciplined oscillators (GPSDOs) provide 10 MHz / 1 PPS references |
| **Ionospheric Modeling** | GNSS TEC data helps predict HF propagation conditions |
| **Distributed Receiver Sync** | GNSS timing synchronizes multiple HF receivers for TDOA/geolocation |

### For HF Time-Frequency Domain Analysis, You'd Actually Need:

#### 1. GNSS-Disciplined Oscillator (GPSDO)
- Provides stable 10 MHz reference and 1 PPS timing pulse
- Use as clock reference for your SDR or spectrum analyzer

#### 2. Software-Defined Radio (SDR) + Analysis Software
```
HF Antenna → SDR (with GPSDO reference) → Software Analysis
                                              ↓
                                    - FFT / Spectrogram
                                    - Short-Time Fourier Transform
                                    - Wavelet Analysis
                                    - Cyclostationary Analysis
```

#### 3. Common Tools for Time-Frequency Analysis
- **GNU Radio** — open-source SDR framework
- **Python + SciPy/NumPy** — `scipy.signal.spectrogram`, `scipy.signal.stft`
- **MATLAB** — Signal Processing Toolbox
- **Baudline** — real-time spectrum analyzer

### Example: Python Time-Frequency Analysis of HF Signal

```python
import numpy as np
from scipy import signal
import matplotlib.pyplot as plt

# Simulated HF signal capture (timestamped via GNSS)
fs = 48000  # Sample rate
t = np.linspace(0, 1, fs)
hf_signal = np.sin(2 * np.pi * 7000 * t) + 0.5 * np.sin(2 * np.pi * 14000 * t)

# Short-Time Fourier Transform (spectrogram)
f, t_spec, Sxx = signal.spectrogram(hf_signal, fs, nperseg=1024)

plt.pcolormesh(t_spec, f, 10 * np.log10(Sxx), shading='gouraud')
plt.ylabel('Frequency [Hz]')
plt.xlabel('Time [sec]')
plt.title('HF Signal Time-Frequency Representation')
plt.colorbar(label='Power [dB]')
plt.show()
```

---

## 3. NEO-6M GPS Module and Timing

### What Each Component Provides

| Component | Timing Accuracy | Role |
|-----------|-----------------|------|
| **NTP (internet)** | ~1-50 ms | Coarse time sync |
| **u-blox NEO-6M** | **~30-60 ns** (via 1 PPS) | Precise timing pulse |
| **KYCORS RTK** | Improves position → slightly better timing | Position corrections |

### The Key: NEO-6M's 1 PPS Output

Your NEO-6M has a **1 PPS (Pulse Per Second)** pin that outputs a very accurate pulse aligned to GPS time. This is your precision timing source — **not the NMEA serial data** (which has latency).

```
NEO-6M Pinout:
┌─────────────┐
│  VCC  GND   │
│  TX   RX    │
│  PPS  ←──── This is your precision timing signal!
└─────────────┘
```

### How KYCORS *Could* Help (But Probably Won't Much for Timing)

KYCORS improves **position accuracy**, which slightly refines the timing solution since position and time are coupled in GNSS. However:

- NEO-6M is a **single-frequency L1-only** receiver
- It can't fully utilize the dual-frequency RTK corrections
- The timing improvement from RTK would be marginal

**Bottom line:** For timing, focus on the **1 PPS signal** from your NEO-6M.

### Setting Up Precision Timing on Windows

#### Option 1: Use Meinberg NTP + PPS (Recommended)

1. **Hardware needed:** USB-to-Serial adapter or direct serial connection from NEO-6M
2. **Connect the 1 PPS** line to the DCD or CTS pin of your serial port
3. Install **Meinberg NTP for Windows**: https://www.meinbergglobal.com/english/sw/ntp.htm
4. Configure `ntp.conf`:

```conf
# Serial GPS for time-of-day (coarse)
server 127.127.20.0 mode 16 minpoll 4 maxpoll 4
fudge 127.127.20.0 flag1 1 time2 0.0

# PPS from serial port (precision)
server 127.127.22.0 minpoll 4 maxpoll 4 prefer
fudge 127.127.22.0 flag3 1
```

#### Option 2: BktTimeSync (Simpler)

A lightweight tool specifically for GPS timing on Windows:
- https://www.obriensweb.com/bkttimesync/

#### Option 3: GPSd + Chrony (If Using Linux)

```bash
# Install
sudo apt install gpsd gpsd-clients chrony pps-tools

# Test PPS
sudo ppstest /dev/pps0

# chrony.conf
refclock SHM 0 offset 0.5 delay 0.2 refid NMEA noselect
refclock PPS /dev/pps0 lock NMEA refid PPS prefer
```

### Expected Accuracy

| Setup | Accuracy |
|-------|----------|
| NTP only | 1-50 ms |
| NEO-6M NMEA (no PPS) | ~100-500 ms |
| **NEO-6M with 1 PPS** | **~1 µs** (after discipline) |
| High-end GPSDO | ~10-50 ns |

---

## 4. The USB Latency Problem

### The Fundamental Problem

USB ruins precision timing.

| Interface | Latency | Jitter | PPS Usable? |
|-----------|---------|--------|-------------|
| Native Serial (legacy) | ~1 µs | Very low | ✅ Yes |
| **USB Serial Adapter** | 1-16 ms | High | ❌ No |
| USB (polling) | 1-10 ms | Variable | ❌ No |

USB is **polled**, not interrupt-driven. Even if you wire the PPS to a USB-serial adapter's DCD pin, you're adding 1-16 ms of unpredictable latency — worse than NTP!

### Your Realistic Options

#### Option 1: Accept USB Latency, Use NMEA Time (~1-10 ms accuracy)
Just use the NEO-6M's NMEA time sentences over USB. Tools like **BktTimeSync** or **NMEATime2** can sync Windows to ~1-10 ms. Better than NTP, but not microsecond-level.

**This is probably fine for HF signal timestamping.**

---

#### Option 2: PCIe Serial Card (~1 µs accuracy)
Install a **real serial port** via PCIe card with hardware interrupts:
- StarTech PEX1S953LP (~$30)
- Wire NEO-6M's PPS to the DCD pin
- Now PPS works properly with Meinberg NTP

---

#### Option 3: Raspberry Pi as Network Time Server (~1-10 µs)
Use a **Raspberry Pi with GPIO** to capture PPS, then serve time over your network:

```
NEO-6M PPS → Pi GPIO → gpsd + chrony → NTP/PTP to your PC
```

Your Windows PC syncs to the Pi via NTP/PTP. This is a popular and inexpensive solution.

---

#### Option 4: USB GPS with Internal Timepulse Discipline
Some GPS modules discipline their NMEA output internally and reduce USB jitter effects:
- **u-blox NEO-M8T** (timing-focused)
- **GlobalSat BU-353S4** (internally compensated)

Still ~1-5 ms, but more consistent.

---

#### Option 5: Dedicated Network Time Appliance
Devices that handle PPS internally and serve PTP/NTP:
- **Meinberg microSync** (expensive)
- **TimeHat + Raspberry Pi** (~$50)
- **Microsemi/Orolia** (enterprise)

---

### Practical Recommendation

| Budget | Solution | Expected Accuracy |
|--------|----------|-------------------|
| **$0** | BktTimeSync + NEO-6M over USB | ~5-20 ms |
| **~$30** | PCIe serial card + PPS wiring | ~1-5 µs |
| **~$50** | Raspberry Pi + GPS HAT as NTP server | ~1-10 µs |

---

## 5. BktTimeSync vs Raspberry Pi GPS Time Server

| Aspect | **BktTimeSync (Windows)** | **Raspberry Pi + GPS** |
|--------|---------------------------|------------------------|
| **Accuracy** | ~5-20 ms | **~1-10 µs** (1000x better) |
| **Setup Time** | ~5 minutes | ~1-2 hours |
| **Cost** | Free | ~$15-50 (GPS HAT or wiring) |
| **Complexity** | Download → Run → Done | Linux config, wiring, gpsd, chrony |
| **PPS Support** | ❌ No (USB kills it) | ✅ Yes (GPIO is interrupt-driven) |
| **Serves Other Devices** | ❌ No | ✅ Yes (NTP server for whole network) |
| **Reliability** | Depends on USB/Windows | Runs headless 24/7 |
| **Your NEO-6M** | Connects via USB | Connects via GPIO (3.3V serial + PPS) |

---

### BktTimeSync — Quick & Easy

#### Pros
- ✅ Running in 5 minutes
- ✅ No hardware changes
- ✅ No Linux knowledge needed
- ✅ Works with your existing USB GPS setup

#### Cons
- ❌ USB latency limits you to ~5-20 ms
- ❌ Only syncs the one PC
- ❌ Windows scheduler can interfere
- ❌ No true PPS support

#### Best For
- Quick improvement over NTP
- "Good enough" timestamping
- Testing before committing to more work

---

### Raspberry Pi GPS Time Server — Precision

#### Pros
- ✅ **True microsecond accuracy** via GPIO PPS
- ✅ Serves time to your whole network (all PCs, SDRs, etc.)
- ✅ Runs 24/7 headless
- ✅ You already have the hardware
- ✅ Uses your existing NEO-6M

#### Cons
- ❌ Requires wiring (4 wires: VCC, GND, TX, PPS)
- ❌ Linux configuration (gpsd, chrony)
- ❌ Initial setup takes 1-2 hours
- ❌ Another device to maintain

#### Best For
- Serious timing work
- Multiple devices needing sync
- HF recording with precise timestamps
- Long-term reliable operation

---

### Raspberry Pi Wiring for NEO-6M

```
NEO-6M          Raspberry Pi GPIO
───────         ─────────────────
VCC  ──────────  Pin 1 (3.3V)
GND  ──────────  Pin 6 (GND)
TX   ──────────  Pin 10 (GPIO 15 / RXD)
PPS  ──────────  Pin 12 (GPIO 18) ← This is the magic
```

```
┌─────────────────────────────────────┐
│  Raspberry Pi GPIO Header           │
│  ┌─┬─┐                              │
│  │1│2│  ← 3.3V (Pin 1)              │
│  ├─┼─┤                              │
│  │ │ │                              │
│  ├─┼─┤                              │
│  │5│6│  ← GND (Pin 6)               │
│  ├─┼─┤                              │
│  │ │ │                              │
│  ├─┼─┤                              │
│  │ │10│ ← RXD (Pin 10)              │
│  ├─┼─┤                              │
│  │11│12│ ← GPIO 18/PPS (Pin 12)     │
│  └─┴─┘                              │
└─────────────────────────────────────┘
```

---

### Quick Decision Guide

| If you need... | Use |
|----------------|-----|
| "Better than nothing" right now | BktTimeSync |
| <1 ms accuracy | Raspberry Pi |
| Sync multiple devices | Raspberry Pi |
| Minimal effort | BktTimeSync |
| Long-term reliable solution | Raspberry Pi |
| To timestamp HF recordings | Raspberry Pi |

---

## 6. The Real Problem: SDR USB Latency

### The Deeper Issue

```
Antenna → SDRplay → USB → Windows → Software
              ↑           ↑           ↑
           Unknown    Variable    Scheduling
           Latency    Latency      Jitter
              └──────────┴───────────┘
                "When did this sample
                 ACTUALLY arrive?"
```

You have **no ground truth**. The system clock is irrelevant because:
1. USB latency from SDRplay: **variable ~1-10 ms**
2. USB buffer depth: **unknown delay**
3. OS scheduling: **more jitter**
4. Software processing: **more delay**

**Even a perfect clock doesn't tell you when the RF hit the antenna.**

---

## 7. Solutions: Get Time Reference INTO the RF Domain

### Option 1: Inject PPS Into Your RF Path (Cheapest)

Bring the GPS PPS into the **same sample stream** as your signal. Then the PPS edge IS your time reference at sample-level precision.

```
                        ┌──────────────────┐
                        │   Your HF Antenna │
                        └────────┬─────────┘
                                 │ (picks up both)
                                 ↓
┌─────────┐    ~1ft     ┌─────────┐
│ NEO-6M  │──PPS──→ 🔘 │ Small   │  ← Just a wire stub or 
│   GPS   │    pulse    │ radiator│     loop near the antenna
└─────────┘             └─────────┘
                             │
        Proximity coupled ───┘
        (near-field)
```

**How:**
- Use a simple resistor combiner or RF coupler
- Inject PPS as a pulse in your passband (or a subharmonic)
- Every second, you see a spike in your IQ — that's your time mark
- Now all samples are referenced to GPS time ±1 sample period

---

### Option 2: Two-Channel SDR (Same Clock)

Some SDRs have 2 coherent inputs sharing the same clock:
- **RTL-SDR Blog V4** (single, but you could use two synced)
- **Airspy HF+ Discovery** (single channel)
- **SDRplay RSPduo** ← **You might want this** (2 coherent tuners!)

With RSPduo:
```
Channel A: HF signal
Channel B: GPS PPS (or a PPS-modulated carrier)
```
Both channels share the same sample clock — **no relative jitter**.

---

### Option 3: SDR with Built-in GPSDO/Timing

Professional SDRs with hardware timestamping:

| SDR | GPS/Timing Support | Cost |
|-----|-------------------|------|
| **Ettus USRP B210** | External GPSDO input, hardware timestamps | ~$1,300 |
| **LimeSDR** | External reference input | ~$300 |
| **Red Pitaya** | PPS input, sample-accurate timestamps | ~$500 |
| **ADALM-PLUTO** (modded) | External clock reference | ~$200 |

These embed timestamps **at the FPGA level**, before USB.

---

### Option 4: Raspberry Pi + Direct ADC (Bypass USB Entirely)

Use the Pi's **GPIO for PPS** and an **I2S or SPI ADC** for RF:

```
HF Signal → ADC (SPI) → Pi GPIO (DMA) → Sample buffer
GPS PPS   → GPIO 18   → Interrupt → Marks sample index
```

No USB. PPS interrupt marks exact sample number. The Pi's DMA captures samples deterministically.

**Limitation:** Max ~1-2 MHz bandwidth via SPI ADC.

---

### Option 5: WWVB/WWV as Your Reference

Since you mentioned timing, use a **known broadcast time signal**:

- **WWV** (2.5, 5, 10, 15, 20 MHz) — HF time signal
- **WWVB** (60 kHz) — LF time signal

Record WWV with your SDR. The tick marks are at known times. **Cross-correlate to find your absolute offset.**

```
Your IQ recording:  [.......TICK.......TICK..........]
Known WWV timing:          │          │
                           00:00:00   00:00:01
                                  ↑
                      Calculate your latency
```

This tells you your **total system delay** — USB, buffers, everything.

---

### Practical Recommendation

| If you have... | Do this |
|----------------|---------|
| **SDRplay RSPdx/RSPduo** | Use Duo's 2nd channel for PPS carrier |
| **Single-channel SDR** | Inject PPS pulse via RF combiner |
| **Access to WWV** | Record it, cross-correlate for calibration |
| **Budget for upgrade** | Get RSPduo or USRP with GPSDO |

### Quickest Experiment: WWV Calibration

1. Tune SDRplay to **10 MHz (WWV)**
2. Record 10 seconds of IQ
3. The "tick" at the top of each second is your reference
4. Measure when the tick appears in your samples
5. **That's your total system latency**

---

## 8. The "Leaky PPS" Approach — Proximity Coupled Injection

No splitters, no cables, no RF plumbing. Just radiate the PPS near your antenna.

```
                    ┌──────────────────┐
                    │   Your HF Antenna │
                    └────────┬─────────┘
                             │ (picks up both)
                             ↓
┌─────────┐    ~1ft     ┌─────────┐
│ NEO-6M  │──PPS──→ 🔘 │ Small   │  ← Just a wire stub or 
│   GPS   │    pulse    │ radiator│     loop near the antenna
└─────────┘             └─────────┘
                             │
        Proximity coupled ───┘
        (near-field)
```

### Simplest Implementation

#### What You Need
- NEO-6M PPS output (3.3V pulse, ~100ms wide typically)
- A short wire (6-12 inches) as a "radiator"
- Maybe a resistor to limit current

#### Basic Circuit

```
NEO-6M PPS pin ────┬──── [100Ω - 1kΩ] ────── Short wire antenna (6-12")
                   │                              │
                  GND                         (open end, radiates)
```

That's it. The PPS is a sharp edge, rich in harmonics. Your SDR will see a broadband "click" every second.

### What You'll See in the SDR

```
Time (samples) ──────────────────────────────────────────────►

Amplitude:     ~~~~▂▃▅█▅▃▂~~~~▂▃▅█▅▃▂~~~~▂▃▅█▅▃▂~~~~
                    ↑           ↑           ↑
                 PPS pulse   PPS pulse   PPS pulse
                 (broadband   (exactly    (your time
                  spike)      1 second)    reference)
```

In the **spectrogram/waterfall**, you'll see a vertical line flash across all frequencies every second.

### Making It Even Cleaner

#### Option A: Differentiate the Pulse (Sharper Edge)
The PPS is typically a ~100ms wide pulse. The **rising edge** is your time mark. A simple RC differentiator sharpens it:

```
PPS ───┤├──┬─── To wire antenna
   100pF   │
          ─┴─ 10kΩ
          GND
```

Now you get a **sharp spike** instead of a square pulse.

#### Option B: Modulate a Carrier (If You Want It on a Specific Frequency)
If you want the PPS to appear at a **specific frequency** instead of broadband:

```
PPS ────→ [Gate a simple oscillator] ────→ Wire antenna

Example: 555 timer or crystal oscillator, keyed by PPS
         Puts a short "beep" at a known frequency
```

### Adjusting the Level

You want the PPS spike to be:
- **Visible** in your IQ stream
- **Not overwhelming** your actual signal

Adjust by:
1. **Distance:** Move the wire closer/farther from your antenna
2. **Resistor value:** Higher resistance = weaker signal
3. **Wire length:** Shorter = less coupling

Start with the wire 1-2 feet from your SDR antenna and adjust.

### Detecting It in Software

Once it's in your IQ stream, detect the pulse:

```python
import numpy as np

def find_pps_edges(iq_samples, sample_rate, threshold=0.8):
    """Find PPS pulse locations in IQ stream"""
    # Compute envelope (magnitude)
    envelope = np.abs(iq_samples)
    
    # Simple threshold detection
    peak = np.max(envelope)
    threshold_level = peak * threshold
    
    # Find rising edges
    above = envelope > threshold_level
    edges = np.where(np.diff(above.astype(int)) == 1)[0]
    
    # Convert sample indices to times
    times = edges / sample_rate
    
    return edges, times

# Usage:
# pps_sample_indices, pps_times = find_pps_edges(iq_data, 2.4e6)
# Now pps_sample_indices[0] is the sample number where the first PPS occurred
# ALL samples are now referenced to GPS time!
```

### What This Gives You

| Measurement | Accuracy |
|-------------|----------|
| PPS edge timing (GPS) | ~30-60 ns |
| Your sample clock | 1 / sample_rate (e.g., 416 ns at 2.4 MHz) |
| **Your effective timing resolution** | **~1 sample period** |

At 2.4 MHz sample rate, you're resolving time to **~416 nanoseconds** — about **1000x better** than you had before.

---

## 9. Creating a Synthetic WWV Signal — PPS-Keyed 1800 Hz Tone

### Your Personal GPS Time Tick

```
Waterfall Display:

        Real WWV                     Your PPS Tick
           ↓                              ↓
 ─────────────────────────────────────────────────────
 │  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  │  ┊  ┊  │
 │  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  │  ┊  ┊  │
 │  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  ┊  │  ┊  ┊  │
 ─────────────────────────────────────────────────────
    500  600                            1800 Hz offset
    Hz   Hz                         (your GPS PPS tick)
```

### Simple Circuit: PPS-Keyed 1800 Hz Tone

#### Option 1: 555 Timer (Simplest)

```
         +5V (or 3.3V)
          │
          ┴ 0.01µF (bypass)
          │
    ┌─────┴─────┐
    │     8   4 │───── +V
    │           │
    │   555     │
    │           │
    │  2  6  7  │
    └──┬──┬──┬──┘
       │  │  │
       │  ├──┴──┬── 8.2kΩ ──┐
       │  │     │           │
       │  │    ─┴─ 0.01µF   │
       │  │    GND          │
       │  │                 │
       └──┴─────────────────┘
       │
       3 (output) ──── 10kΩ ──── Short wire antenna
       │
       1 ─── GND

    RESET (pin 4) ←── PPS from NEO-6M
```

**Frequency:** f = 1.44 / (R × C) ≈ 1.44 / (8.2kΩ × 0.01µF) ≈ **1750 Hz**

Tweak R to hit exactly 1800 Hz, or close enough.

**How it works:**
- PPS HIGH → 555 oscillates at 1800 Hz
- PPS LOW → 555 reset, no output
- Result: ~100ms burst of 1800 Hz every second

---

#### Option 2: ATtiny85 (More Precise, Programmable)

If you want exact frequency and adjustable pulse duration:

```cpp
// ATtiny85 - PPS triggered tone burst
#define PPS_PIN   2  // PPS input from GPS
#define TONE_PIN  0  // Output to antenna

void setup() {
  pinMode(PPS_PIN, INPUT);
  pinMode(TONE_PIN, OUTPUT);
}

void loop() {
  if (digitalRead(PPS_PIN) == HIGH) {
    // Generate 1800 Hz for 50ms at PPS rising edge
    tone(TONE_PIN, 1800);
    delay(50);  // 50ms tick duration (like WWV)
    noTone(TONE_PIN);
    
    // Wait for PPS to go low before re-arming
    while (digitalRead(PPS_PIN) == HIGH);
  }
}
```

---

#### Option 3: Just Use a Crystal Oscillator + Gate

If you have a specific frequency crystal:

```
PPS ────→ [74HC00 NAND gate] ←──── Crystal oscillator (e.g., 1.8432 MHz ÷ 1024)
               │
               ↓
          Wire antenna
```

---

### Tuning It to Your Display

Let's say you're tuned to **10 MHz (WWV)** with your SDR:

| Your SDR Center | Tone Carrier Freq | Audio Offset |
|-----------------|-------------------|--------------|
| 10.000 MHz | 10.0018 MHz | +1800 Hz |
| 5.000 MHz | 5.0018 MHz | +1800 Hz |
| 7.200 MHz | 7.2018 MHz | +1800 Hz |

**You can pick ANY offset.** 1800 Hz puts you above the WWV tones (500/600) but still in typical SSB audio passband.

Or go to **2100 Hz** to be even further away from voice/SSTV activity.

---

### Making It Look Exactly Like WWV

WWV's second tick is:
- **5 cycles of 1000 Hz** (5ms) at the top of each second
- 1200 Hz tone on the 29th and 59th second (minute markers)

If you want to mimic that precisely:

```cpp
// Mimic WWV 5ms tick at 1000 Hz, but at 1800 Hz
void loop() {
  if (digitalRead(PPS_PIN) == HIGH) {
    // 5ms burst of 1800 Hz (9 cycles)
    for (int i = 0; i < 9; i++) {
      digitalWrite(TONE_PIN, HIGH);
      delayMicroseconds(278);  // Half period of 1800 Hz
      digitalWrite(TONE_PIN, LOW);
      delayMicroseconds(278);
    }
    while (digitalRead(PPS_PIN) == HIGH);
  }
}
```

---

### What You'll See

Your SDR waterfall will show:

```
Time →

2100 Hz ─┼───────────────────────────────────────────
         │
1800 Hz ─┼──█──────────█──────────█──────────█────── ← Your PPS ticks
         │                                            (exactly 1 second apart)
1500 Hz ─┼───────────────────────────────────────────
         │
1000 Hz ─┼───────────────────────────────────────────
         │
 600 Hz ─┼──▄──────────▄──────────▄──────────▄────── ← WWV 600 Hz tone
         │
 500 Hz ─┼──▄──────────▄──────────▄──────────▄────── ← WWV 500 Hz tick
         │
       ──┴───────────────────────────────────────────
         0s         1s         2s         3s
```

**Your tick at 1800 Hz is your GPS-derived time reference, sitting right next to WWV as a sanity check.**

---

### Detection in Python

```python
import numpy as np
from scipy import signal

def detect_pps_ticks(iq_samples, sample_rate, tone_freq=1800, bandwidth=100):
    """Detect 1800 Hz PPS tick bursts in IQ stream"""
    
    # Bandpass filter around 1800 Hz
    nyq = sample_rate / 2
    low = (tone_freq - bandwidth/2) / nyq
    high = (tone_freq + bandwidth/2) / nyq
    b, a = signal.butter(4, [low, high], btype='band')
    filtered = signal.filtfilt(b, a, np.abs(iq_samples))
    
    # Envelope detection
    envelope = np.abs(signal.hilbert(filtered))
    
    # Find peaks (tick locations)
    threshold = np.max(envelope) * 0.5
    peaks, _ = signal.find_peaks(envelope, height=threshold, distance=sample_rate*0.5)
    
    return peaks  # Sample indices of each PPS tick

# Usage:
pps_samples = detect_pps_ticks(iq_data, 2.4e6, tone_freq=1800)
print(f"PPS ticks at samples: {pps_samples}")
print(f"Interval: {np.diff(pps_samples) / 2.4e6} seconds")  # Should be ~1.000000
```

---

## 10. Summary

| Component | Value |
|-----------|-------|
| Tone frequency | 1800 Hz (your choice) |
| Tick duration | 5-50 ms |
| Source | 555 timer or ATtiny |
| Trigger | NEO-6M PPS |
| Radiator | Short wire near SDR antenna |

You'll have a **GPS-synchronized timing tick in your IQ stream** that you can see alongside WWV, giving you both a **reference** and a **sanity check**.

---

## Quick Reference: Parts List

### For 555 Timer Version
- 1x 555 Timer IC (NE555 or similar)
- 1x 8.2kΩ resistor (adjust for exact frequency)
- 1x 10kΩ resistor (output limiting)
- 2x 0.01µF capacitor
- 6-12" wire for antenna
- NEO-6M GPS module (PPS output)

### For ATtiny85 Version
- 1x ATtiny85 microcontroller
- 1x 10kΩ resistor
- 6-12" wire for antenna
- NEO-6M GPS module (PPS output)
- USB programmer for ATtiny

---

*Document generated from chat session - December 17, 2025*
