# Phoenix SDR TCP Control Interface

## Design Specification for Remote SDR Control

**Document Version:** 1.0
**Date:** December 14, 2025
**Status:** DRAFT
**Authors:** Phoenix Nest Development Team

---

## 1. Executive Summary

This document specifies a TCP/IP socket interface for remote control of the Phoenix SDR (SDRplay RSP2 Pro). The interface enables external applications to:

- Set and query radio frequency
- Adjust gain, LNA, and AGC settings
- Configure sample rate and bandwidth
- Start/stop I/Q streaming
- Receive real-time status updates

The protocol is designed for simplicity, reliability, and compatibility with existing radio control conventions.

---

## 2. Design Goals

| Priority | Goal | Rationale |
|----------|------|-----------|
| **P0** | Text-based protocol | Human-readable for debugging; works with netcat, telnet |
| **P0** | Single-client operation | SDR is not shareable; avoid contention issues |
| **P1** | Low latency | Frequency changes should complete in <100ms |
| **P1** | Hamlib compatibility | Leverage existing client ecosystem where possible |
| **P2** | Extensibility | Support future SDR features without protocol changes |
| **P2** | Error reporting | Clear, consistent error codes and messages |

---

## 3. Protocol Overview

### 3.1 Connection Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Transport | TCP | Reliable, ordered delivery |
| Default Port | **4535** | Avoids conflict with rigctld (4532), rotctld (4533) |
| Encoding | ASCII | All commands/responses are printable ASCII |
| Line Terminator | `\n` (0x0A) | Unix-style newline |
| Max Line Length | 256 bytes | Including terminator |

### 3.2 Connection Behavior

1. **Single Client Only**: Server accepts one connection at a time. Additional connections receive `ERR BUSY\n` and are closed.
2. **Idle Timeout**: Connections idle for >5 minutes may be closed (configurable).
3. **Keepalive**: Clients should send `PING\n` every 60 seconds if idle.
4. **Graceful Disconnect**: Client should send `QUIT\n` before closing socket.

---

## 4. Command Protocol

### 4.1 Basic Syntax

```
COMMAND [ARGUMENT1] [ARGUMENT2] ...\n
```

- Commands are **case-insensitive** (but uppercase is conventional)
- Arguments are space-separated
- Numeric values use decimal notation (floats allowed where noted)
- Frequencies are in **Hz** (not kHz or MHz)

### 4.2 Response Format

**Success (no return value):**
```
OK\n
```

**Success (with return value):**
```
OK <value>\n
```
or for multiple values:
```
OK <key1>=<value1> <key2>=<value2> ...\n
```

**Error:**
```
ERR <CODE> [message]\n
```

### 4.3 Error Codes

| Code | Meaning |
|------|---------|
| `SYNTAX` | Malformed command |
| `UNKNOWN` | Unknown command |
| `PARAM` | Invalid parameter value |
| `RANGE` | Value out of valid range |
| `STATE` | Invalid state for this operation (e.g., already streaming) |
| `BUSY` | SDR is busy (e.g., another client connected) |
| `HARDWARE` | SDR hardware error |
| `TIMEOUT` | Operation timed out |

---

## 5. Command Reference

### 5.1 Frequency Control

#### SET_FREQ - Set Center Frequency

```
SET_FREQ <frequency_hz>\n
```

**Parameters:**
- `frequency_hz`: Center frequency in Hz (1000 to 2000000000)

**Response:**
```
OK\n
```

**Example:**
```
SET_FREQ 15000000\n     → OK\n                    (15 MHz)
SET_FREQ 7255000\n      → OK\n                    (7.255 MHz)
SET_FREQ 50\n           → ERR RANGE freq out of range\n
```

#### GET_FREQ - Get Current Frequency

```
GET_FREQ\n
```

**Response:**
```
OK <frequency_hz>\n
```

**Example:**
```
GET_FREQ\n              → OK 15000000\n
```

---

