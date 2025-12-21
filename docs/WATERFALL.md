# Phoenix SDR Waterfall Display

Real-time spectrum waterfall and WWV/WWVH time signal decoder with UDP telemetry output.

## Quick Start

```powershell
# Start waterfall (connects to sdr_server on localhost:4536)
.\bin\waterfall.exe

# With specific SDR server
.\bin\waterfall.exe -t 192.168.1.100:4536

# Pipe from simple_am_receiver
cmd /c ".\bin\simple_am_receiver.exe -f 5.000450 -g 59 -l 0 -o | .\bin\waterfall.exe --stdin"

# Test mode (no SDR hardware)
.\bin\waterfall.exe --test-pattern
```

## Command-Line Options

```
Usage: waterfall.exe [options]

I/Q Input:
  -t, --tcp HOST[:PORT]   Connect to SDR server I/Q port (default localhost:4536)
  --stdin                 Read from stdin instead of TCP
  --test-pattern          Generate synthetic 1000Hz test tone (no SDR needed)

Window Configuration:
  -w, --width WIDTH       Set waterfall width (default: 1024)
  -H, --height HEIGHT     Set window height (default: 768)
  -x, --pos-x X           Set window X position (default: centered)
  -y, --pos-y Y           Set window Y position (default: centered)

Output:
  -l, --log-csv           Enable CSV file logging (default: UDP telemetry only)

Debug:
  --reload-debug          Reload tuned parameters from waterfall.ini

Help:
  -h, --help              Show this help

UDP Telemetry:          Broadcast on port 3005 (always enabled)
Control Interface:      Type commands in console (freq, gain, status, etc.)
See: docs/UDP_TELEMETRY_OUTPUT_PROTOCOL.md
```

## Features

### Real-Time Display
- **Waterfall Spectrum:** Scrolling FFT display showing frequency content over time
- **Dual Signal Paths:**
  - Detector path: 50 kHz for pulse detection (ticks, markers, BCD)
  - Display path: 12 kHz for visualization and audio
- **Frequency Bands:** Visual markers for tone frequencies (100, 440, 500, 600, 1000, 1200 Hz)
- **Live Audio:** Optional audio output of demodulated signal

### WWV/WWVH Decoding
- **Tick Detector:** 5ms pulses at 1000 Hz (WWV) or 1200 Hz (WWVH)
- **Marker Detector:** 800ms minute markers (second 0 of each minute)
- **Sync Detector:** Phase-locked loop for sub-millisecond timing
- **BCD Time Decoder:** Binary-coded time transmitted on 100 Hz subcarrier
- **Tone Trackers:** Monitors 500 Hz (WWV) and 600 Hz (WWVH) tones

### Telemetry Output
- **UDP Broadcast:** Port 3005 (always enabled, non-blocking)
- **CSV Logging:** Optional file logging (`-l` flag)
- **15 Channels:** CHAN, TICK, MARK, CARR, SYNC, SUBC, CORR, T500, T600, BCDE, BCDS, CONS, CTRL, RESP
- **Real-time:** ~1 message/second per channel

See [UDP_TELEMETRY_OUTPUT_PROTOCOL.md](UDP_TELEMETRY_OUTPUT_PROTOCOL.md) for full channel documentation.

## Window Layout

```
┌────────────────────────────────────────────────────────────────┐
│  Phoenix SDR Waterfall - WWV 5.000450 MHz                      │
├────────────────────────────────────────────────────────────────┤
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                    Waterfall Spectrum                    │  │
│  │  Time ↓           (FFT, 12 kHz display path)             │  │
│  │  ─────────────────────────────────────────────────────   │  │
│  │  │ │ │   │   │ │ │   100  500  1000  1500 Hz           │  │
│  │  ─────────────────────────────────────────────────────   │  │
│  │                                                          │  │
│  │  Frequency bands marked with colored bars               │  │
│  │  Flash indicators show detector activity                │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Status Bar                                              │  │
│  │  SYNC: LOCKED | Tick: T47 | Marker: M2 | BCD: 15:34:22  │  │
│  │  SNR: 35 dB | Carrier: -6 dB | Sub500: -12 dB           │  │
│  └──────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────┘
```

### Visual Feedback
- **Flash Bars:** Detector events trigger colored band flashes
  - Blue: Tick detected
  - Green: Marker detected
  - Yellow: BCD symbol decoded
  - Red: Sync lost/reacquired
- **Status Updates:** Real-time display of decoder state

## Interactive Console Commands

Type commands in the console window (if connected via TCP to sdr_server):

### Frequency Control
```
freq 5.0005         # Set frequency to 5.0005 MHz
freq 10.0           # Tune to WWV 10 MHz
freq 15.0           # Tune to WWV 15 MHz
```

### Gain Control
```
gain 59             # Set gain reduction to 59 dB (less RF gain)
gain 20             # Set gain reduction to 20 dB (more RF gain)
lna 1               # Set LNA state (0-8)
```

### Status
```
status              # Query SDR settings
```

### Antenna Selection
```
antenna A           # Select antenna port A
antenna B           # Select antenna port B
antenna Z           # Select Hi-Z port
```

