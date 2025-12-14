# Phoenix SDR I/Q Streaming Interface

## Design Specification for TCP I/Q Sample Delivery

**Document Version:** 1.0  
**Date:** December 14, 2025  
**Status:** DRAFT  
**Authors:** Phoenix Nest Development Team

---

## 1. Executive Summary

This document specifies a TCP/IP interface for streaming raw I/Q samples from the Phoenix SDR server to client applications (e.g., waterfall display, signal analyzers). This is a **companion interface** to the TCP Control Interface (port 4535).

**Key Design:**
- **Port 4535**: Control commands (text protocol) - existing
- **Port 4536**: I/Q sample stream (binary protocol) - NEW

---

## 2. Design Goals

| Priority | Goal | Rationale |
|----------|------|-----------|
| **P0** | Binary efficiency | Minimize overhead for high sample rates |
| **P0** | Simple framing | Easy to parse in any language |
| **P1** | Low latency | Real-time display requirements |
| **P1** | Backpressure handling | Don't overflow slow clients |
| **P2** | Metadata sync | Client knows sample rate, format |

---

## 3. Protocol Overview

### 3.1 Connection Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Transport | TCP | Reliable, ordered delivery |
| Default Port | **4536** | I/Q stream port (control is 4535) |
| Encoding | Binary | Little-endian |
| Client Model | Single client | Same as control port |

### 3.2 Connection Behavior

1. **Client connects** to port 4536
2. **Server sends header** with stream parameters
3. **Client issues START** on control port (4535)
4. **Server streams I/Q data** continuously until STOP
5. **Client disconnects** or server sends disconnect notification

### 3.3 Relationship to Control Port

```
┌─────────────────────────────────────────────────────────────────┐
│                        CLIENT APPLICATION                        │
│                         (e.g., waterfall)                        │
│  ┌─────────────────────────────┐  ┌────────────────────────────┐│
│  │   Control Connection        │  │   I/Q Stream Connection    ││
│  │   Port 4535 (text)          │  │   Port 4536 (binary)       ││
│  │   SET_FREQ, START, STOP...  │  │   Raw I/Q samples          ││
│  └──────────────┬──────────────┘  └──────────────┬─────────────┘│
└─────────────────┼─────────────────────────────────┼──────────────┘
                  │                                 │
                  ▼                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│                        SDR SERVER                                │
│  ┌─────────────────────────────┐  ┌────────────────────────────┐│
│  │   Control Thread            │  │   I/Q Stream Thread        ││
│  │   Parse commands            │  │   Send sample buffers      ││
│  │   Update hardware           │  │   Handle backpressure      ││
│  └──────────────┬──────────────┘  └──────────────┬─────────────┘│
│                 │                                 │              │
│                 └────────────────┬────────────────┘              │
│                                  ▼                               │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                   SDR Callback Thread                        ││
│  │   Receives samples from SDRplay API                          ││
│  │   Pushes to ring buffer → I/Q Stream Thread reads            ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
```

---

## 4. Binary Protocol

### 4.1 Stream Header (sent once on connect)

When a client connects to port 4536, the server immediately sends a header:

```c
struct iq_stream_header {
    uint32_t magic;           // 0x50485849 = "PHXI" (Phoenix IQ)
    uint32_t version;         // Protocol version (1)
    uint32_t sample_rate;     // Current sample rate in Hz
    uint32_t sample_format;   // Format code (see below)
    uint32_t center_freq_lo;  // Center frequency low 32 bits
    uint32_t center_freq_hi;  // Center frequency high 32 bits
    uint32_t reserved[2];     // Future use (0)
};  // Total: 32 bytes
```

**Sample Format Codes:**

| Code | Format | Bytes/Sample | Description |
|------|--------|--------------|-------------|
| 1 | `IQ_S16` | 4 | Interleaved int16 I, int16 Q (native from SDRplay) |
| 2 | `IQ_F32` | 8 | Interleaved float32 I, float32 Q |
| 3 | `IQ_U8` | 2 | Interleaved uint8 I, uint8 Q (rtl_tcp compatible) |

