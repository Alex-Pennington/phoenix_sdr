# Phoenix SDR Runtime Parameter Tuning System

## Overview

The Phoenix SDR waterfall application supports runtime tuning of signal processing parameters via UDP commands and INI file persistence. This enables automated optimization using external controller agents or manual parameter adjustment without recompilation.

**Version:** 1.11.2+157 (December 2025)

### Key Features

- **UDP Command Interface** - Port 3006 (localhost only)
- **INI Persistence** - Automatic save on parameter change
- **Reload on Startup** - `--reload-debug` flag loads saved parameters
- **Validation** - Range checking with fallback to defaults
- **Rate Limiting** - 10 commands/second maximum
- **Telemetry Feedback** - CTRL/RESP channels for command logging
- **22 Total Parameters** - Across 4 detector modules

## Architecture

```
Controller Agent (Python/etc)
         │
         ▼
    UDP:3006 (Command Input)
         │
         ▼
  waterfall.c (Command Processor)
         │
         ├──> tick_detector (4 params)
         ├──> tick_correlator (2 params)
         ├──> marker_detector (3 params)
         └──> sync_detector (13 params)
         │
         ▼
    waterfall.ini (Persistence)
         │
         ▼
  UDP:3005 (Telemetry Output)
         │
         ▼
    Controller Agent (Feedback)
```

## UDP Command Protocol

### Connection Details

- **Protocol:** UDP (connectionless)
- **Port:** 3006
- **Address:** localhost (127.0.0.1)
- **Encoding:** ASCII text
- **Delimiter:** Newline (`\n`)
- **Rate Limit:** 10 commands/second
- **Non-blocking:** Commands processed between SDL event polls

### Command Format

```
<COMMAND_NAME> <VALUE>\n
```

**Examples:**
```
SET_TICK_THRESHOLD 2.5
SET_CORR_CONFIDENCE 0.8
SET_SYNC_LOCKED_THRESHOLD 0.65
```

### Response Format

Commands generate responses on the TELEM_RESP channel (bit 13, UDP:3005):

**Success:**
```
OK <parameter_name>=<value>
```

**Parse Error:**
```
ERR PARSE <COMMAND_NAME> requires numeric value
```

**Rate Limit:**
```
ERR RATE_LIMIT exceeded (10/sec)
```

**Unknown Command:**
```
ERR UNKNOWN_CMD <COMMAND_NAME>
```

## INI File Persistence

### File Location

`waterfall.ini` in the current working directory (typically `bin/`)

### File Format

Standard INI format with multiple `[section]` blocks:

```ini
[tick_detector]
threshold_multiplier=2.997
adapt_alpha_down=0.949037
adapt_alpha_up=0.050517
min_duration_ms=5.50

[tick_correlator]
epoch_confidence_threshold=0.726
max_consecutive_misses=6

[marker_detector]
threshold_multiplier=3.000
noise_adapt_rate=0.001000
min_duration_ms=500.00

[sync_detector]
weight_tick=0.050
weight_marker=0.400
weight_p_marker=0.150
weight_tick_hole=0.200
weight_combined_hole_marker=0.500
confidence_locked_threshold=0.700
confidence_min_retain=0.050
confidence_tentative_init=0.300
confidence_decay_normal=0.9999
confidence_decay_recovering=0.980
tick_phase_tolerance_ms=100.0
marker_tolerance_ms=500.0
p_marker_tolerance_ms=200.0
```

### Save Behavior

- **Trigger:** Immediate on successful UDP command
- **Method:** Full file overwrite with all current values
- **Atomicity:** Not guaranteed (single `fopen("w")`)

### Load Behavior

- **Trigger:** Startup only if `--reload-debug` flag present
- **Missing File:** Silent fallback to compiled defaults
- **Invalid Values:** Warning logged, parameter uses default
- **Unknown Sections:** Silently ignored

## Parameter Reference

### tick_detector (4 parameters)

Controls 5ms tick pulse detection on 1000/1200 Hz tones.

| Parameter | UDP Command | Range | Default | Description |
|-----------|-------------|-------|---------|-------------|
| `threshold_multiplier` | `SET_TICK_THRESHOLD` | 1.0 - 5.0 | 2.0 | Detection sensitivity above noise floor |
| `adapt_alpha_down` | `SET_TICK_ADAPT_DOWN` | 0.9 - 0.999 | 0.995 | Fast noise floor decay (α = 1 - decay_rate) |
| `adapt_alpha_up` | `SET_TICK_ADAPT_UP` | 0.001 - 0.1 | 0.02 | Slow noise floor rise (α form) |
| `min_duration_ms` | `SET_TICK_MIN_DURATION` | 1.0 - 10.0 | 2.0 | Minimum pulse width for valid tick |