### 5.2 Gain Control

#### SET_GAIN - Set Gain Reduction

```
SET_GAIN <gain_reduction_db>\n
```

**Parameters:**
- `gain_reduction_db`: Gain reduction in dB (20-59 for RSP2)

**Note:** Higher value = more attenuation = weaker signal

**Response:**
```
OK\n
```

**Example:**
```
SET_GAIN 40\n           → OK\n
SET_GAIN 15\n           → ERR RANGE gain must be 20-59\n
```

#### GET_GAIN - Get Current Gain Reduction

```
GET_GAIN\n
```

**Response:**
```
OK <gain_reduction_db>\n
```

#### SET_LNA - Set LNA State

```
SET_LNA <lna_state>\n
```

**Parameters:**
- `lna_state`: LNA state index (0-8 for RSP2, frequency-dependent)

**Response:**
```
OK\n
```

#### GET_LNA - Get Current LNA State

```
GET_LNA\n
```

**Response:**
```
OK <lna_state>\n
```

#### SET_AGC - Set AGC Mode

```
SET_AGC <mode>\n
```

**Parameters:**
- `mode`: `OFF`, `5HZ`, `50HZ`, or `100HZ`

**Response:**
```
OK\n
```

**Example:**
```
SET_AGC OFF\n           → OK\n
SET_AGC 50HZ\n          → OK\n
SET_AGC AUTO\n          → ERR PARAM unknown AGC mode\n
```

#### GET_AGC - Get Current AGC Mode

```
GET_AGC\n
```

**Response:**
```
OK <mode>\n
```

---

### 5.3 Sample Rate and Bandwidth

#### SET_SRATE - Set Sample Rate

```
SET_SRATE <sample_rate_hz>\n
```

**Parameters:**
- `sample_rate_hz`: Sample rate in Hz (2000000 to 10000000)

**Note:** Cannot be changed while streaming. Stop first.

**Response:**
```
OK\n
```

**Example:**
```
SET_SRATE 2000000\n     → OK\n                    (2 MSPS)
SET_SRATE 6000000\n     → OK\n                    (6 MSPS)
```

#### GET_SRATE - Get Actual Sample Rate

```
GET_SRATE\n
```

**Response:**
```
OK <actual_sample_rate_hz>\n
```

**Note:** Returns actual rate, which may differ slightly from requested.

#### SET_BW - Set IF Bandwidth

```
SET_BW <bandwidth_khz>\n
```

**Parameters:**
- `bandwidth_khz`: 200, 300, 600, 1536, 5000, 6000, 7000, or 8000

**Note:** Cannot be changed while streaming.

**Response:**
```
OK\n
```

#### GET_BW - Get Current Bandwidth

```
GET_BW\n
```

**Response:**
```
OK <bandwidth_khz>\n
```

---

### 5.4 Hardware Configuration

#### SET_ANTENNA - Set Antenna Port

```
SET_ANTENNA <port>\n
```

**Parameters:**
- `port`: `A`, `B`, or `HIZ` (High-Z for low frequencies)

**Response:**
```
OK\n
```

#### GET_ANTENNA - Get Current Antenna

```
GET_ANTENNA\n
```

**Response:**
```
OK <port>\n
```

#### SET_BIAST - Enable/Disable Bias-T

```
SET_BIAST <state>\n
```

**Parameters:**
- `state`: `ON` or `OFF`

**⚠️ WARNING:** Enabling Bias-T supplies DC voltage to antenna port. Can damage equipment if not designed for it.

**Response:**
```
OK\n
```

#### SET_NOTCH - Enable/Disable FM Notch

```
SET_NOTCH <state>\n
```

**Parameters:**
- `state`: `ON` or `OFF`

**Response:**
```
OK\n
```

---

### 5.5 Streaming Control

#### START - Start I/Q Streaming

```
START\n
```

Starts I/Q sample streaming with current configuration.