**Default:** `IQ_S16` (format code 1) - native SDRplay format, most efficient.

### 4.2 Data Frames (sent continuously while streaming)

Each data frame contains a batch of I/Q samples:

```c
struct iq_data_frame {
    uint32_t magic;           // 0x49514451 = "IQDQ" (IQ Data)
    uint32_t sequence;        // Frame sequence number (wraps at 2^32)
    uint32_t num_samples;     // Number of I/Q pairs in this frame
    uint32_t flags;           // Bit flags (see below)
    // Followed by: num_samples * sample_size bytes of I/Q data
};  // Header: 16 bytes, then variable data
```

**Flags:**

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | `OVERLOAD` | ADC overload detected in this frame |
| 1 | `FREQ_CHANGE` | Frequency was changed (re-read header) |
| 2 | `GAIN_CHANGE` | Gain was changed by AGC |
| 3-31 | Reserved | Must be 0 |

### 4.3 Metadata Update (sent when parameters change)

When frequency, sample rate, or format changes mid-stream:

```c
struct iq_metadata_update {
    uint32_t magic;           // 0x4D455441 = "META"
    uint32_t sample_rate;     // New sample rate in Hz
    uint32_t sample_format;   // New format code
    uint32_t center_freq_lo;  // New center frequency low 32 bits
    uint32_t center_freq_hi;  // New center frequency high 32 bits
    uint32_t reserved[3];     // Future use (0)
};  // Total: 32 bytes
```

### 4.4 Example Data Flow

```
Client connects to port 4536
  ← Server sends: iq_stream_header (32 bytes)

Client connects to port 4535, sends: START
  ← Control port responds: OK

  ← I/Q port sends: iq_data_frame header (16 bytes)
  ← I/Q port sends: I/Q samples (num_samples * 4 bytes for S16)
  ← I/Q port sends: iq_data_frame header
  ← I/Q port sends: I/Q samples
  ... continues ...

Client sends on control port: SET_FREQ 10000000
  ← Control port responds: OK
  ← I/Q port sends: iq_metadata_update (32 bytes)
  ← I/Q port continues with data frames

Client sends on control port: STOP
  ← Control port responds: OK
  (I/Q stream pauses, connection stays open)

Client sends on control port: QUIT
  ← Control port responds: BYE
  (Both connections close)
```

---

## 5. Sample Format Details

### 5.1 IQ_S16 Format (Default)

Native format from SDRplay API. Most efficient, no conversion needed.

```
Byte offset:  0   1   2   3   4   5   6   7   ...
              I0L I0H Q0L Q0H I1L I1H Q1L Q1H ...
              └─I0──┘ └─Q0──┘ └─I1──┘ └─Q1──┘
```

- I and Q are signed 16-bit integers, little-endian
- Range: -32768 to +32767
- 4 bytes per sample pair

### 5.2 IQ_F32 Format

Normalized floating-point format. Easier math, larger bandwidth.

```
Byte offset:  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  ...
              └────I0 float───┘ └────Q0 float───┘ └────I1 float───┘ └────Q1 float───┘
```

- I and Q are IEEE 754 float32, little-endian
- Range: approximately -1.0 to +1.0 (normalized)
- 8 bytes per sample pair

### 5.3 IQ_U8 Format (rtl_tcp Compatible)

Unsigned 8-bit format, compatible with rtl_tcp clients.

```
Byte offset:  0   1   2   3   ...
              I0  Q0  I1  Q1  ...
```

- I and Q are unsigned 8-bit integers
- Range: 0-255 (128 = zero)
- 2 bytes per sample pair
- **Note:** Requires conversion from S16, loses precision

---

## 6. Backpressure and Flow Control

### 6.1 Problem

At 2 MSPS with IQ_S16 format:
- Data rate = 2,000,000 × 4 = **8 MB/s**
- If client can't keep up, buffers overflow

### 6.2 Solution: Ring Buffer with Drop Policy