**Tuning Notes:**
- Lower `threshold_multiplier` → more sensitive, more false positives
- Higher `adapt_alpha_down` (closer to 1.0) → slower noise decay, better for QRM
- Lower `min_duration_ms` → detect shorter pulses, more noise susceptible

### tick_correlator (2 parameters)

Controls second-epoch correlation and lock persistence.

| Parameter | UDP Command | Range | Default | Description |
|-----------|-------------|-------|---------|-------------|
| `epoch_confidence_threshold` | `SET_CORR_CONFIDENCE` | 0.5 - 0.95 | 0.8 | Minimum confidence to establish lock |
| `max_consecutive_misses` | `SET_CORR_MAX_MISSES` | 2 - 10 | 5 | Missed ticks before losing lock |

**Tuning Notes:**
- Lower `epoch_confidence_threshold` → faster lock, less reliable
- Higher `max_consecutive_misses` → persist through fades, slower reacquisition

### marker_detector (3 parameters)

Controls 800ms minute marker detection.

| Parameter | UDP Command | Range | Default | Description |
|-----------|-------------|-------|---------|-------------|
| `threshold_multiplier` | `SET_MARKER_THRESHOLD` | 2.0 - 5.0 | 3.0 | Detection sensitivity above baseline |
| `noise_adapt_rate` | `SET_MARKER_ADAPT_RATE` | 0.0001 - 0.01 | 0.001 | Baseline tracking speed |
| `min_duration_ms` | `SET_MARKER_MIN_DURATION` | 300.0 - 700.0 | 500.0 | Minimum pulse width for valid marker |

**Tuning Notes:**
- Higher `threshold_multiplier` → reject more noise, miss weak markers
- Higher `noise_adapt_rate` → faster baseline tracking, less stable threshold
- Lower `min_duration_ms` → detect truncated markers, more false positives

### sync_detector (13 parameters)

Controls evidence-based confidence system and LOCKED state gating.

#### Evidence Weights (5 parameters)

| Parameter | UDP Command | Range | Default | Description |
|-----------|-------------|-------|---------|-------------|
| `weight_tick` | `SET_SYNC_WEIGHT_TICK` | 0.01 - 0.2 | 0.05 | Tick evidence contribution to confidence |
| `weight_marker` | `SET_SYNC_WEIGHT_MARKER` | 0.1 - 0.6 | 0.40 | Marker evidence contribution |
| `weight_p_marker` | `SET_SYNC_WEIGHT_P_MARKER` | 0.05 - 0.3 | 0.15 | P-marker evidence contribution |
| `weight_tick_hole` | `SET_SYNC_WEIGHT_TICK_HOLE` | 0.05 - 0.4 | 0.20 | Tick hole (second 29/59) evidence |
| `weight_combined_hole_marker` | `SET_SYNC_WEIGHT_COMBINED` | 0.2 - 0.8 | 0.50 | Combined hole+marker evidence |

**Tuning Notes:**
- Sum of weights does NOT need to equal 1.0 (independent contributions)
- Marker has highest weight (40%) - strongest sync indicator
- Combined hole+marker (50%) - best correlation signal
- Tick weight (5%) - frequent but weak evidence

#### Confidence Thresholds (3 parameters)

| Parameter | UDP Command | Range | Default | Description |
|-----------|-------------|-------|---------|-------------|
| `confidence_locked_threshold` | `SET_SYNC_LOCKED_THRESHOLD` | 0.5 - 0.9 | 0.70 | **CRITICAL** - Gate for LOCKED state |
| `confidence_min_retain` | `SET_SYNC_MIN_RETAIN` | 0.01 - 0.2 | 0.05 | Minimum to stay TENTATIVE/LOCKED |
| `confidence_tentative_init` | `SET_SYNC_TENTATIVE_INIT` | 0.1 - 0.5 | 0.30 | Initial TENTATIVE confidence |

**Tuning Notes:**
- `confidence_locked_threshold` is the PRIMARY optimization target
- Lower threshold → faster lock, less reliable
- Higher `min_retain` → harder to lose sync, slower recovery

#### Decay Rates (2 parameters)

| Parameter | UDP Command | Range | Default | Description |
|-----------|-------------|-------|---------|-------------|
| `confidence_decay_normal` | `SET_SYNC_DECAY_NORMAL` | 0.99 - 0.9999 | 0.9999 | Slow decay when locked/normal |
| `confidence_decay_recovering` | `SET_SYNC_DECAY_RECOVERING` | 0.90 - 0.99 | 0.980 | Fast decay when recovering |

