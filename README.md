# Phoenix Nest MARS Suite — WWV Receiver

**Purpose:** Professional WWV/WWVH time signal receiver with comprehensive decoding, remote operation, and telemetry output.

**Version:** v2.0.0-beta — Major Release with Remote Operation Support

---

## Requirements

- SDRplay RSP2 Pro
- SDRplay API installed
- Windows 10/11

---

## Quick Start

### Local Operation (All-in-One)

```powershell
# Night reception (5 MHz WWV)
cmd /c ".\bin\simple_am_receiver.exe -f 5.000450 -g 59 -l 0 -o | .\bin\waterfall.exe"

# Daytime reception (10 MHz WWV)
cmd /c ".\bin\simple_am_receiver.exe -f 10.000450 -g 50 -l 2 -o | .\bin\waterfall.exe"
```

### Remote Operation (SDR Server + Client)

```powershell
# On SDR machine (runs hardware)
.\bin\sdr_server.exe -f 5.000450 -g 59

# On remote machine (visualization)
.\bin\waterfall.exe --tcp <sdr_ip>:4536
```

### Signal Relay (TCP-to-TCP Bridge)

```powershell
# Relay server at central location
.\bin\signal_relay.exe --listen 0.0.0.0:4536 --upstream <sdr_ip>:4536

# Multiple clients connect to relay
.\bin\waterfall.exe --tcp <relay_ip>:4536
```

---

## Display

The waterfall window has two panels:

| Panel | Width | Content |
|-------|-------|---------|
| Left | 1024 px | FFT waterfall (spectrum over time) |
| Right | 200 px | 7 bucket energy bars (WWV frequencies) |

### Visual Indicators

- **Carrier line:** Vertical line at center of waterfall
- **Red dots:** Energy above threshold at monitored frequencies
- **Purple flash:** 1000 Hz bar flashes purple when tick detected
- **Color scale:** Blue (low) → Cyan → Green → Yellow → Red (high)

---

## simple_am_receiver.exe

| Flag | Description | Default |
|------|-------------|---------|
| -f | Frequency in MHz | 15.0 |
| -g | Gain reduction (20-59 dB) | 40 |
| -l | LNA state (0-4) | 0 |
| -v | Volume | 50.0 |
| -o | Output PCM to stdout | off |
| -a | Mute speaker audio | off |

**Tip:** Tune slightly off-center to avoid DC hole (e.g., `-f 5.000450`)

---

## waterfall.exe

Split-screen display with automatic tick detection.

### Command-Line Options

| Flag | Description | Default |
|------|-------------|---------|
| -t, --tcp HOST[:PORT] | Connect to SDR server I/Q port | localhost:4536 |
| --stdin | Read from stdin instead of TCP | - |
| --test-pattern | Generate synthetic 1000Hz test tone | - |
| -w, --width WIDTH | Set waterfall width in pixels | 1024 |
| -H, --height HEIGHT | Set window height in pixels | 600 |
| -l, --log-csv | Enable CSV file logging | UDP only |
| -h, --help | Show help message | - |

**Examples:**
```powershell
# Standard TCP mode
.\bin\waterfall.exe --tcp localhost:4536

# Custom window size
.\bin\waterfall.exe --tcp localhost:4536 -w 800 -H 400

# Test pattern (no SDR needed)
.\bin\waterfall.exe --test-pattern

# Enable CSV logging
.\bin\waterfall.exe --tcp localhost:4536 --log-csv
```

### Keyboard Controls

| Key | Function |
|-----|----------|
| 0-7 | Select parameter (0=gain, 1-7=frequency thresholds) |
| +/- | Adjust selected parameter |
| D | Toggle tick detection on/off |
| S | Print tick statistics |
| Q/Esc | Quit |

### Monitored Frequencies

| Key | Frequency | Bandwidth | Signal |
|-----|-----------|-----------|--------|
| 1 | 100 Hz | ±10 Hz | BCD time code |
| 2 | 440 Hz | ±5 Hz | Calibration tone |
| 3 | 500 Hz | ±5 Hz | Minute marker |
| 4 | 600 Hz | ±5 Hz | Station ID |
| 5 | 1000 Hz | ±100 Hz | **WWV tick** |
| 6 | 1200 Hz | ±100 Hz | WWVH tick |
| 7 | 1500 Hz | ±20 Hz | Hour marker |

### Tick Detection Output

When ticks are detected, the console shows:
```
[  23.5s] TICK #12   int= 1002ms  avg=  999ms
[  24.5s] TICK #13   int=  998ms  avg= 1000ms
```

