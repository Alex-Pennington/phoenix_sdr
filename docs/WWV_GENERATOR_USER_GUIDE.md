# WWV Test Signal Generator - User Guide

**Version:** 1.9.0+
**Last Updated:** December 18, 2025
**Tool:** `wwv_gen.exe`

---

## Overview

The WWV Test Signal Generator creates NIST-compliant WWV/WWVH time station signals for testing and development. It generates precise reference signals matching the actual broadcast format, including tick pulses, minute markers, BCD time code, and station identification tones.

**Key Features:**
- ✅ NIST WWV/WWVH broadcast format compliance
- ✅ Dual output modes: File (.iqr) or TCP streaming
- ✅ Compatible with Phoenix SDR waterfall and detector tools
- ✅ Drop-in replacement for live SDR hardware during testing
- ✅ Precise timing: 2 MHz sample rate with 500ns resolution
- ✅ Configurable start time, date, and station type

---

## Quick Start

### File Mode (Save to Disk)
```powershell
# Generate 2-minute test signal
.\bin\wwv_gen.exe -o test.iqr

# View in waterfall
.\bin\iqr_play.exe test.iqr | .\bin\waterfall.exe
```

### TCP Streaming Mode (Real-time)
```powershell
# Terminal 1: Start signal generator
.\bin\wwv_gen.exe -p 4536

# Terminal 2: View in waterfall
.\bin\waterfall.exe -t localhost:4536
```

---

## Command Line Options

### Basic Options

| Option | Argument | Default | Description |
|--------|----------|---------|-------------|
| `-h` | — | — | Show help and exit |
| `-t` | `HH:MM` | `00:00` | Start time (24-hour format) |
| `-d` | `DDD` | `001` | Day of year (1-366) |
| `-y` | `YY` | `25` | Year (last 2 digits, 00-99) |
| `-s` | `wwv\|wwvh` | `wwv` | Station type (WWV or WWVH) |

### Output Mode Options

| Option | Argument | Default | Description |
|--------|----------|---------|-------------|
| `-o` | `FILE` | `wwv_test.iqr` | Output filename (file mode) |
| `-p` | `PORT` | `4536` | TCP port for streaming (enables TCP mode) |
| `-c` | — | off | Continuous streaming (requires `-p`) |

**Mode Selection:**
- **File mode:** Use `-o` (default) - generates 2-minute .iqr file
- **TCP mode:** Use `-p` - streams over network to clients

---

## Usage Examples

### File Output Mode

#### Basic File Generation
```powershell
# Default: 2-minute signal starting at 00:00, day 1, 2025
.\bin\wwv_gen.exe -o my_signal.iqr
```

**Output:**
- File: `my_signal.iqr` (~458 MB)
- Duration: Exactly 2 minutes (120 million samples)
- Format: IQR with 64-byte header + int16 I/Q pairs

#### Specific Time/Date
```powershell
# Noon on Christmas Day 2025 (day 359)
.\bin\wwv_gen.exe -t 12:00 -d 359 -y 25 -o christmas.iqr

# Midnight New Year's 2026 (day 1)
.\bin\wwv_gen.exe -t 00:00 -d 001 -y 26 -o newyear.iqr

# 6:30 PM on day 100, 2025
.\bin\wwv_gen.exe -t 18:30 -d 100 -y 25 -o evening.iqr
```

#### Station Selection
```powershell
# WWV (Fort Collins, CO) - 1000 Hz tone
.\bin\wwv_gen.exe -s wwv -o wwv_signal.iqr

# WWVH (Kauai, HI) - 1200 Hz tone
.\bin\wwv_gen.exe -s wwvh -o wwvh_signal.iqr
```

### TCP Streaming Mode

#### Basic Streaming
```powershell
# Start server on default port 4536, 2-minute loop
.\bin\wwv_gen.exe -p 4536
```

**What happens:**
1. Opens TCP server on port 4536
2. Waits for client connection
3. Sends protocol header with signal parameters
4. Streams 2-minute signal in 8192-sample frames
5. Loops back to start and repeats
6. Press Ctrl+C to stop

#### Continuous Streaming
```powershell
# Stream forever (24/7 operation)
.\bin\wwv_gen.exe -p 4536 -c
```

Streams continuously without the 2-minute loop reset. Useful for long-duration testing.

#### Custom Port
```powershell
# Use alternative port (avoid conflicts)
.\bin\wwv_gen.exe -p 5000

# Different port for multiple instances
.\bin\wwv_gen.exe -p 4537 -s wwv -t 12:00
.\bin\wwv_gen.exe -p 4538 -s wwvh -t 12:00
```