**Note:** Console commands only work when connected to sdr_server via TCP (`-t` option).
When using `--stdin` or `--test-pattern`, SDR control is not available.

## I/Q Input Modes

### TCP Mode (Default)
Connects to sdr_server for live I/Q stream:
```powershell
# Start sdr_server first
.\bin\sdr_server.exe -f 5.000450 -g 59

# Then start waterfall
.\bin\waterfall.exe -t localhost:4536
```

### Stdin Mode
Reads I/Q from stdin (piped from another program):
```powershell
# Pipe from simple_am_receiver
cmd /c ".\bin\simple_am_receiver.exe -f 5.000450 -g 59 -l 0 -o | .\bin\waterfall.exe --stdin"

# Pipe from IQR file replay
cmd /c ".\bin\iqr_play.exe my_recording.iqr | .\bin\waterfall.exe --stdin"
```

Expected stdin format:
- **Magic:** `IQDQ` (0x49514451)
- **Frame header:** 32 bytes (see SDR_IQ_STREAMING_INTERFACE.md)
- **Samples:** int16 I/Q pairs (IQIQIQ...)

### Test Pattern Mode
Generates synthetic 1000 Hz tone for testing without hardware:
```powershell
.\bin\waterfall.exe --test-pattern
```

Useful for:
- Verifying waterfall display works
- Testing detector calibration
- Demonstrating features without SDR

## UDP Telemetry

### Listening to Telemetry

#### PowerShell
```powershell
$udp = New-Object System.Net.Sockets.UdpClient(3005)
$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
while($true) {
    $bytes = $udp.Receive([ref]$ep)
    [System.Text.Encoding]::ASCII.GetString($bytes)
}
```

#### Telemetry Logger
```powershell
# Start logger in background (system tray)
.\bin\telem_logger.exe -o logs/

# Or with console output
.\bin\telem_logger.exe -o logs/ -v
```

See [TELEMETRY_LOGGER.md](TELEMETRY_LOGGER.md) for logger documentation.

### Telemetry Channels

| Channel | Rate | Description |
|---------|------|-------------|
| CHAN | ~1/sec | Overall channel quality (SNR, carrier, noise) |
| TICK | Event | Tick pulse detections |
| MARK | Event | Minute marker detections |
| SYNC | Event | Sync state changes (SEARCHING, LOCKED, etc.) |
| CARR | ~1/sec | DC carrier frequency tracking |
| SUBC | ~1/sec | 500/600 Hz subcarrier analysis |
| T500 | ~1/sec | 500 Hz tone tracker (WWV) |
| T600 | ~1/sec | 600 Hz tone tracker (WWVH) |
| CORR | Event | Marker correlation results |
| BCDE | ~1/sec | 100 Hz BCD envelope (deprecated) |
| BCDS | Event | BCD symbols and decoded time |
| CONS | Event | Console/status messages |
| CTRL | Event | Control commands received |
| RESP | Event | Command responses |

Full protocol: [UDP_TELEMETRY_OUTPUT_PROTOCOL.md](UDP_TELEMETRY_OUTPUT_PROTOCOL.md)

## CSV File Logging

Enable with `-l` flag:
```powershell
.\bin\waterfall.exe -l
```

Creates timestamped CSV files in logs/ directory:
```
logs/telem_CHAN_20251220_143000.csv
logs/telem_TICK_20251220_143000.csv
logs/telem_MARK_20251220_143000.csv
...
```

**Note:** UDP telemetry is always enabled. CSV logging is optional and writes the same data to files.

## Signal Processing

### Dual-Path Architecture

```
                    2 MHz I/Q (from SDR or stdin)
                              │
                    ┌─────────┴─────────┐
                    │  Normalize [-1,1] │
                    └─────────┬─────────┘
                              │
        ┌─────────────────────┴─────────────────────┐
        │                                           │
        ▼                                           ▼
  DETECTOR PATH (50 kHz)                    DISPLAY PATH (12 kHz)
  ─────────────────────                     ──────────────────────
  5 kHz lowpass                             5 kHz lowpass
  Decimate 40:1                             Decimate 166:1
        │                                           │
        ├──► Tick Detector                         ├──► FFT Waterfall
        ├──► Marker Detector                       ├──► Audio Output
        ├──► Sync Detector                         └──► Tone Trackers
        ├──► BCD Decoders
        └──► Correlators
```

### Key Points
- **Independent paths:** Detectors use 50 kHz, display uses 12 kHz
- **No crosstalk:** Each path has own filters and decimators
- **Exact divergence:** Signal split matches waterfall.c lines 2151-2238
- **Shared input:** Both paths start from same normalized I/Q samples

See [SDR_WATERFALL_AND_AM_DEMODULATION.md](SDR_WATERFALL_AND_AM_DEMODULATION.md) for DSP theory.

## Configuration File (waterfall.ini)

Located in bin/waterfall.ini, contains tuned detector parameters.

### Reloading Parameters
```powershell
# Edit bin/waterfall.ini
notepad bin\waterfall.ini

# Reload without restarting waterfall
.\bin\waterfall.exe --reload-debug
```