```
┌─────────────────────────────────────────────────────────────────┐
│                     RING BUFFER (e.g., 4 MB)                     │
│  ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐              │
│  │frame│frame│frame│frame│     │     │     │     │              │
│  │  1  │  2  │  3  │  4  │empty│empty│empty│empty│              │
│  └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘              │
│       ↑                   ↑                                      │
│     read_ptr           write_ptr                                 │
│   (I/Q Thread)        (SDR Callback)                            │
└─────────────────────────────────────────────────────────────────┘
```

**Rules:**
1. SDR callback writes to ring buffer (never blocks)
2. If buffer full, **drop oldest frames** (increment read_ptr)
3. I/Q thread reads from buffer, sends to TCP
4. If TCP send would block, use non-blocking I/O with timeout
5. Track dropped frames in `sequence` gaps (client can detect)

### 6.3 Client Detection of Drops

Client monitors `sequence` field in `iq_data_frame`:
- If sequence jumps (e.g., 100 → 105), 4 frames were dropped
- Client can log warning, adjust visualization

---

## 7. Implementation Details

### 7.1 Server-Side Changes to sdr_server.c

```c
/* New globals */
static SOCKET g_iq_listen_socket = INVALID_SOCKET;
static SOCKET g_iq_client_socket = INVALID_SOCKET;
static volatile bool g_iq_connected = false;

/* Ring buffer */
#define IQ_RING_BUFFER_SIZE (4 * 1024 * 1024)  /* 4 MB */
static uint8_t g_iq_ring_buffer[IQ_RING_BUFFER_SIZE];
static volatile size_t g_iq_write_pos = 0;
static volatile size_t g_iq_read_pos = 0;
static volatile uint32_t g_iq_sequence = 0;

/* In SDR callback (called by SDRplay API): */
void on_samples(const int16_t *xi, const int16_t *xq, uint32_t count, void *ctx) {
    if (!g_iq_connected) return;
    
    /* Interleave I/Q and write to ring buffer */
    /* ... */
}

/* I/Q streaming thread: */
DWORD WINAPI iq_stream_thread(LPVOID param) {
    while (g_running) {
        /* Accept client on port 4536 */
        /* Send header */
        /* Loop: read from ring buffer, send frames */
    }
    return 0;
}
```

### 7.2 Command-Line Options

```
sdr_server.exe [options]
  -p PORT    Control port (default: 4535)
  -i PORT    I/Q stream port (default: 4536)
  -I         Disable I/Q streaming (control only)
  -f FORMAT  I/Q format: s16, f32, u8 (default: s16)
```

### 7.3 New Control Commands

Add to existing TCP control protocol (port 4535):

| Command | Description |
|---------|-------------|
| `SET_IQFORMAT <s16\|f32\|u8>` | Set I/Q sample format |
| `GET_IQFORMAT` | Get current I/Q format |
| `GET_IQPORT` | Get I/Q streaming port number |
| `GET_IQSTATS` | Get I/Q stream statistics (frames sent, dropped) |

---

## 8. Client Implementation Guide

### 8.1 C Client Example

```c
#include <stdio.h>
#include <stdint.h>
#include <winsock2.h>

#pragma pack(push, 1)
struct iq_stream_header {
    uint32_t magic;
    uint32_t version;
    uint32_t sample_rate;
    uint32_t sample_format;
    uint32_t center_freq_lo;
    uint32_t center_freq_hi;
    uint32_t reserved[2];
};

struct iq_data_frame {
    uint32_t magic;
    uint32_t sequence;
    uint32_t num_samples;
    uint32_t flags;
};
#pragma pack(pop)

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    
    /* Connect to I/Q stream port */
    SOCKET iq_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_port = htons(4536);
    connect(iq_sock, (struct sockaddr *)&server, sizeof(server));
    
    /* Read header */
    struct iq_stream_header header;
    recv(iq_sock, (char *)&header, sizeof(header), 0);
    
    if (header.magic != 0x50485849) {
        printf("Invalid magic\n");
        return 1;
    }
    
    printf("Sample rate: %u Hz\n", header.sample_rate);
    printf("Format: %u\n", header.sample_format);
    
    /* Now connect to control port and send START */
    /* ... */
    
    /* Read I/Q frames */
    while (1) {
        struct iq_data_frame frame;
        recv(iq_sock, (char *)&frame, sizeof(frame), 0);
        
        if (frame.magic != 0x49514451) {
            printf("Invalid frame magic\n");
            continue;
        }
        
        /* Read sample data */
        size_t data_size = frame.num_samples * 4;  /* S16 format */
        int16_t *samples = malloc(data_size);
        recv(iq_sock, (char *)samples, data_size, 0);
        
        /* Process samples... */
        printf("Frame %u: %u samples\n", frame.sequence, frame.num_samples);
        
        free(samples);
    }
    
    closesocket(iq_sock);
    WSACleanup();
    return 0;
}
```