#### Combined Options
```powershell
# WWVH signal, 3:45 PM, continuous stream
.\bin\wwv_gen.exe -p 4536 -c -s wwvh -t 15:45 -d 250 -y 25
```

### Integration Examples

#### Test with Waterfall
```powershell
# Terminal 1: Generate signal
.\bin\wwv_gen.exe -p 4536 -t 12:00

# Terminal 2: Display in waterfall
.\bin\waterfall.exe -t localhost:4536
```

**Expected display:**
- 1000 Hz carrier (WWV) or 1200 Hz (WWVH)
- Tick marks every second (5ms pulses)
- Minute marker at second 0 (800ms pulse)
- 500 Hz or 600 Hz tones per schedule
- BCD amplitude modulation (18% / 3%)

#### Test Tick Detector
```powershell
# Stream to detector (CSV output)
.\bin\wwv_gen.exe -p 4536 | .\bin\tick_detector.exe

# Or use file
.\bin\wwv_gen.exe -o test.iqr
.\bin\iqr_play.exe test.iqr | .\bin\tick_detector.exe
```

#### Automated Testing
```powershell
# Generate test suite
.\bin\wwv_gen.exe -t 00:00 -o test_midnight.iqr
.\bin\wwv_gen.exe -t 12:00 -o test_noon.iqr
.\bin\wwv_gen.exe -t 23:59 -o test_rollover.iqr

# Run detector tests
foreach ($file in Get-ChildItem *.iqr) {
    Write-Host "Testing $file..."
    .\bin\iqr_play.exe $file | .\bin\waterfall.exe
}
```

---

## Output Format Details

### File Mode (.iqr Format)

**Header (64 bytes):**
```
Offset  Size  Field
------  ----  -----
0       16    Magic "IQRFMT" + version
16      4     Sample rate (2,000,000 Hz)
20      8     Center frequency (5,000,000 Hz)
28      4     Bandwidth (2,000 kHz)
32      4     Gain reduction (59 dB)
36      4     LNA state (0)
40      24    Reserved
```

**Data:**
- Format: Interleaved int16 I/Q pairs
- Samples: 240,000,000 values (120M I/Q pairs)
- Size: ~458 MB per 2-minute file
- Endianness: Little-endian (Intel/AMD)

**Sample Layout:**
```
[I0][Q0][I1][Q1][I2][Q2]...
 └─2─┘ └─2─┘ └─2─┘      (bytes)
```

### TCP Streaming Mode

**Protocol:** Phoenix SDR I/Q Streaming Interface v1

**Connection Flow:**
```
Client connects → Server sends header → Server streams frames → Loop/Stop
```

**Header (32 bytes, sent once):**
```c
struct iq_stream_header {
    uint32_t magic;           // 0x50485849 ("PHXI")
    uint32_t version;         // 1
    uint32_t sample_rate;     // 2000000
    uint32_t sample_format;   // 1 (int16)
    uint32_t center_freq_lo;  // 5000000 (low 32 bits)
    uint32_t center_freq_hi;  // 0 (high 32 bits)
    uint32_t gain_reduction;  // 59
    uint32_t lna_state;       // 0
};
```

**Frame Header (16 bytes, per frame):**
```c
struct iq_data_frame {
    uint32_t magic;           // 0x49514451 ("IQDQ")
    uint32_t sequence;        // Frame counter (0, 1, 2, ...)
    uint32_t num_samples;     // 8192 (I/Q pairs per frame)
    uint32_t flags;           // 0 (no overload/changes)
};
```

**Frame Data:**
- 8192 I/Q pairs = 32,768 bytes
- Interleaved int16: `[I0][Q0][I1][Q1]...`
- ~244 frames per second at 2 Msps

**Bandwidth:**
- Sample rate: 2 Msps × 4 bytes = 8 MB/s
- Frame rate: ~244 Hz
- Overhead: <1% (headers)

---

## Signal Content

### WWV Broadcast Format (NIST Specification)

**Every Second (except 29 and 59):**
- **Tick pulse:** 5ms burst of 1000 Hz tone
- **Timing:** Aligned to UTC second boundary
- **Purpose:** Second synchronization

**Second 0 (Minute Marker):**
- **Duration:** 800ms instead of 5ms
- **Tone:** 1000 Hz (WWV) or 1200 Hz (WWVH)
- **Purpose:** Minute synchronization

**BCD Time Code:**
- **Carrier:** 100 Hz subcarrier
- **Modulation:** Amplitude modulation of main carrier
- **Levels:**
  - Binary 0: 200ms pulse → 18% depth
  - Binary 1: 500ms pulse → 18% depth
  - Marker: 800ms pulse → 18% depth
  - Reference: 3% depth between pulses
