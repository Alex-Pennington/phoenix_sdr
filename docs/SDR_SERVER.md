# Phoenix SDR Server

TCP server for remote control and I/Q streaming from SDRplay RSP2 Pro hardware.

## Quick Start

```powershell
# Start server with default settings
.\bin\sdr_server.exe

# Custom ports and address
.\bin\sdr_server.exe -p 4535 -i 4536 -T 0.0.0.0

# Specific device and logging
.\bin\sdr_server.exe -d 1 -l

# Start minimized with logging
.\bin\sdr_server.exe -m -l
```

## Command-Line Options

```
Usage: sdr_server.exe [options]

Options:
  -p PORT    Control port (default: 4535)
  -i PORT    I/Q stream port (default: 4536)
  -T ADDR    Listen address (default: 127.0.0.1)
  -I         Disable I/Q streaming port
  -d INDEX   Select SDR device index (default: 0)
  -l         Log output to file (sdr_server_<version>.log)
  -m         Start minimized (or hidden if -l also set)
  -h         Show this help
```

## Ports and Protocols

| Port | Protocol | Direction | Purpose | Documentation |
|------|----------|-----------|---------|---------------|
| 4535 | TCP Text | Bidirectional | Control commands/responses | [SDR_TCP_CONTROL_INTERFACE.md](SDR_TCP_CONTROL_INTERFACE.md) |
| 4536 | TCP Binary | Server→Client | I/Q sample stream (2 MHz int16) | [SDR_IQ_STREAMING_INTERFACE.md](SDR_IQ_STREAMING_INTERFACE.md) |

### Listen Address Options

| Address | Description | Use Case |
|---------|-------------|----------|
| `127.0.0.1` | Localhost only (default) | Local waterfall/tools, secure |
| `0.0.0.0` | All interfaces | Remote access (firewall required) |
| Specific IP | Single interface | Multi-homed systems |

## Control Interface (Port 4535)

Text-based command protocol. See [SDR_TCP_CONTROL_INTERFACE.md](SDR_TCP_CONTROL_INTERFACE.md) for full specification.

### Common Commands

```
freq 5.0005          # Set frequency to 5.0005 MHz
gain 59              # Set gain reduction to 59 dB
lna 1                # Set LNA state (0-8)
antenna A            # Select antenna port (A, B, Z)
start                # Start streaming I/Q
stop                 # Stop streaming I/Q
status               # Query current settings
exit                 # Disconnect client
```

### Example Session

```powershell
# Connect with telnet
telnet localhost 4535

# Send commands
freq 10.0
OK freq 10.0000000

gain 40
OK gain 40

status
Freq: 10.000000 MHz, Gain: 40 dB, LNA: 1, Ant: A, Streaming: no
```

## I/Q Streaming (Port 4536)

Binary protocol with framed int16 I/Q samples at 2 MHz. See [SDR_IQ_STREAMING_INTERFACE.md](SDR_IQ_STREAMING_INTERFACE.md) for details.

### Frame Format

```c
struct iq_frame_header {
    uint32_t magic;            // 0x49514451 = "IQDQ"
    uint32_t sequence;         // Frame counter
    uint32_t num_samples;      // I/Q pairs in frame (8192)
    uint32_t sample_rate;      // Samples/sec (2000000)
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t center_freq_lo;   // Frequency in Hz (low 32 bits)
    uint32_t center_freq_hi;   // Frequency in Hz (high 32 bits)
};
// Followed by: int16_t samples[num_samples * 2] (IQIQIQ...)
```

### Client Example

```powershell
# Connect to I/Q stream
$tcp = New-Object System.Net.Sockets.TcpClient("localhost", 4536)
$stream = $tcp.GetStream()

# Read header (32 bytes)
$header = New-Object byte[] 32
$stream.Read($header, 0, 32)

# Read I/Q samples
$samples = New-Object byte[] 32768  # 8192 samples × 2 (I/Q) × 2 bytes
$stream.Read($samples, 0, 32768)
```

## Device Selection

### Query Available Devices

```powershell
# Start server with logging to see device enumeration
.\bin\sdr_server.exe -l

# Check log file
cat sdr_server_*.log
```

Output example:
```
Found 2 SDRplay device(s):
  [0] RSPduo SerNum=123456 HWVer=1 Mode=Single
  [1] RSP2 SerNum=789012 HWVer=2
Using device [0]: RSPduo SerNum=123456
```

### Select Specific Device

```powershell
# Use second device (index 1)
.\bin\sdr_server.exe -d 1
```

## Logging

### File Logging (`-l` option)

Creates timestamped log file:
```
sdr_server_v0.8.11-beta_20251220_143022.log
```

Log content:
- Device enumeration and selection
- Client connections/disconnections
- Command processing (requests and responses)
- Streaming state changes
- Hardware errors and warnings
- Async notifications (gain changes, overloads)

### Minimize/Hide Window (`-m` option)

```powershell
# Minimized but visible on taskbar
.\bin\sdr_server.exe -m

# Completely hidden (requires -l to monitor)
.\bin\sdr_server.exe -m -l
```

## Connection Model