**Tuning Notes:**
- Values are multipliers per periodic check (not per sample)
- Higher (closer to 1.0) → confidence decays slower
- `decay_normal` very high (0.9999) → hold LOCKED through brief fades
- `decay_recovering` lower (0.98) → quickly drop from false locks

#### Tolerances (3 parameters - CURRENTLY UNUSED)

| Parameter | UDP Command | Range | Default | Description |
|-----------|-------------|-------|---------|-------------|
| `tick_phase_tolerance_ms` | `SET_SYNC_TICK_TOLERANCE` | 50.0 - 200.0 | 100.0 | Reserved for tick timing validation |
| `marker_tolerance_ms` | `SET_SYNC_MARKER_TOLERANCE` | 200.0 - 800.0 | 500.0 | Reserved for marker timing validation |
| `p_marker_tolerance_ms` | `SET_SYNC_P_MARKER_TOLERANCE` | 100.0 - 400.0 | 200.0 | Reserved for P-marker timing validation |

**Status:** Parameters are exposed and initialized but NOT referenced in validation code. Reserved for future enhancement.

## Command Line Usage

### Starting with Default Parameters

```powershell
.\bin\waterfall.exe
```

Parameters use compiled `#define` defaults from detector source files.

### Starting with Saved Parameters

```powershell
.\bin\waterfall.exe --reload-debug
```

Loads `waterfall.ini` if present, otherwise uses defaults.

### Recommended Workflow

1. Start waterfall with `--reload-debug`
2. Send UDP commands to optimize parameters
3. Parameters auto-save to `waterfall.ini` on each change
4. Restart with same parameters preserved

## PowerShell Test Client

Basic UDP client for manual testing:

```powershell
# test_udp_cmd.ps1
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Connect("127.0.0.1", 3006)
$cmd = "SET_TICK_THRESHOLD 2.5`n"
$bytes = [System.Text.Encoding]::ASCII.GetBytes($cmd)
$udp.Send($bytes, $bytes.Length) | Out-Null
$udp.Close()
```

Usage:
```powershell
.\test_udp_cmd.ps1
# Check TELEM_RESP output in waterfall console
```

## Python Controller Agent Integration

### Example: scipy.optimize.minimize

```python
import socket
import time
import numpy as np
from scipy.optimize import minimize