- **Encoding:** LSB-first, 1-2-4-8 weighting per digit
- **Content:** Minutes, hours, day of year, year

**Station Tones:**
- **500 Hz tone:** Minutes 0, 1 (WWV propagation advisory)
- **600 Hz tone:** Minutes 1, 2 (WWV DUT1 information)
- **1000 Hz:** WWV station identification (continuous except ticks)
- **1200 Hz:** WWVH station identification (continuous except ticks)

**Hour Markers:**
- **1500 Hz tone:** During seconds 00-04 of minutes 0, 30
- **Purpose:** Hour/half-hour identification

**Silent Periods (Guard Zones):**
- **Samples 0-20,000:** 10ms silence at start of second
- **Samples 30,000-80,000:** 25ms silence mid-second
- **Purpose:** Detector synchronization

### Signal Characteristics

**Baseband (DC-Centered):**
- All tones centered at 0 Hz in I/Q baseband
- WWV 1000 Hz tone → 450 Hz offset (DC hole avoidance)
- WWVH 1200 Hz tone → 450 Hz offset
- USB demodulation recommended

**Amplitude:**
- Full scale: ±32767 (int16 range)
- Typical: ~±20,000 (60% of full scale)
- Prevents clipping during modulation peaks

**Phase Coherence:**
- All oscillators maintain continuous phase
- No phase discontinuities across samples
- Suitable for phase-locked loop testing

---

## Testing and Validation

### Visual Verification

**Waterfall Display:**
```powershell
.\bin\wwv_gen.exe -p 4536
.\bin\waterfall.exe -t localhost:4536
```

**Look for:**
- ✅ Horizontal carrier line at 1000 Hz (WWV) or 1200 Hz (WWVH)
- ✅ Vertical tick marks every 1 second (60 per minute)
- ✅ Tall minute marker at second 0
- ✅ 500 Hz tone band (minutes 0-1)
- ✅ 600 Hz tone band (minutes 1-2)
- ✅ BCD modulation visible as intensity changes

### Detector Testing

**Tick Detector:**
```powershell
# Should detect 58 ticks per minute (seconds 29, 59 silent)
.\bin\wwv_gen.exe -o test.iqr
.\bin\iqr_play.exe test.iqr | .\bin\tick_detector.exe > ticks.csv
# Verify CSV shows ticks at 1-second intervals
```

**Marker Detector:**
```powershell
# Should detect minute marker at second 0
.\bin\wwv_gen.exe -p 4536
.\bin\waterfall.exe -t localhost:4536
# Watch for 800ms pulse detection at start of each minute
```

**BCD Decoder:**
```powershell
# Should decode time code correctly
.\bin\wwv_gen.exe -t 12:34 -d 100 -y 25 -o test.iqr
.\bin\iqr_play.exe test.iqr | .\bin\bcd_decoder.exe
# Verify output: 12:34, day 100, year 2025
```

### Performance Testing

**Frame Rate Check:**
```powershell
# TCP mode shows frames/sec in output
.\bin\wwv_gen.exe -p 4536
# Should see ~244 frames/second
```

**File Generation Speed:**
```powershell
# Measure generation time
Measure-Command { .\bin\wwv_gen.exe -o test.iqr }
# Typical: 5-10 seconds for 2-minute file
```

**Memory Usage:**
```powershell
# Monitor during streaming
.\bin\wwv_gen.exe -p 4536 -c
# Should be <10 MB RAM (small buffers)
```

---

## Troubleshooting

### Common Issues

#### "Error: bind() failed on port 4536"

**Cause:** Port already in use by another application

**Solutions:**
```powershell
# Check what's using the port
netstat -ano | findstr :4536

# Use different port
.\bin\wwv_gen.exe -p 4537

# Stop conflicting application
# (Find PID from netstat, then Task Manager → End Task)
```

#### "Client disconnected" during streaming

**Cause:** Client application closed connection

**Normal behavior:**
- Waterfall closed
- Network interruption
- Client error

**Action:**
- Server continues waiting
- Restart client to reconnect
- Server loops automatically

#### File too large / disk space

**Issue:** 458 MB per 2-minute file

**Solutions:**
```powershell
# Use TCP streaming instead (no files)
.\bin\wwv_gen.exe -p 4536

# Generate shorter test files (not supported directly)
# Use TCP streaming with recording:
.\bin\wwv_gen.exe -p 4536 | .\bin\iq_record.exe -d 30
# (Records 30 seconds instead of 2 minutes)
```

#### Wrong time displayed in decoder

**Cause:** Incorrect command-line time