**Response:**
```
OK\n
```
or
```
ERR STATE already streaming\n
```

#### STOP - Stop I/Q Streaming

```
STOP\n
```

**Response:**
```
OK\n
```
or
```
ERR STATE not streaming\n
```

#### STATUS - Get Streaming Status

```
STATUS\n
```

**Response (not streaming):**
```
OK STREAMING=0 FREQ=15000000 GAIN=40 LNA=4 AGC=OFF SRATE=2000000 BW=200\n
```

**Response (streaming):**
```
OK STREAMING=1 FREQ=15000000 GAIN=40 LNA=4 AGC=OFF SRATE=2000000 BW=200 OVERLOAD=0\n
```

---

### 5.6 Utility Commands

#### PING - Connection Keepalive

```
PING\n
```

**Response:**
```
PONG\n
```

#### VER - Get Version Information

```
VER\n
```

**Response:**
```
OK PHOENIX_SDR=0.3.0 PROTOCOL=1.0 API=3.15\n
```

#### CAPS - Get Capabilities

```
CAPS\n
```

**Response (multi-line):**
```
OK CAPS
FREQ_MIN=1000
FREQ_MAX=2000000000
GAIN_MIN=20
GAIN_MAX=59
LNA_STATES=9
SRATE_MIN=2000000
SRATE_MAX=10000000
BW=200,300,600,1536,5000,6000,7000,8000
ANTENNA=A,B,HIZ
AGC=OFF,5HZ,50HZ,100HZ
END
```

#### HELP - List Available Commands

```
HELP\n
```

**Response:**
```
OK COMMANDS: SET_FREQ GET_FREQ SET_GAIN GET_GAIN SET_LNA GET_LNA SET_AGC GET_AGC SET_SRATE GET_SRATE SET_BW GET_BW SET_ANTENNA GET_ANTENNA SET_BIAST SET_NOTCH START STOP STATUS PING VER CAPS HELP QUIT\n
```

#### QUIT - Disconnect

```
QUIT\n
```

Gracefully closes connection. Server stops streaming if active.

**Response:**
```
BYE\n
```
Then connection is closed.

---

## 6. Asynchronous Notifications

The server may send unsolicited messages to report events:

### 6.1 Notification Format

```
! <EVENT> <data>\n
```

Notifications are prefixed with `!` to distinguish from command responses.

### 6.2 Event Types

#### OVERLOAD - ADC Overload Detected

```
! OVERLOAD DETECTED\n
! OVERLOAD CLEARED\n
```

Sent when ADC overload condition changes. Client should reduce gain.

#### GAIN_CHANGE - AGC Adjusted Gain

```
! GAIN_CHANGE GAIN=35 LNA=4\n
```

Sent when AGC adjusts gain (only if AGC enabled).

#### DISCONNECT - Server Shutting Down

```
! DISCONNECT reason\n
```

Sent before server closes connection (e.g., server shutdown).

---

## 7. Implementation Architecture

### 7.1 Server Components (Phoenix SDR Side)