### Single Client per Port
- Control port: 1 client at a time
- I/Q port: 1 client at a time
- New connection kicks existing client

### Independent Port Operation
- Control and I/Q ports operate independently
- Can have control client without I/Q streaming
- I/Q streaming requires `start` command via control port

### Typical Usage Patterns

**Pattern 1: Waterfall Display**
```
Client connects to port 4536 → Server auto-starts streaming → Waterfall receives I/Q
Client disconnects → Server stops streaming
```

**Pattern 2: Manual Control**
```
Client connects to port 4535 → Send commands → Server responds
Control client can remain connected without I/Q streaming
```

**Pattern 3: Combined (signal_splitter)**
```
Splitter connects to port 4536 (I/Q) → Auto-start streaming
Splitter connects to port 4535 (Control) → Forward commands from remote
Both connections persistent until disconnect
```

## Network Topology Examples

### Local Operation
```
┌───────────┐
│ Waterfall │ localhost:4536 (I/Q)
└───────────┘
```

### Remote Operation (NAT-Friendly)
```
Mountain-Top                     Internet                 Remote Site
┌────────────┐                                          ┌────────────┐
│ sdr_server │ ← 127.0.0.1:4536 ← signal_splitter ───→ │   Relay    │
│  -T 127... │                        │                 │   Server   │
└────────────┘                        │                 └──────┬─────┘
                                      │                        │
                                      └─ 127.0.0.1:4535       │
                                         (control)             ▼
                                                         Clients...
```

## Security Considerations

### Default (127.0.0.1)
- **Access:** Localhost only
- **Risk:** Low (local processes)
- **Use Case:** Single-user workstation

### Open (0.0.0.0)
- **Access:** All network interfaces
- **Risk:** Medium (no authentication)
- **Mitigation:**
  - Firewall rules (allow specific IPs)
  - Non-routable network (192.168.x.x)
  - VPN tunnel
  - Manual supervision

### Best Practices
1. Use `-T 127.0.0.1` unless remote access required
2. If remote access needed, use firewall whitelist or VPN
3. Start/stop manually (don't run as background service)
4. Monitor logs (`-l`) for unauthorized connections
5. RX-only operation limits risk (worst case = wrong frequency)

## Troubleshooting

### "Failed to initialize SDR hardware"
- Verify SDRplay RSP2 Pro connected via USB
- Install SDRplay API 3.x (check `C:\Program Files\SDRplay\API\`)
- Try different device index (`-d 0`, `-d 1`)
- Check Windows Device Manager for driver issues

### "Address already in use"
- Another instance of sdr_server running
- Kill existing process:
  ```powershell
  Get-Process sdr_server | Stop-Process
  ```
- Or use different port:
  ```powershell
  .\bin\sdr_server.exe -p 4537 -i 4538
  ```

### "Connection refused" (client)
- Verify sdr_server running
- Check port numbers match
- If using `-T` specific address, client must connect to that address
- Firewall may be blocking port

### No I/Q data received
- Send `start` command via control port (if not auto-started)
- Check control port status: `status` command should show `Streaming: yes`
- Verify client reading correct port (4536 default)

### Frequent disconnects
- Network instability (check ping times)
- Client processing too slow (data overflow)
- USB bandwidth issues (try different USB port)

## Performance

### CPU Usage
- Idle: <1% (waiting for connections)
- Streaming: 5-10% (2 MHz I/Q transfer)

### Memory Usage
- Base: ~50 MB
- Per client: +10 MB (buffers)

### Network Bandwidth
- I/Q stream: ~8 MB/sec (2 MHz × 2 channels × 2 bytes/sample)
- Control: <1 KB/sec (text commands)

## Implementation Notes

### Hardware Abstraction
Uses `phoenix_sdr.h` abstraction layer for SDRplay hardware:
- `psdr_device_info_t` for device enumeration
- `psdr_rx_config_t` for RX configuration
- `psdr_stream_callback_fn` for I/Q delivery

### Streaming Callback
Hardware streams directly to client via callback:
```c
void stream_callback(const int16_t *samples, uint32_t count) {
    // Frame header + samples → TCP client
}
```

### Async Notifications
Server sends unsolicited messages on control port:
```
! GAIN_CHANGE 45    # AGC adjusted gain
! OVERLOAD          # ADC overload detected
```

## Related Documentation

- **Control Protocol:** [SDR_TCP_CONTROL_INTERFACE.md](SDR_TCP_CONTROL_INTERFACE.md) - Full command reference
- **I/Q Streaming:** [SDR_IQ_STREAMING_INTERFACE.md](SDR_IQ_STREAMING_INTERFACE.md) - Binary protocol details
- **Signal Splitter:** [SIGNAL_SPLITTER.md](SIGNAL_SPLITTER.md) - Remote relay client
- **Waterfall Client:** [SDR_WATERFALL_AND_AM_DEMODULATION.md](SDR_WATERFALL_AND_AM_DEMODULATION.md) - Display client

## Version History

- **v0.8.11-beta:** Current release with control/I/Q ports
- **v0.8.x:** Added I/Q streaming port (4536)
- **v0.7.x:** Initial TCP control interface (4535)