**Check:**
```powershell
# Verify time format HH:MM (24-hour)
.\bin\wwv_gen.exe -t 14:30    # ✅ Correct
.\bin\wwv_gen.exe -t 2:30pm   # ❌ Wrong format

# Verify day of year (1-366)
.\bin\wwv_gen.exe -d 100      # ✅ April 10
.\bin\wwv_gen.exe -d 400      # ❌ Out of range
```

#### No ticks detected

**Check signal path:**
```powershell
# Verify generator output
.\bin\wwv_gen.exe -p 4536
# Should show "Client connected! Streaming samples..."

# Verify waterfall receiving
.\bin\waterfall.exe -t localhost:4536
# Should show waterfall display with ticks

# Check audio
# Ticks should be audible as clicks every second
```

### Debug Mode

**Verbose output:**
```powershell
# File mode shows progress
.\bin\wwv_gen.exe -o test.iqr
# Progress: 10% (12 seconds)...
# Progress: 20% (24 seconds)...

# TCP mode shows frame count
.\bin\wwv_gen.exe -p 4536
# Streaming: 00:05 (1220 frames)...
# Streaming: 00:10 (2440 frames)...
```

**Verify TCP protocol:**
```powershell
# Use test client to check header
.\test_tcp_stream.ps1
# Should show:
#   Magic: 0x50485849 (PHXI)
#   Version: 1
#   Sample Rate: 2000000 Hz
```

---

## Technical Specifications

### Signal Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Sample rate | 2,000,000 Hz | Fixed (2 Msps) |
| Center frequency | 5,000,000 Hz | Metadata only |
| Baseband offset | 450 Hz | DC hole avoidance |
| Data format | int16 I/Q | Native SDRplay format |
| Precision | 16-bit signed | ±32767 range |
| Duration (file) | 120 seconds | Exactly 240M samples |
| Duration (TCP) | Configurable | 2-min loop or continuous |

### WWV Signal Timing

| Event | Duration | Frequency | Notes |
|-------|----------|-----------|-------|
| Tick pulse | 5 ms | 1000 Hz | Every second except 29, 59 |
| Minute marker | 800 ms | 1000/1200 Hz | Second 0 only |
| BCD "0" pulse | 200 ms | 100 Hz SC | 18% AM depth |
| BCD "1" pulse | 500 ms | 100 Hz SC | 18% AM depth |
| BCD marker | 800 ms | 100 Hz SC | 18% AM depth |
| BCD reference | — | 100 Hz SC | 3% AM depth |

### Protocol Specifications

**TCP Streaming:**
- Port: 4536 (default, configurable)
- Protocol: Phoenix SDR I/Q Streaming v1
- Frame size: 8192 I/Q pairs (32 KB)
- Frame rate: ~244 Hz
- Bandwidth: ~8 MB/s
- Compatibility: `sdr_server.exe`, `waterfall.exe`

**File Format:**
- Extension: `.iqr`
- Header: 64 bytes (IQR format v1)
- Data: Little-endian int16
- Interleave: I/Q/I/Q/...
- Compatibility: `iqr_play.exe`, FFmpeg (raw)

---

## Limitations and Constraints

### Known Limitations

1. **Fixed Duration (File Mode)**
   - Always generates exactly 2 minutes
   - Cannot specify custom duration
   - Workaround: Use TCP mode with timed recording

2. **No Real-time Clock Sync**
   - Start time set manually via `-t`
   - Does not sync with system clock
   - Time does not advance beyond 2-minute window

3. **Single Client (TCP Mode)**
   - One TCP client at a time
   - Subsequent clients must wait for disconnect
   - Workaround: Run multiple instances on different ports

4. **Windows Only**
   - Uses WinSock2 (ws2_32.dll)
   - Not portable to Linux/Mac without modifications
   - MSYS2 MinGW64 build required

5. **Fixed Sample Rate**
   - Always 2 Msps (hardcoded)
   - Cannot change sample rate
   - Matches SDRplay default rate

### Performance Constraints

| Constraint | Limit | Impact |
|------------|-------|--------|
| Memory usage | ~10 MB | Minimal (small buffers) |
| CPU usage | <5% | Single-threaded, efficient |
| Disk I/O | ~45 MB/s | File write speed |
| Network | 8 MB/s | TCP streaming bandwidth |
| File size | 458 MB | Per 2-minute file |

### System Requirements

**Minimum:**
- CPU: 1 GHz single-core
- RAM: 100 MB
- Disk: 500 MB free (for files)
- Network: 10 Mbps (for TCP)

**Recommended:**
- CPU: 2 GHz dual-core
- RAM: 500 MB
- Disk: 5 GB free
- Network: 100 Mbps

