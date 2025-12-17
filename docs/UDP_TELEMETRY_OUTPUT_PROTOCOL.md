# UDP Telemetry Output Protocol

Phoenix SDR waterfall broadcasts real-time telemetry data via UDP broadcast on port 3005.

## Overview

- **Port:** 3005 (UDP broadcast)
- **Address:** 255.255.255.255 (broadcast)
- **Format:** CSV with 4-character channel prefix
- **Rate:** ~1 message per second per enabled channel
- **Non-blocking:** Fire-and-forget, no acknowledgment required

## Listening

### PowerShell
```powershell
$udp = New-Object System.Net.Sockets.UdpClient(3005)
$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
while($true) {
    $bytes = $udp.Receive([ref]$ep)
    [System.Text.Encoding]::ASCII.GetString($bytes)
}
```

### Netcat (Linux/WSL)
```bash
nc -u -l 3005
```

---

## Channel Messages

### CHAN - Channel Quality

Broadcast every ~1 second. Overall signal quality metrics.

**Format:** `CHAN,time,timestamp_ms,carrier_db,snr_db,sub500_db,sub600_db,tone1000_db,noise_db,quality\n`

| Field | Type | Description |
|-------|------|-------------|
| `time` | string | Wall clock time `HH:MM:SS` |
| `timestamp_ms` | float | Milliseconds since waterfall start |
| `carrier_db` | float | Carrier power level (dB) |
| `snr_db` | float | Signal-to-noise ratio (dB), tone1000 vs noise floor |
| `sub500_db` | float | 500 Hz subcarrier power (dB) |
| `sub600_db` | float | 600 Hz subcarrier power (dB) |
| `tone1000_db` | float | 1000 Hz tick tone power (dB) |
| `noise_db` | float | Noise floor estimate (dB) |
| `quality` | string | `GOOD` (>15dB), `FAIR` (>8dB), `POOR` (>3dB), `NONE` |

**Example:**
```
CHAN,14:32:15,85320.0,-45.2,18.3,-52.1,-58.4,-38.5,-56.8,GOOD
```

---

### CARR - Carrier Frequency Tracker

Broadcast every ~1 second when carrier tracking is valid. Measures DC/carrier frequency offset.

**Format:** `CARR,time,timestamp_ms,measured_hz,offset_hz,offset_ppm,snr_db\n`

| Field | Type | Description |
|-------|------|-------------|
| `time` | string | Wall clock time `HH:MM:SS` |
| `timestamp_ms` | float | Milliseconds since waterfall start |
| `measured_hz` | float | Measured carrier frequency (Hz) - should be ~0 for DC |
| `offset_hz` | float | Frequency offset from nominal (Hz) |
| `offset_ppm` | float | Frequency offset in parts per million |
| `snr_db` | float | Carrier signal-to-noise ratio (dB) |

**Example:**
```
CARR,14:32:15,85320.0,0.125,0.125,0.01,35.2
```

---

### T500 - 500 Hz Tone Tracker

Broadcast every ~1 second when 500 Hz tone tracking is valid.

**Format:** `T500,time,timestamp_ms,measured_hz,offset_hz,offset_ppm,snr_db\n`

| Field | Type | Description |
|-------|------|-------------|
| `time` | string | Wall clock time `HH:MM:SS` |
| `timestamp_ms` | float | Milliseconds since waterfall start |
| `measured_hz` | float | Measured 500 Hz tone frequency (Hz) |
| `offset_hz` | float | Offset from 500 Hz nominal (Hz) |
| `offset_ppm` | float | Frequency offset in parts per million |
| `snr_db` | float | Tone signal-to-noise ratio (dB) |

**Example:**
```
T500,14:32:15,85320.0,500.023,0.023,0.05,22.5
```

---

### T600 - 600 Hz Tone Tracker

Broadcast every ~1 second when 600 Hz tone tracking is valid.

**Format:** `T600,time,timestamp_ms,measured_hz,offset_hz,offset_ppm,snr_db\n`

| Field | Type | Description |
|-------|------|-------------|
| `time` | string | Wall clock time `HH:MM:SS` |
| `timestamp_ms` | float | Milliseconds since waterfall start |
| `measured_hz` | float | Measured 600 Hz tone frequency (Hz) |
| `offset_hz` | float | Offset from 600 Hz nominal (Hz) |
| `offset_ppm` | float | Frequency offset in parts per million |
| `snr_db` | float | Tone signal-to-noise ratio (dB) |

**Example:**
```
T600,14:32:15,85320.0,599.987,-0.013,-0.02,19.8
```

---

### SUBC - Subcarrier Analysis

Broadcast every ~1 second. Compares 500 Hz vs 600 Hz subcarrier per WWV schedule.

