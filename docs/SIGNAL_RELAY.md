# Signal Relay Server

Linux broadcast server for remote SDR signal distribution.

## Architecture

```
                         ┌──────────────────────┐
                         │   signal_splitter    │  Mountain-top
                         │   (Windows client)   │
                         └──────────┬───────────┘
                                    │
                   ┌────────────────┼────────────────┐
                   │                │                │
         Detector  │      Display   │     Control    │
         50 kHz    │      12 kHz    │     Text       │
         Port 4410 │      Port 4411 │     Port 4409  │
                   │                │                │
                   ▼                ▼                ▼
         ┌─────────────────────────────────────────────┐
         │          signal_relay (Linux server)        │
         │          - Accepts 1 source per stream      │
         │          - Broadcasts to N clients          │
         │          - Per-client ring buffers          │
         │          - Control: 1 source + 1 client     │
         └─────────────────────────────────────────────┘
                   │                │                │
         ┌─────────┴─────┐  ┌───────┴──────┐  ┌──────┴────────┐
         ▼               ▼  ▼              ▼  ▼               ▼
     Client 1        Client 2          Client 3          Control Client
     (waterfall)     (logger)          (analyzer)        (remote user)
```

## Ports

| Port | Stream | Sample Rate | Format | Connections |
|------|--------|-------------|--------|-------------|
| 4410 | Detector | 50 kHz | float32 I/Q | 1 source → N clients |
| 4411 | Display | 12 kHz | float32 I/Q | 1 source → N clients |
| 4409 | Control | N/A | Text | 1 source ↔ 1 client |

## Connection Model

### I/Q Streams (Detector/Display)
- **Source:** Single connection from `signal_splitter`
- **Clients:** Multiple simultaneous connections (waterfall, loggers, analyzers)
- **Buffering:** Per-client 30-second ring buffer (tolerates slow clients)
- **Broadcast:** All clients receive same data with independent flow control

### Control Stream
- **Source:** Single connection from `signal_splitter` (forwards SDR commands/responses)
- **Client:** Single remote user connection
- **Relay:** Bidirectional passthrough (no command parsing)
- **Rejection:** Additional connections rejected while occupied

## Usage

### Relay Server Setup (DigitalOcean Droplet)

```bash
# Compile on Linux
gcc -O3 -o signal_relay signal_relay.c -lm

# Run with nohup (survives SSH disconnect)
nohup ./signal_relay > relay.log 2>&1 &

# Monitor status
tail -f relay.log
```

### Client Connection Examples

**Waterfall client:**
```bash
# Connect to detector stream
telnet relay.example.com 4410
# (Receives FT32 magic + float32 I/Q frames)
```

**Control client:**
```bash
# Connect to control relay
telnet relay.example.com 4409

# Send commands (forwarded to SDR via signal_splitter)
freq 10.0005
gain 59
status

# Receive responses
OK freq 10.0005
OK gain 59
Freq: 10.000500 MHz, Gain: 59 dB, LNA: 1
```

## Protocol Details

### I/Q Frame Format
```c
struct relay_data_frame {
    uint32_t magic;        // 0x46543332 = "FT32"
    uint32_t sequence;     // Frame counter
    uint32_t num_samples;  // I/Q pairs in frame (2048)
    uint32_t reserved;
};
// Followed by: float32 I/Q pairs (native byte order)
```

### Control Protocol
- **Format:** UTF-8 text lines (newline-terminated)
- **Direction:** Bidirectional
- **Commands:** `freq <MHz>`, `gain <dB>`, `status`, etc.
- **Responses:** `OK <params>`, error messages, status reports

## Status Reporting

Logs printed every 30 seconds:
```
[STATUS] Uptime: 3600 sec
[STATUS] Detector: source=UP clients=3 (total_served=5)
[STATUS]   Relayed: 1440000000 bytes, 87890 frames
[STATUS] Display: source=UP clients=2 (total_served=3)
[STATUS]   Relayed: 345600000 bytes, 21094 frames
[STATUS] Control: source=UP client=CONNECTED
```

## Connection Tolerance

### Source Disconnect (signal_splitter dies)
- Closes all client connections for that stream
- Waits for source to reconnect
- Clients must also reconnect after source returns

### Client Disconnect
- Removes client from broadcast list
- Frees ring buffer
- Other clients unaffected
- Client can reconnect anytime

### Client Slow/Stalled
- Data queued in per-client ring buffer (30 seconds @ stream rate)
- If buffer full: oldest frames dropped (ring wraparound)
- Allows slow clients without blocking fast clients

## Performance

### Bandwidth (per client)
- Detector: ~400 KB/sec
- Display: ~96 KB/sec
- Control: <1 KB/sec (sporadic)

### Scaling
- **1 client:** ~500 KB/sec total
- **5 clients:** ~2.5 MB/sec total
- **10 clients:** ~5 MB/sec total

Tested on DigitalOcean $6/month droplet (1 vCPU, 1GB RAM).

### CPU Usage
- Minimal (select()-based I/O multiplexing)
- Estimated: <2% per client on modern CPU

### Memory Usage
- **Per detector client:** ~12 MB ring buffer
- **Per display client:** ~3 MB ring buffer
- **Base overhead:** ~1 MB
- **10 clients:** ~75 MB total

## Operational Model

### Security
- **No authentication** (relay open to internet, firewall-controlled)
- **Trust model:** Only authorized IPs whitelisted on DigitalOcean firewall
- **Risk:** Low (RX-only equipment, worst case = bad data to clients)

### Supervision
- **Manual start:** SSH to droplet, run `nohup ./signal_relay &`
- **Monitoring:** `tail -f relay.log` for status updates
- **Shutdown:** `killall signal_relay` when not in use

### Typical Session
1. Admin starts relay server on droplet
2. Mountain-top starts `signal_splitter` → connects to relay
3. Remote clients connect → receive streams
4. Session ends → admin kills relay, mountain-top stops splitter
5. System idle until next session

## Troubleshooting

### "Address already in use"
```bash
# Find process using port
sudo lsof -i :4410

# Kill stale process
kill <PID>
```

### No data received
- Check relay status log: is source connected?
- Verify firewall allows inbound on 4410/4411/4409
- Check signal_splitter is running and connected

### Control commands not working
- Verify both control source (signal_splitter) and client connected
- Check relay status: `Control: source=UP client=CONNECTED`
- Ensure only one control client (additional rejected)

## Implementation Notes

### select() Multiplexing
- Single-threaded event loop
- Scales well to dozens of clients
- Non-blocking I/O prevents stalls

### Ring Buffer Flow Control
- Independent per-client buffering
- Slow clients don't block fast clients
- Graceful degradation (drop oldest on overflow)

### Connection State Machine
```
LISTEN → ACCEPT → READ/WRITE → DISCONNECT → CLEANUP
```

Each socket tracked independently with proper cleanup on disconnect.