---

## Advanced Usage

### Multiple Simultaneous Streams

```powershell
# Terminal 1: WWV on port 4536
.\bin\wwv_gen.exe -p 4536 -s wwv -t 12:00

# Terminal 2: WWVH on port 4537
.\bin\wwv_gen.exe -p 4537 -s wwvh -t 12:00

# Terminal 3: Compare stations
.\bin\waterfall.exe -t localhost:4536   # WWV
.\bin\waterfall.exe -t localhost:4537   # WWVH
```

### Batch Processing

```powershell
# Generate test signals for entire day (hourly)
for ($hour = 0; $hour -lt 24; $hour++) {
    $time = "{0:D2}:00" -f $hour
    $file = "wwv_hour_{0:D2}.iqr" -f $hour
    Write-Host "Generating $time..."
    .\bin\wwv_gen.exe -t $time -o $file
}
```

### Scripted Testing

```powershell
# Automated regression test
$tests = @(
    @{time="00:00"; day=1},
    @{time="12:00"; day=1},
    @{time="23:59"; day=365}
)

foreach ($test in $tests) {
    Write-Host "Testing $($test.time) day $($test.day)..."
    .\bin\wwv_gen.exe -t $test.time -d $test.day -o test.iqr
    $result = .\bin\iqr_play.exe test.iqr | .\bin\bcd_decoder.exe
    # Verify result matches expected time
}
```

### Integration with CI/CD

```yaml
# Example GitHub Actions workflow
- name: Generate Test Signal
  run: .\bin\wwv_gen.exe -o test.iqr

- name: Run Detector Tests
  run: |
    .\bin\iqr_play.exe test.iqr | .\bin\tick_detector.exe > ticks.csv
    .\bin\iqr_play.exe test.iqr | .\bin\marker_detector.exe > markers.csv

- name: Validate Output
  run: python validate_detections.py ticks.csv markers.csv
```

---

## Appendix

### BCD Time Code Format

**Frame Structure (60 seconds):**
```
Second  Content
------  -------
0       Minute marker (800ms, reference)
1-8     Minutes (BCD, LSB first)
9       Position marker
10-17   Hours (BCD, LSB first)
18      Reserved (0)
19      Position marker
20-28   Day of year (BCD, LSB first)
29      Silent (no tick)
30-38   Day of year continued
39      Position marker
40-48   Year (BCD, LSB first)
49      Position marker
50-58   Control bits, DUT1, LS warning
59      Silent (no tick)
```

**BCD Encoding:**
- Each decimal digit: 4 bits (0-9)
- Weight: 1-2-4-8 (LSB first)
- Example: 5 → bits 0100 → seconds 1(0), 2(0), 3(1), 4(0)

### Station Differences

| Feature | WWV (Colorado) | WWVH (Hawaii) |
|---------|----------------|---------------|
| Location | Fort Collins, CO | Kauai, HI |
| Tone | 1000 Hz | 1200 Hz |
| Voice | Male | Female |
| Time offset | UTC | UTC |
| Generator flag | `-s wwv` | `-s wwvh` |

### File Format Reference

**IQR Header Fields:**
```c
typedef struct {
    char magic[8];        // "IQRFMT\0\0"
    uint32_t version;     // 1
    uint32_t header_size; // 64
    uint32_t sample_rate; // 2000000
    double center_freq;   // 5000000.0
    uint32_t bandwidth;   // 2000
    uint32_t gain_reduction; // 59
    uint32_t lna_state;   // 0
    uint8_t reserved[24]; // Future use
} iqr_header_t;
```

### Related Documentation

- [Phoenix SDR I/Q Streaming Interface](SDR_IQ_STREAMING_INTERFACE.md)
- [WWV BCD Decoder Algorithm](wwv_bcd_decoder_algorithm.md)
- [Building WWV Generator](../BUILD_WWV_GEN.md)
- [WWV Test Signal Generator Specification](WWV_Test_Signal_Generator_Specification.md)

---

## Support

**Issues/Questions:**
- GitHub: https://github.com/Alex-Pennington/phoenix_sdr/issues
- Documentation: See `docs/` folder
- Examples: See `BUILD_WWV_GEN.md`

**Version Information:**
```powershell
.\bin\wwv_gen.exe -h
# Shows version number and build info
```

**Build from Source:**
```powershell
# Minimal build (WWV generator only)
powershell -ExecutionPolicy Bypass -File build_wwv_gen.ps1

# Full project build
.\build.ps1
```

---

**Document Version:** 1.0
**Generator Version:** 1.9.0+
**Last Updated:** December 18, 2025