**Format:** `SUBC,time,timestamp_ms,minute,expected,sub500_db,sub600_db,delta_db,detected,match\n`

| Field | Type | Description |
|-------|------|-------------|
| `time` | string | Wall clock time `HH:MM:SS` |
| `timestamp_ms` | float | Milliseconds since waterfall start |
| `minute` | int | Current minute of the hour (0-59) |
| `expected` | string | Expected tone per WWV schedule: `500Hz`, `600Hz`, or `NONE` |
| `sub500_db` | float | 500 Hz power (dB) |
| `sub600_db` | float | 600 Hz power (dB) |
| `delta_db` | float | Difference: sub500_db - sub600_db |
| `detected` | string | Detected tone: `500Hz` (delta>3), `600Hz` (delta<-3), `NONE` |
| `match` | string | `YES` if detected matches expected, `NO` if mismatch, `-` if expected=NONE |

**Example:**
```
SUBC,14:32:15,85320.0,32,500Hz,-52.1,-58.4,6.3,500Hz,YES
```

---

### MARK - Minute Marker Events

Broadcast when a minute marker is detected (long ~800ms pulse at second 0 or 59).

**Format:** `MARK,time,timestamp_ms,marker_num,wwv_sec,expected,accum_energy,duration_ms,since_last_sec,baseline,threshold\n`

| Field | Type | Description |
|-------|------|-------------|
| `time` | string | Wall clock time `HH:MM:SS` |
| `timestamp_ms` | float | Milliseconds since waterfall start |
| `marker_num` | string | Marker number (e.g., `M1`, `M2`, ...) |
| `wwv_sec` | int | WWV second position (0-59) |
| `expected` | string | Expected WWV event at this second |
| `accum_energy` | float | Accumulated energy during marker |
| `duration_ms` | float | Marker pulse duration (ms), typically ~800ms |
| `since_last_sec` | float | Time since last marker (seconds) |
| `baseline` | float | Energy baseline level |
| `threshold` | float | Detection threshold |

**Example:**
```
MARK,14:33:00,145320.0,M3,0,MINUTE_MARKER,0.045623,823.5,60.02,0.000123,0.001234
```

---

### SYNC - Synchronization State

Broadcast when sync state changes or markers are confirmed.

**Format:** `SYNC,time,timestamp_ms,marker_num,state,interval_sec,delta_ms,tick_dur_ms,marker_dur_ms\n`

| Field | Type | Description |
|-------|------|-------------|
| `time` | string | Wall clock time `HH:MM:SS` |
| `timestamp_ms` | float | Milliseconds since waterfall start |
| `marker_num` | int | Confirmed marker count |
| `state` | string | Sync state: `SEARCHING`, `ACQUIRING`, `LOCKED` |
| `interval_sec` | float | Interval between markers (seconds) |
| `delta_ms` | float | Timing error (ms) from expected |
| `tick_dur_ms` | float | Last tick pulse duration (ms) |
| `marker_dur_ms` | float | Last marker pulse duration (ms) |

**Example:**
```
SYNC,14:33:00,145320.0,3,LOCKED,60.0,12,5.1,823.5
```

---

## Reserved Channels (Not Yet Implemented)

| Prefix | Channel Enum | Description |
|--------|--------------|-------------|
| `TICK` | `TELEM_TICKS` | Tick pulse detection events |
| `CORR` | `TELEM_CORR` | Tick correlation data |

---

## Enabling/Disabling Channels

Channels are controlled via bitmask in waterfall.c:

```c
#include "waterfall_telemetry.h"

telem_init(3005);
telem_enable(TELEM_CHANNEL | TELEM_CARRIER | TELEM_SUBCAR | TELEM_TONE500 | TELEM_TONE600);
```

Channel bitmask values:
| Channel | Bit | Value |
|---------|-----|-------|
| `TELEM_CHANNEL` | 0 | 0x001 |
| `TELEM_TICKS` | 1 | 0x002 |
| `TELEM_MARKERS` | 2 | 0x004 |
| `TELEM_CARRIER` | 3 | 0x008 |
| `TELEM_SYNC` | 4 | 0x010 |
| `TELEM_SUBCAR` | 5 | 0x020 |
| `TELEM_CORR` | 6 | 0x040 |
| `TELEM_TONE500` | 7 | 0x080 |
| `TELEM_TONE600` | 8 | 0x100 |
| `TELEM_ALL` | - | 0x1FF |

---

## Implementation Files

- `tools/waterfall_telemetry.h` - API header
- `tools/waterfall_telemetry.c` - UDP broadcast implementation
- `test/test_telemetry.c` - Unit tests