class WaterfallController:
    def __init__(self, host='127.0.0.1', port=3006):
        self.host = host
        self.port = port

    def send_command(self, cmd, value):
        """Send UDP command to waterfall"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        message = f"{cmd} {value}\n"
        sock.sendto(message.encode('ascii'), (self.host, self.port))
        sock.close()
        time.sleep(0.05)  # Rate limit

    def set_tick_threshold(self, value):
        self.send_command('SET_TICK_THRESHOLD', value)

    def set_tick_adapt_down(self, value):
        self.send_command('SET_TICK_ADAPT_DOWN', value)

    def set_sync_locked_threshold(self, value):
        self.send_command('SET_SYNC_LOCKED_THRESHOLD', value)

class TelemetryListener:
    def __init__(self, port=3005):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', port))
        self.sock.settimeout(0.1)

    def get_correlation_metrics(self, duration=10.0):
        """Collect tick timing variance from CORR telemetry"""
        variances = []
        start = time.time()
        while time.time() - start < duration:
            try:
                data, _ = self.sock.recvfrom(4096)
                line = data.decode('ascii', errors='ignore')
                if line.startswith('CORR'):
                    # Parse: CORR,timestamp,variance,...
                    parts = line.split(',')
                    if len(parts) > 2:
                        variance = float(parts[2])
                        variances.append(variance)
            except socket.timeout:
                continue
        return np.mean(variances) if variances else 999.9

# Optimization loop
controller = WaterfallController()
listener = TelemetryListener()

def objective(params):
    """Minimize tick timing variance"""
    threshold, adapt_down, locked_thresh = params

    controller.set_tick_threshold(threshold)
    controller.set_tick_adapt_down(adapt_down)
    controller.set_sync_locked_threshold(locked_thresh)

    # Collect metrics
    variance = listener.get_correlation_metrics(duration=30.0)
    return variance

# Initial guess
x0 = [2.0, 0.995, 0.70]

# Bounds
bounds = [(1.0, 5.0),      # threshold_multiplier
          (0.9, 0.999),    # adapt_alpha_down
          (0.5, 0.9)]      # locked_threshold

# Optimize
result = minimize(objective, x0, method='Nelder-Mead',
                  bounds=bounds, options={'maxiter': 100})

print(f"Optimal parameters: {result.x}")
print(f"Final variance: {result.fun}")
```

### Proven Results

Controller agent test (December 2025) achieved **250x improvement** in tick timing variance:

- **Before:** variance = 477.9 ms² (all defaults)
- **After:** variance = 1.9 ms² (optimized tick_detector)
- **Optimized Values:**
  - `threshold_multiplier = 2.997`
  - `adapt_alpha_down = 0.949`
  - `adapt_alpha_up = 0.051`
  - `min_duration_ms = 5.50`

## Multi-Objective Optimization Strategy

### Objective Functions

**Primary:** Minimize tick timing variance
```python
variance = np.std([tick_offset_1, tick_offset_2, ...]) ** 2
```

**Secondary:** Maximize LOCKED time percentage
```python
locked_pct = (time_in_LOCKED / total_time) * 100
```

**Combined:**
```python
objective = variance * 10.0 + (100 - locked_pct)
```

### Parameter Dependencies

Known interactions between detectors:

1. **tick_detector.threshold_multiplier** affects **tick_correlator** lock acquisition
   - Lower threshold → more detections → faster correlation
   - But more false positives → degraded correlation quality

2. **marker_detector.threshold_multiplier** affects **sync_detector** LOCKED gate
   - Higher threshold → fewer markers → less sync evidence
   - Lower threshold → false markers → incorrect confidence boost

3. **sync_detector.confidence_locked_threshold** is final LOCKED gate
   - Overrides all upstream detectors if confidence insufficient
   - Optimize LAST after tick/marker detectors tuned

### Recommended Tuning Order

1. **tick_detector** (4 params) - Optimize tick timing variance
2. **tick_correlator** (2 params) - Optimize lock acquisition speed
3. **marker_detector** (3 params) - Optimize marker detection rate
4. **sync_detector** (13 params) - Optimize LOCKED state achievement

## Telemetry Monitoring

### CTRL Channel (bit 12)

Logs all received UDP commands:

```
CTRL SET_TICK_THRESHOLD 2.5
CTRL SET_CORR_CONFIDENCE 0.8
```

### RESP Channel (bit 13)

Logs command responses:

```
RESP OK threshold_multiplier=2.500
RESP OK epoch_confidence_threshold=0.800
RESP ERR PARSE SET_INVALID requires numeric value
RESP ERR RATE_LIMIT exceeded (10/sec)
```

### Filtering with telem_logger

```powershell
# Log only control/response traffic
.\bin\telem_logger.exe --channels CTRL,RESP -o logs/
```

## Validation and Safety

### Parameter Bounds Enforcement

All setters validate ranges before applying:

```c
bool tick_detector_set_threshold_mult(tick_detector_t *td, float mult) {
    if (!td || mult < 1.0f || mult > 5.0f) {
        return false;  // Validation failed
    }
    td->threshold_multiplier = mult;
    return true;
}
```

Invalid commands logged but do NOT crash waterfall:

```
[WARN] Invalid threshold_multiplier=10.0 in INI, using default
```

### Rate Limiting

Protects against command flooding:

- **Limit:** 10 commands/second
- **Window:** Rolling 1-second buckets
- **Overflow:** Commands rejected with `ERR RATE_LIMIT`
- **Purpose:** Prevent UDP spam from degrading signal processing

### UDP Security

**Current:** Localhost only (127.0.0.1)
- No network exposure
- Safe for single-machine optimization

**Future:** If network tuning needed:
- Bind to specific interface only
- Add authentication tokens
- Use TLS encryption (switch to TCP)

## Troubleshooting

### Parameters Not Loading from INI

**Symptom:** Waterfall starts with defaults despite valid `waterfall.ini`

**Solution:** Add `--reload-debug` flag:
```powershell
.\bin\waterfall.exe --reload-debug
```

### Parameters Not Saving

**Symptom:** UDP commands accepted but `waterfall.ini` unchanged

**Check:**
1. File permissions (write access to `bin/`)
2. TELEM_RESP output - should show `OK` response
3. File locked by editor/other process

### UDP Commands Ignored

**Symptom:** No response on TELEM_RESP channel

**Check:**
1. Port 3006 not blocked by firewall
2. Command format (space separator, newline terminator)
3. Numeric value parsing (`2.5` not `2,5`)
4. Rate limit (max 10/sec)

### Parameter Has No Effect

**Symptom:** Parameter changes but signal processing unchanged

**Causes:**
1. **Hot path not updated** - C code still uses `#define` not struct member
2. **Wrong detector** - Changing correlator when tick_detector is problem
3. **Tolerance parameters** - Currently unused (tick/marker/p_marker tolerance)

**Debug:**
1. Check detector source for struct member usage
2. Verify parameter used in hot path (not just getter/setter)
3. Monitor telemetry for expected behavior change

## API Reference Summary

### tick_detector Commands
```
SET_TICK_THRESHOLD <1.0-5.0>
SET_TICK_ADAPT_DOWN <0.9-0.999>
SET_TICK_ADAPT_UP <0.001-0.1>
SET_TICK_MIN_DURATION <1.0-10.0>
```

### tick_correlator Commands
```
SET_CORR_CONFIDENCE <0.5-0.95>
SET_CORR_MAX_MISSES <2-10>
```

### marker_detector Commands
```
SET_MARKER_THRESHOLD <2.0-5.0>
SET_MARKER_ADAPT_RATE <0.0001-0.01>
SET_MARKER_MIN_DURATION <300.0-700.0>
```

### sync_detector Commands
```
SET_SYNC_WEIGHT_TICK <0.01-0.2>
SET_SYNC_WEIGHT_MARKER <0.1-0.6>
SET_SYNC_WEIGHT_P_MARKER <0.05-0.3>
SET_SYNC_WEIGHT_TICK_HOLE <0.05-0.4>
SET_SYNC_WEIGHT_COMBINED <0.2-0.8>
SET_SYNC_LOCKED_THRESHOLD <0.5-0.9>
SET_SYNC_MIN_RETAIN <0.01-0.2>
SET_SYNC_TENTATIVE_INIT <0.1-0.5>
SET_SYNC_DECAY_NORMAL <0.99-0.9999>
SET_SYNC_DECAY_RECOVERING <0.90-0.99>
SET_SYNC_TICK_TOLERANCE <50.0-200.0>
SET_SYNC_MARKER_TOLERANCE <200.0-800.0>
SET_SYNC_P_MARKER_TOLERANCE <100.0-400.0>
```

## Version History

- **v1.11.2+145** - Initial tick_detector parameters (4)
- **v1.11.2+149** - Added tick_correlator parameters (2)
- **v1.11.2+151** - Added marker_detector parameters (3)
- **v1.11.2+157** - Added sync_detector parameters (13) - **COMPLETE**

## Future Enhancements

### GET Commands
Query current parameter values without telemetry parsing:
```
GET_TICK_THRESHOLD
RESP threshold_multiplier=2.500
```

### Parameter Presets
INI sections for different signal conditions:
```ini
[weak_signal]
tick_detector.threshold_multiplier=1.5
sync_detector.confidence_locked_threshold=0.55

[strong_signal]
tick_detector.threshold_multiplier=3.5
sync_detector.confidence_locked_threshold=0.85
```

Command: `LOAD_PRESET weak_signal`

### Auto-Tuning Mode
Built-in optimization loop:
```
START_AUTOTUNE variance 60
RESP AUTOTUNE_STARTED duration=60s objective=variance
...
RESP AUTOTUNE_COMPLETE variance=2.1 iterations=47
```

### Parameter Change History
CSV log of all parameter changes:
```csv
timestamp_ms,detector,parameter,old_value,new_value,source
1234567.8,tick_detector,threshold_multiplier,2.0,2.5,UDP
1234580.2,sync_detector,locked_threshold,0.7,0.65,UDP
```

## References

- **UDP Telemetry Protocol:** [UDP_TELEMETRY_OUTPUT_PROTOCOL.md](UDP_TELEMETRY_OUTPUT_PROTOCOL.md)
- **Sync Detector Spec:** [UNIFIED_SYNC_IMPLEMENTATION_SPEC.md](UNIFIED_SYNC_IMPLEMENTATION_SPEC.md)
- **WWV Signal Characteristics:** [wwv_signal_characteristics.md](wwv_signal_characteristics.md)
- **Source Code:**
  - `tools/waterfall.c` - UDP command processor, INI persistence
  - `tools/tick_detector.c/h` - Tick pulse detection parameters
  - `tools/tick_correlator.c/h` - Epoch correlation parameters
  - `tools/marker_detector.c/h` - Minute marker detection parameters
  - `tools/sync_detector.c/h` - Sync confidence system parameters

---

**Last Updated:** December 20, 2025
**Phoenix SDR Version:** 1.11.2+157