```
┌─────────────────────────────────────────────────────────────────┐
│                        TCP Server                                │
│  ┌─────────────────┐  ┌──────────────────┐  ┌────────────────┐ │
│  │  Socket Listener │  │ Command Parser   │  │ Response Fmt   │ │
│  │  (port 4535)     │→│ (text → struct)  │→│ (struct → text)│ │
│  └─────────────────┘  └──────────────────┘  └────────────────┘ │
└──────────────────────────────┬──────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                     SDR Control Layer                            │
│  ┌─────────────────┐  ┌──────────────────┐  ┌────────────────┐ │
│  │ Command Handler │  │ State Machine    │  │ Event Queue    │ │
│  │ (dispatch)      │  │ (streaming/idle) │  │ (async notif)  │ │
│  └─────────────────┘  └──────────────────┘  └────────────────┘ │
└──────────────────────────────┬──────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                     Phoenix SDR Library                          │
│  ┌─────────────────┐  ┌──────────────────┐  ┌────────────────┐ │
│  │ psdr_configure  │  │ psdr_start/stop  │  │ psdr_update    │ │
│  └─────────────────┘  └──────────────────┘  └────────────────┘ │
└──────────────────────────────┬──────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                     SDRplay API                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 7.2 Threading Model

1. **Main Thread**: Runs TCP accept loop
2. **Client Thread**: Handles single connected client (read commands, send responses)
3. **SDR Callback Thread**: Owned by SDRplay API (delivers I/Q samples)
4. **Event Thread** (optional): Processes async notifications

### 7.3 Thread Safety

- Command processing is serialized (single client)
- SDR callbacks may arrive during command processing
- Use mutex to protect shared state
- Notification queue decouples callback thread from socket I/O

---

## 8. Client Implementation Guide

### 8.1 Example Session (Netcat)

```bash
$ nc localhost 4535
VER
OK PHOENIX_SDR=0.3.0 PROTOCOL=1.0 API=3.15
SET_FREQ 14100000
OK
SET_GAIN 35
OK
SET_AGC OFF
OK
STATUS
OK STREAMING=0 FREQ=14100000 GAIN=35 LNA=4 AGC=OFF SRATE=2000000 BW=200
START
OK
! OVERLOAD DETECTED
! OVERLOAD CLEARED
STOP
OK
QUIT
BYE
```

### 8.2 Python Client Example

```python
import socket

class PhoenixSDRClient:
    def __init__(self, host='localhost', port=4535):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((host, port))
        self.sock.settimeout(5.0)

    def send_command(self, cmd):
        """Send command and return response."""
        self.sock.sendall((cmd + '\n').encode('ascii'))
        response = b''
        while not response.endswith(b'\n'):
            chunk = self.sock.recv(256)
            if not chunk:
                raise ConnectionError("Server closed connection")
            response += chunk
        return response.decode('ascii').strip()

    def set_freq(self, freq_hz):
        resp = self.send_command(f'SET_FREQ {freq_hz}')
        if not resp.startswith('OK'):
            raise RuntimeError(f"SET_FREQ failed: {resp}")

    def get_freq(self):
        resp = self.send_command('GET_FREQ')
        if resp.startswith('OK '):
            return int(resp[3:])
        raise RuntimeError(f"GET_FREQ failed: {resp}")

    def set_gain(self, gain_db):
        resp = self.send_command(f'SET_GAIN {gain_db}')
        if not resp.startswith('OK'):
            raise RuntimeError(f"SET_GAIN failed: {resp}")

    def start(self):
        resp = self.send_command('START')
        if not resp.startswith('OK'):
            raise RuntimeError(f"START failed: {resp}")

    def stop(self):
        resp = self.send_command('STOP')
        if not resp.startswith('OK'):
            raise RuntimeError(f"STOP failed: {resp}")

    def status(self):
        """Return dict of current status."""
        resp = self.send_command('STATUS')
        if resp.startswith('OK '):
            params = {}
            for item in resp[3:].split():
                if '=' in item:
                    k, v = item.split('=', 1)
                    params[k] = v
            return params
        raise RuntimeError(f"STATUS failed: {resp}")

    def close(self):
        self.send_command('QUIT')
        self.sock.close()


# Example usage:
if __name__ == '__main__':
    sdr = PhoenixSDRClient()

    sdr.set_freq(7255000)       # 7.255 MHz
    sdr.set_gain(40)            # 40 dB gain reduction

    print(sdr.status())

    sdr.start()
    import time
    time.sleep(5)
    sdr.stop()

    sdr.close()
```

### 8.3 C Client Example

```c
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