### Common Parameters
```ini
[tick_detector]
energy_threshold = 0.015
duration_min_ms = 3.0
duration_max_ms = 7.0

[marker_detector]
energy_threshold = 0.012
duration_min_ms = 700.0
duration_max_ms = 900.0

[sync_detector]
lock_threshold = 3
unlock_threshold = 5
```

**Caution:** Incorrect values can break detection. Use --reload-debug to test changes.

## Performance

### CPU Usage
- **Idle:** ~15% (1 core, waterfall updates)
- **Active decoding:** ~25% (all detectors running)
- **Test pattern:** ~10% (synthetic signal, no detectors)

### Memory Usage
- **Base:** ~100 MB (SDL, FFT buffers, ring buffers)
- **Per detector:** +5-10 MB
- **Total:** ~150-200 MB typical

### Frame Rate
- **Waterfall:** 30 FPS (display path decimation)
- **Detector processing:** Real-time (50 kHz path, no drops)

## Troubleshooting

### No waterfall display
- Verify SDL2 DLL in bin/ folder
- Check window position not off-screen (`-x 100 -y 100`)
- Try `--test-pattern` to isolate SDR issues

### No detections (SYNC: SEARCHING)
- Signal too weak (increase RF gain: `gain 20`)
- Wrong frequency (WWV: 2.5, 5, 10, 15, 20 MHz)
- Antenna issue (try `antenna Z` for Hi-Z)
- DC offset (tune 450 Hz off-center: `freq 5.000450`)

### UDP telemetry not received
- Check firewall blocking port 3005
- Verify listener IP (broadcast 255.255.255.255)
- Try telem_logger.exe to confirm output
- Console shows "Telemetry: enabled" on startup

### Audio issues
- Waterfall audio module separate from AM receiver
- Check Windows audio mixer (waterfall volume)
- Audio decimation independent of display path

### High CPU usage
- Reduce waterfall width (`-w 512`)
- Reduce window height (`-H 600`)
- Close unnecessary detector channels in code

## Development Notes

### Frozen Files (Per COPILOT-INSTRUCTIONS.md)
- `waterfall.c` signal chain (lines 2151-2238): Detector/display divergence point
- `waterfall_dsp.c`: Lowpass filters and decimation
- `marker_detector.c/.h`: Minute marker detection
- `marker_correlator.c/.h`: Marker correlation
- `sync_detector.c/.h`: Sync state machine

**Do not modify** without explicit review.

### Adding New Detectors
1. Create `tools/my_detector.c` and `.h` following detector pattern
2. Integrate callback in `waterfall.c` (detector path, 50 kHz samples)
3. Register flash source in `waterfall_flash.c`
4. Add telemetry channel in `waterfall_telemetry.h`
5. Update waterfall.ini with tuning parameters

See [tick_detector.c](../tools/tick_detector.c) as reference implementation.

## Related Documentation

- **UDP Protocol:** [UDP_TELEMETRY_OUTPUT_PROTOCOL.md](UDP_TELEMETRY_OUTPUT_PROTOCOL.md) - Full channel specs
- **Telemetry Logger:** [TELEMETRY_LOGGER.md](TELEMETRY_LOGGER.md) - CSV logging tool
- **I/Q Streaming:** [SDR_IQ_STREAMING_INTERFACE.md](SDR_IQ_STREAMING_INTERFACE.md) - Input protocol
- **DSP Theory:** [SDR_WATERFALL_AND_AM_DEMODULATION.md](SDR_WATERFALL_AND_AM_DEMODULATION.md) - Signal processing
- **SDR Server:** [SDR_SERVER.md](SDR_SERVER.md) - Control and I/Q source
- **Signal Splitter:** [SIGNAL_SPLITTER.md](SIGNAL_SPLITTER.md) - Remote relay

## Example Workflows

### Local WWV Reception
```powershell
# Terminal 1: Start SDR server
.\bin\sdr_server.exe -f 5.000450 -g 59

# Terminal 2: Start waterfall
.\bin\waterfall.exe

# Terminal 3: Start telemetry logger
.\bin\telem_logger.exe -o logs/
```

### Remote Operation
```powershell
# Mountain-top: Start SDR and splitter
.\bin\sdr_server.exe -f 5.000450 -g 59
.\bin\signal_splitter.exe --relay-host relay.example.com

# Remote: Connect waterfall to relay
.\bin\waterfall.exe -t relay.example.com:4411
```

### File Analysis
```powershell
# Record I/Q
.\bin\phoenix_sdr.exe -f 10.0 -d 60 -o wwv10

# Replay with waterfall
cmd /c ".\bin\iqr_play.exe recordings\2025_12_20\wwv10_raw.iqr | .\bin\waterfall.exe --stdin"
```

## Version History

- **v0.8.11-beta:** Current release with full detector suite
- **v0.8.x:** Added BCD time decoding, sync detector
- **v0.7.x:** Added UDP telemetry, detector framework
- **v0.6.x:** Initial waterfall with basic tick detection