### 8.2 Integration with Waterfall

The waterfall application currently reads from stdin (pipe):

```powershell
# Current usage (pipe):
simple_am_receiver.exe | waterfall.exe

# New usage (TCP):
waterfall.exe --tcp 127.0.0.1:4536
```

Waterfall changes:
1. Add TCP client mode (`--tcp host:port`)
2. Read stream header, parse sample rate
3. Read data frames in a loop
4. Feed samples to FFT/display pipeline

---

## 9. Performance Considerations

### 9.1 Bandwidth Requirements

| Sample Rate | Format | Data Rate | Notes |
|-------------|--------|-----------|-------|
| 2 MSPS | S16 | 8 MB/s | Typical for HF work |
| 2 MSPS | F32 | 16 MB/s | Higher precision |
| 2 MSPS | U8 | 4 MB/s | rtl_tcp compatible |
| 6 MSPS | S16 | 24 MB/s | Wideband capture |
| 10 MSPS | S16 | 40 MB/s | Maximum rate |

### 9.2 Network Considerations

- **Localhost**: No practical limit, 40 MB/s is fine
- **Gigabit LAN**: ~100 MB/s theoretical, 40 MB/s practical
- **100 Mbit LAN**: ~10 MB/s, limits to ~2.5 MSPS with S16

### 9.3 Buffer Sizing

| Buffer Size | Duration at 2 MSPS | Trade-off |
|-------------|-------------------|-----------|
| 1 MB | 125 ms | Low latency, risk of drops |
| 4 MB | 500 ms | Balanced (recommended) |
| 16 MB | 2 seconds | High latency, fewer drops |

---

## 10. Implementation Checklist

### Phase 1: Basic Streaming
- [ ] Add second listening socket on port 4536
- [ ] Implement ring buffer for I/Q samples
- [ ] Send stream header on connect
- [ ] Stream data frames while START active
- [ ] Handle client disconnect gracefully

### Phase 2: Robustness
- [ ] Implement backpressure/drop handling
- [ ] Add sequence numbers for drop detection
- [ ] Send metadata updates on freq/rate change
- [ ] Add `GET_IQPORT`, `GET_IQSTATS` commands

### Phase 3: Format Options
- [ ] Implement S16 → F32 conversion
- [ ] Implement S16 → U8 conversion
- [ ] Add `SET_IQFORMAT` command

### Phase 4: Client Integration
- [ ] Update waterfall.c to accept TCP input
- [ ] Add `--tcp` command-line option
- [ ] Test end-to-end with live SDR

---

## 11. Quick Reference

### Magic Numbers

| Magic | Hex | Meaning |
|-------|-----|---------|
| `PHXI` | 0x50485849 | Stream header |
| `IQDQ` | 0x49514451 | Data frame |
| `META` | 0x4D455441 | Metadata update |

### Port Assignments

| Port | Protocol | Purpose |
|------|----------|---------|
| 4535 | Text/ASCII | Control commands |
| 4536 | Binary | I/Q sample stream |

### Default Configuration

| Parameter | Default |
|-----------|---------|
| I/Q Format | S16 (native) |
| Ring Buffer | 4 MB |
| Frame Size | ~8192 samples (~32 KB) |

---

**END OF DOCUMENT**