int send_command(SOCKET sock, const char *cmd, char *response, int resp_size) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s\n", cmd);
    send(sock, buf, strlen(buf), 0);

    int n = recv(sock, response, resp_size - 1, 0);
    if (n > 0) {
        response[n] = '\0';
        // Trim trailing newline
        while (n > 0 && (response[n-1] == '\n' || response[n-1] == '\r')) {
            response[--n] = '\0';
        }
        return (strncmp(response, "OK", 2) == 0) ? 0 : -1;
    }
    return -1;
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_port = htons(4535);

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        printf("Connection failed\n");
        return 1;
    }

    char resp[256];

    send_command(sock, "SET_FREQ 15000000", resp, sizeof(resp));
    printf("SET_FREQ: %s\n", resp);

    send_command(sock, "SET_GAIN 35", resp, sizeof(resp));
    printf("SET_GAIN: %s\n", resp);

    send_command(sock, "STATUS", resp, sizeof(resp));
    printf("STATUS: %s\n", resp);

    send_command(sock, "QUIT", resp, sizeof(resp));

    closesocket(sock);
    WSACleanup();
    return 0;
}
```

---

## 9. Comparison with Existing Protocols

### 9.1 vs Hamlib rigctld

| Feature | rigctld | Phoenix SDR TCP |
|---------|---------|-----------------|
| Command style | Single-char or `\cmd` | Full words |
| SDR-specific | No (radio abstraction) | Yes (direct SDR control) |
| Sample rate control | No | Yes |
| Bandwidth control | Passband only | IF bandwidth |
| I/Q streaming | No | Yes (control only) |
| Extended protocol | `+` prefix | Not needed |

**Compatibility Note:** We could add a `HAMLIB` command to switch to rigctld-compatible mode for frequency/mode control if needed.

### 9.2 vs rtl_tcp

| Feature | rtl_tcp | Phoenix SDR TCP |
|---------|---------|-----------------|
| Transport | TCP + binary I/Q | TCP text commands |
| I/Q delivery | Over TCP | Separate (not in this spec) |
| Commands | Binary (dongle commands) | Text |
| Bidirectional | Commands + I/Q | Commands only |

**Note:** This spec covers control only. I/Q sample delivery is handled separately by the existing `psdr_callbacks_t` mechanism.

---

## 10. Security Considerations

1. **No Authentication**: This protocol has no authentication. Do NOT expose on untrusted networks.

2. **Localhost Only by Default**: Server should bind to `127.0.0.1` by default. Explicit `-T 0.0.0.0` required for network access.

3. **Bias-T Safety**: The `SET_BIAST ON` command requires confirmation: client must send `SET_BIAST ON CONFIRM` to actually enable it.

4. **Firewall**: If exposing to network, recommend SSH tunnel or VPN.

5. **Input Validation**: All numeric inputs must be validated against valid ranges before passing to SDR API.

---

## 11. Future Extensions

### 11.1 Reserved Commands

These command names are reserved for future use:

| Command | Intended Purpose |
|---------|-----------------|
| `STREAM_TCP` | Stream I/Q over TCP (like rtl_tcp) |
| `RECORD` | Record I/Q to file |
| `PLAYBACK` | Playback recorded I/Q |
| `SPECTRUM` | Request FFT/spectrum data |
| `WATERFALL` | Subscribe to waterfall data |
| `MACRO` | Execute command sequence |
| `AUTH` | Authentication extension |

### 11.2 Protocol Versioning

The `VER` command returns `PROTOCOL=1.0`. Future incompatible changes will increment the major version. Clients should check this on connect.

---

## 12. Implementation Checklist

### Phase 1: Core Commands (MVP)
- [ ] TCP server with single-client accept
- [ ] `SET_FREQ`, `GET_FREQ`
- [ ] `SET_GAIN`, `GET_GAIN`
- [ ] `SET_LNA`, `GET_LNA`
- [ ] `START`, `STOP`, `STATUS`
- [ ] `PING`, `VER`, `QUIT`
- [ ] Error handling and responses

### Phase 2: Full Configuration
- [ ] `SET_AGC`, `GET_AGC`
- [ ] `SET_SRATE`, `GET_SRATE`
- [ ] `SET_BW`, `GET_BW`
- [ ] `SET_ANTENNA`, `GET_ANTENNA`
- [ ] `SET_BIAST`, `SET_NOTCH`
- [ ] `CAPS`, `HELP`

### Phase 3: Async Notifications
- [ ] `! OVERLOAD` events
- [ ] `! GAIN_CHANGE` events
- [ ] `! DISCONNECT` events
- [ ] Event queue and delivery

### Phase 4: Testing & Documentation
- [ ] Unit tests for parser
- [ ] Integration tests with SDR
- [ ] Python client library
- [ ] Command-line test client

---

## 13. References

1. **Hamlib rigctld**: http://hamlib.sourceforge.net/html/rigctld.1.html
2. **SDRplay API Specification**: SDRplay_SDR_API_Specification.pdf
3. **Phoenix SDR Library**: phoenix_sdr.h

---

## Appendix A: Quick Reference Card

```
┌──────────────────────────────────────────────────────────────────┐
│                 PHOENIX SDR TCP COMMANDS                          │
├──────────────────────────────────────────────────────────────────┤
│  FREQUENCY                                                        │
│    SET_FREQ <hz>           Set center frequency                  │
│    GET_FREQ                Get current frequency                 │
├──────────────────────────────────────────────────────────────────┤
│  GAIN                                                             │
│    SET_GAIN <db>           Set gain reduction (20-59)            │
│    GET_GAIN                Get current gain reduction            │
│    SET_LNA <state>         Set LNA state (0-8)                   │
│    GET_LNA                 Get current LNA state                 │
│    SET_AGC <mode>          Set AGC (OFF/5HZ/50HZ/100HZ)          │
│    GET_AGC                 Get current AGC mode                  │
├──────────────────────────────────────────────────────────────────┤
│  SAMPLE RATE / BANDWIDTH                                         │
│    SET_SRATE <hz>          Set sample rate (2M-10M Hz)           │
│    GET_SRATE               Get actual sample rate                │
│    SET_BW <khz>            Set IF bandwidth                      │
│    GET_BW                  Get current bandwidth                 │
├──────────────────────────────────────────────────────────────────┤
│  HARDWARE                                                         │
│    SET_ANTENNA <A|B|HIZ>   Select antenna port                   │
│    GET_ANTENNA             Get current antenna                   │
│    SET_BIAST <ON|OFF>      Enable/disable Bias-T ⚠️               │
│    SET_NOTCH <ON|OFF>      Enable/disable FM notch               │
├──────────────────────────────────────────────────────────────────┤
│  STREAMING                                                        │
│    START                   Start I/Q streaming                   │
│    STOP                    Stop I/Q streaming                    │
│    STATUS                  Get full status                       │
├──────────────────────────────────────────────────────────────────┤
│  UTILITY                                                          │
│    PING                    Keepalive (returns PONG)              │
│    VER                     Version info                          │
│    CAPS                    Capabilities                          │
│    HELP                    List commands                         │
│    QUIT                    Disconnect                            │
├──────────────────────────────────────────────────────────────────┤
│  ASYNC NOTIFICATIONS (server → client)                           │
│    ! OVERLOAD DETECTED     ADC overload occurred                 │
│    ! OVERLOAD CLEARED      ADC overload resolved                 │
│    ! GAIN_CHANGE G=x L=y   AGC adjusted gain                     │
│    ! DISCONNECT reason     Server closing connection             │
├──────────────────────────────────────────────────────────────────┤
│  RESPONSES                                                        │
│    OK [value]              Success                               │
│    ERR <CODE> [msg]        Error (SYNTAX/PARAM/RANGE/STATE/...)  │
│    PONG                    Response to PING                      │
│    BYE                     Response to QUIT                      │
└──────────────────────────────────────────────────────────────────┘

Default port: 4535    Single client only    Frequencies in Hz
```

---

**END OF DOCUMENT**
