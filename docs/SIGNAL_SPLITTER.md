# Signal Splitter

Splits 2 MHz I/Q from SDR server into detector (50 kHz) and display (12 kHz) streams for remote relay.

## Architecture

```
┌─────────────────┐  ┌─────────────────┐
│  sdr_server     │  │  sdr_server     │  Local mountain-top system
│  localhost:4536 │  │  localhost:4535 │  I/Q stream + Control port
│  (I/Q stream)   │  │  (Control)      │
└────────┬────────┘  └────────┬────────┘
         │                    │
         ▼                    ▼
┌─────────────────────────────────────────┐
│            signal_splitter              │  Local processor
│  - Receives 2 MHz I/Q                   │  - Forwards control commands
│  - Signal divergence (waterfall.c copy) │  - Bidirectional text relay
│  - 30-second ring buffers               │
└────────┬────────────────────────────────┘
         │
         ├─────────────────┬─────────────────┬─────────────────┐
         ▼                 ▼                 ▼                 ▼
    Remote Relay      Remote Relay      Remote Relay      Remote Relay
    50 kHz I/Q        12 kHz I/Q        Control           (DO droplet)
    float32           float32           Text              Linux
    Port 4410         Port 4411         Port 4409
```

## Signal Processing

**Exact copy of waterfall.c divergence point (lines 2151-2238):**

1. **Normalize:** int16 → float [-1, 1]
2. **Detector Path:** 5 kHz lowpass → decimate 40:1 → 50 kHz I/Q
3. **Display Path:** 5 kHz lowpass → decimate 166:1 → 12 kHz I/Q
4. **Control Path:** Bidirectional text relay (no modification)

## Control Path

Commands flow bidirectionally:
- **Client → SDR:** `freq 5.0005`, `gain 59`, `status`
- **SDR → Client:** Responses and status updates

Forwarding is transparent passthrough with no command parsing or validation.

## Usage

### Mountain-Top System Setup

```bash
# Terminal 1: Start SDR server
.\bin\sdr_server.exe -f 5.000450 -g 59 -l 3

# Terminal 2: Start signal splitter (connects to remote relay)
.\bin\signal_splitter.exe --relay-host relay.example.com
```

### Command-Line Options

```
--sdr-host HOST        SDR server hostname (default: localhost)
--sdr-port PORT        SDR I/Q port (default: 4536)
--sdr-ctrl-port PORT   SDR control port (default: 4535)
--relay-host HOST      Relay server hostname (required)
--relay-det PORT       Relay detector port (default: 4410)
--relay-disp PORT      Relay display port (default: 4411)
--relay-ctrl PORT      Relay control port (default: 4409)
```

## Connection Tolerance

### SDR Server Disconnect
- Stops processing
- Closes relay connections
- Retries every 5 seconds
- Reinitializes DSP filters on reconnect

### Relay Server Disconnect
- **Detector buffer:** 1.5M samples (30 sec @ 50 kHz = 12 MB)
- **Display buffer:** 360k samples (30 sec @ 12 kHz = 2.88 MB)
- Continues receiving from SDR server
- Buffers to ring (discards oldest on overflow)
- Retries every 5 seconds
- Flushes buffered samples on reconnect

### Graceful Shutdown
- Catches SIGINT (Ctrl+C) and SIGTERM
- Flushes remaining frames
- Closes all connections cleanly

## Status Reporting

Every 5 seconds, prints to stderr:

```
[STATUS] Connections: SDR=UP DET=UP DISP=UP
[STATUS] Samples: RX=1234567 DET_TX=30864 DISP_TX=7407
[STATUS] Buffers: DET=0/1500000 (0.0%) DISP=0/360000 (0.0%)
[STATUS] Overflows: DET=0 DISP=0
[STATUS] Dropped: DET=0 DISP=0
```

## Protocol

### Connection Header (sent once at connect)

```c
struct relay_stream_header {
    uint32_t magic;        // 0x46543332 = "FT32"
    uint32_t sample_rate;  // 50000 or 12000
    uint32_t reserved1;
    uint32_t reserved2;
};
```

### Data Frame (continuous)

```c
struct relay_data_frame {
    uint32_t magic;        // 0x44415441 = "DATA"
    uint32_t sequence;     // Frame counter
    uint32_t num_samples;  // I/Q pairs in frame (typically 2048)
    uint32_t reserved;
};
// Followed by: float I/Q pairs (native byte order)
```

### Frame Size
- **2048 samples/frame** (16 KB data)
- Detector: 40.96 ms/frame @ 50 kHz
- Display: 170.67 ms/frame @ 12 kHz

### Byte Order
- **Native float32** (no endian conversion)
- Works for x86 Windows → x86 Linux (both little-endian)
- Control path: UTF-8 text (no encoding issues)

## Operational Model

### Security Approach
- **No authentication** (low-risk RX-only equipment)
- **Manual start/stop** via SSH (hands-on supervision)
- **Firewall access control** (DigitalOcean droplet IP whitelist)
- **Shutdown when not in use** (remote client leaves → splitter stops)

### Typical Workflow
1. Root SSHs to mountain-top system
2. Starts `sdr_server.exe` and `signal_splitter.exe`
3. Remote client connects to relay server
4. Root monitors telemetry/logs
5. Client finishes session → root Ctrl+C to stop programs
6. System idle until next session

### Control Path Security
- **Simple bidirectional relay** (no command filtering)
- Commands: `freq`, `gain`, `status`, etc.
- Trust model: Only authorized users have relay server access
- Risk: Minimal (worst case = retune receiver to wrong frequency)

## Example: Remote Desktop to Mountain-Top

**Root's desktop (controller):**
```bash
ssh root@mountaintop
```

**Mountain-top system:**
```bash
# Start SDR server
.\bin\sdr_server.exe -f 5.000450 -g 59 -l 3

# In another terminal/screen session
.\bin\signal_splitter.exe --relay-host relay.example.com
```

**Relay server receives:**
- Port 4410: Detector stream (50 kHz float32 I/Q)
- Port 4411: Display stream (12 kHz float32 I/Q)
- Port 4409: Control relay (bidirectional text)

## Performance

### Bandwidth Requirements

**Detector Path:**
- 50,000 samples/sec × 2 (I/Q) × 4 bytes (float32) = **400 KB/sec**
- With TCP overhead: ~450 KB/sec

**Display Path:**
- 12,000 samples/sec × 2 (I/Q) × 4 bytes (float32) = **96 KB/sec**
- With TCP overhead: ~110 KB/sec

**Control Path:**
- Negligible (~1 KB/sec typical, sporadic commands/responses)

**Total:** ~560 KB/sec (~4.5 Mbps)

### CPU Usage
- Minimal (4 filter operations per sample @ 2 MHz)
- Estimated: <5% on modern CPU

### Memory Usage
- Ring buffers: 12 MB + 2.88 MB = ~15 MB
- Frame buffers: negligible
- Total: <20 MB

## Next Steps

The relay server (Linux DO droplet) needs to:
1. Listen on ports 4410 and 4411
2. Parse FT32/DATA protocol
3. Distribute streams to multiple clients (waterfall, detectors, etc.)
4. Handle client connects/disconnects without disrupting stream