- **int** = Interval since last tick (should be ~1000ms)
- **avg** = Average interval over last 15 seconds
- **!** = Marker if interval outside 950-1050ms range

### CSV Logging

Tick data is logged to `wwv_ticks.csv`:
```
timestamp_ms,tick_num,energy_peak,duration_ms,interval_ms,avg_interval_ms,noise_floor
```

---

## WWV Frequencies

| Frequency | Notes |
|-----------|-------|
| 2.5 MHz | Low power, night propagation |
| 5 MHz | Good night reception |
| 10 MHz | Most reliable daytime |
| 15 MHz | Daytime, good distance |
| 20 MHz | Daytime only, WWV only |

---

## Expected Results

- Purple flash on 5th bar (1000 Hz) once per second
- Interval display ~1000ms (±50ms normal variance)
- Average converges to ~1000ms over 15 seconds
- Gap at seconds 29 and 59 (no tick transmitted)
- Carrier visible as center vertical line in waterfall

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| No signal | Try different frequency, check antenna |
| Intervals erratic | Signal too weak, try 5 MHz at night or 10 MHz daytime |
| Many rejected ticks | Voice interference, wait for quiet window |
| No purple flash | Press D to enable detection, check signal strength |
| Too much noise | Reduce LNA (-l 0), increase gain reduction (-g 59) |

---

## Signal Reference

### WWV (Fort Collins, CO)
- Second tick: 1000 Hz, 5 ms pulse
- Minute marker: 1000 Hz, 800 ms pulse
- Hour marker: 1500 Hz, 800 ms pulse
- Voice: Male, 7.5 sec before minute
- Silent: Seconds 29 and 59

### WWVH (Kauai, HI)
- Second tick: 1200 Hz, 5 ms pulse
- Minute marker: 1200 Hz, 800 ms pulse
- Hour marker: 1500 Hz, 800 ms pulse
- Voice: Female, 15 sec before minute
- Silent: Seconds 29 and 59

---

## Documentation

### System Architecture
- [SDR_SERVER.md](docs/SDR_SERVER.md) — SDR streaming server for remote operation
- [WATERFALL.md](docs/WATERFALL.md) — Waterfall display, detector modules, signal processing
- [SIGNAL_SPLITTER.md](docs/SIGNAL_SPLITTER.md) — Signal splitter for multi-client distribution
- [SIGNAL_RELAY.md](docs/SIGNAL_RELAY.md) — TCP relay server for network routing

### Telemetry & Data Output
- [UDP_TELEMETRY_OUTPUT_PROTOCOL.md](docs/UDP_TELEMETRY_OUTPUT_PROTOCOL.md) — 15-channel telemetry specification
- [TELEMETRY_LOGGER.md](docs/TELEMETRY_LOGGER.md) — System tray UDP→CSV logger tool
- [wwv_csv_documentation.md](docs/wwv_csv_documentation.md) — CSV file format reference

### WWV Signal Analysis (Technical)
- [wwv_signal_characteristics.md](docs/wwv_signal_characteristics.md) — WWV/WWVH broadcast format, timing specs
- [wwv_bcd_decoder_algorithm.md](docs/wwv_bcd_decoder_algorithm.md) — BCD time code demodulation
- [fldigi_wwv_analysis.md](docs/fldigi_wwv_analysis.md) — Comparison with fldigi implementation
- [signal_path_18DEC2025.md](docs/signal_path_18DEC2025.md) — Current signal processing architecture
- [DSP filtering for WWV tickBCD separation.md](docs/DSP%20filtering%20for%20WWV%20tickBCD%20separation.md) — Filter design analysis

### Build & Testing
- [BUILDING.md](docs/BUILDING.md) — Build system, dependencies, compiler setup
- [BETA_TESTING_GUIDE.md](docs/BETA_TESTING_GUIDE.md) — Beta testing procedures
- [BETA_TESTING_WWV.md](docs/BETA_TESTING_WWV.md) — WWV-specific test scenarios

---

## Build

```powershell
.\build.ps1              # Debug build (all tools)
.\build.ps1 -Release     # Optimized release build
.\build.ps1 -Target tools # Tools only (no tests)
.\build.ps1 -Clean       # Clean artifacts
```

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| v2.0.0-beta | 2025-12-20 | Major release: Signal splitter/relay, UDP telemetry suite, comprehensive docs |
| v1.11.2 | 2025-12-20 | Runtime parameter tuning, INI persistence, control path relay |
| v1.0.0 | 2025-12-19 | UDP telemetry consolidation, telem_logger tool, multi-detector architecture |

---

**Phoenix Nest MARS Suite**
KY4OLB
