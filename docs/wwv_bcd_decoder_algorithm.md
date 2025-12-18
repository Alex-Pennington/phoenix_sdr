# WWV BCD Time Code Decoder Algorithm

> ⚠️ **PARTIALLY SUPERSEDED** - December 18, 2025
> 
> **Superseded content:**
> - Input sample rate is now 50kHz (not 48kHz) - avoids sample drops
> - Goertzel approach replaced by FFT-based detection in bcd_time_detector.c and bcd_freq_detector.c
> - Pulse classification now done via bcd_correlator.c with window-based integration
> - Output sample rate and decimation factors have changed
> 
> **Still valuable:** WWV BCD pulse encoding (200/500/800ms), frame structure, time code format

## Overview

This document describes the algorithm for decoding the Binary Coded Decimal (BCD) time code transmitted by WWV and WWVH on a 100 Hz subcarrier. The time code presents UTC information in serial fashion at a rate of 1 pulse per second, including the current minute, hour, and day of year.

## SDRuno SP2 Reference Parameters

Based on reverse-engineering of SDRuno's AUX SP (SP2) display:

| Parameter | Value |
|-----------|-------|
| Span | 7.3 kHz |
| FFT Size | 2048 points |
| RBW (binwidth) | 5.86 Hz |
| Derived Sample Rate | ~12 kHz (FFT × RBW) |
| Window Function | Sin³ (NENBW ≈ 1.73) |
| Effective RBW | ~10 Hz |

## Input Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Input Sample Rate | 48 kHz | From SDR capture |
| Output Sample Rate | 2.4 kHz | After decimation |
| Target Frequency | 100 Hz | BCD subcarrier |
| Pulse Rate | 1 PPS | Pulse per second |

## WWV BCD Pulse Encoding

| Pulse Width | Duration | Meaning |
|-------------|----------|---------|
| Short | ~200 ms | Binary 0 |
| Medium | ~500 ms | Binary 1 |
| Long | ~800 ms | Position Marker (P) |

## Algorithm Pipeline

```
┌─────────────────────────────────────────────────────────────────┐
│                    WWV BCD DECODER PIPELINE                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  48 kHz IQ ──┬──► Decimate ──► 2.5 kHz ──► Goertzel ──► Envelope │
│              │      ÷20                    @100 Hz      Detect   │
│              │                                             │     │
│              │                                             ▼     │
│              │                                      Pulse Width  │
│              │                                       Measure     │
│              │                                             │     │
│              │                                             ▼     │
│              │                                      Symbol       │
│              │                                      Classify     │
│              │                                      (0/1/P)      │
│              │                                             │     │
│              │                                             ▼     │
│              │                                      Frame Sync   │
│              │                                      (find P0)    │
│              │                                             │     │
│              │                                             ▼     │
│              │                                      BCD Decode   │
│              │                                      MM:HH:DDD    │
│              │                                                   │
│              └──► Waterfall Display (parallel path)              │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Stage 1: Decimation (48 kHz → 2.4 kHz)

Two-stage decimation for better filter performance.

```python
# Decimation configuration
DECIM_STAGE1 = 4   # 48000 → 12000 Hz
DECIM_STAGE2 = 5   # 12000 → 2400 Hz
FINAL_FS = 2400    # Final sample rate
```

### Rationale

- Two-stage approach allows for gentler anti-alias filters
- 2400 Hz provides 24× oversampling of 100 Hz target
- Clean integer decimation factors

---

## Stage 2: 100 Hz Extraction (Goertzel Filter)

Goertzel algorithm is optimal for single-frequency detection - O(N) complexity vs O(N log N) for full FFT.

```python
# Goertzel parameters for 100 Hz detection
BLOCK_SIZE = 24          # samples per block (10 ms at 2400 Hz)
BLOCKS_PER_SECOND = 100  # 10 ms resolution for envelope

# Goertzel coefficient for 100 Hz at 2400 Hz sample rate
k = (BLOCK_SIZE * 100) / FINAL_FS  # = 1.0 (bin 1)
omega = (2 * pi * k) / BLOCK_SIZE
coeff = 2 * cos(omega)

def goertzel_magnitude(samples):
    """
    Compute magnitude of 100 Hz component using Goertzel algorithm.
    
    Args:
        samples: Array of BLOCK_SIZE samples
        
    Returns:
        Magnitude of 100 Hz component
    """
    s0, s1, s2 = 0, 0, 0
    for sample in samples:
        s0 = sample + coeff * s1 - s2
        s2 = s1
        s1 = s0
    return sqrt(s1*s1 + s2*s2 - coeff*s1*s2)
```

### Why Goertzel?

1. Single frequency of interest (100 Hz)
2. Much more efficient than FFT for single bin
3. Lower latency
4. Simpler implementation

---

## Stage 3: Envelope Detection & Smoothing

```python
# Running envelope with simple low-pass
# Output: 100 values per second (10 ms spacing)

envelope_buffer = RingBuffer(100)  # 1 second history

def update_envelope(goertzel_mag):
    """
    Apply exponential smoothing to Goertzel magnitude.
    
    Args:
        goertzel_mag: Raw magnitude from Goertzel filter
        
    Returns:
        Smoothed envelope value
    """
    alpha = 0.3
    smoothed = alpha * goertzel_mag + (1 - alpha) * prev_smoothed
    envelope_buffer.push(smoothed)
    return smoothed
```

---

## Stage 4: Pulse Width Measurement

```python
# Threshold-based edge detection with hysteresis
THRESHOLD_HIGH = noise_floor + 6  # dB - Pulse ON
THRESHOLD_LOW = noise_floor + 3   # dB - Pulse OFF (hysteresis)

# At 100 samples/sec (10 ms resolution):
#   200 ms pulse = ~20 samples
#   500 ms pulse = ~50 samples  
#   800 ms pulse = ~80 samples

state = IDLE
pulse_start = 0
pulse_width = 0

def detect_pulse(envelope_sample, sample_idx):
    """
    Detect pulse edges and measure width.
    
    Args:
        envelope_sample: Current envelope value
        sample_idx: Current sample index (10 ms ticks)
        
    Returns:
        Classified symbol (BIT_ZERO, BIT_ONE, POSITION_MARK) or None
    """
    global state, pulse_start
    
    if state == IDLE and envelope_sample > THRESHOLD_HIGH:
        state = IN_PULSE
        pulse_start = sample_idx
    elif state == IN_PULSE and envelope_sample < THRESHOLD_LOW:
        state = IDLE
        pulse_width = sample_idx - pulse_start
        return classify_pulse(pulse_width)
    return None
```

### Hysteresis Thresholding

Using separate high/low thresholds prevents chatter on noisy pulse edges:

```
Signal Level
     │
  +6 dB ─────────────────── THRESHOLD_HIGH (turn ON)
     │         ┌─────┐
     │         │     │
  +3 dB ──────┼─────┼───── THRESHOLD_LOW (turn OFF)
     │        │     │
     │────────┘     └────── 
     │
  Noise Floor
     └─────────────────────► Time
```

---

## Stage 5: Symbol Classification

```python
# Pulse width thresholds (in 10ms units)
def classify_pulse(width_samples):
    """
    Classify pulse width into symbol type.
    
    Args:
        width_samples: Pulse width in 10ms sample units
        
    Returns:
        Symbol type (BIT_ZERO, BIT_ONE, or POSITION_MARK)
    """
    width_ms = width_samples * 10  # Convert to ms
    
    if width_ms < 350:
        return BIT_ZERO      # ~200 ms nominal
    elif width_ms < 650:
        return BIT_ONE       # ~500 ms nominal
    else:
        return POSITION_MARK # ~800 ms nominal
```

### Classification Boundaries

```
    0 ms     200 ms    350 ms    500 ms    650 ms    800 ms   1000 ms
    │         │         │         │         │         │         │
    │◄───────────────►│         │         │         │         │
    │    BIT_ZERO     │         │         │         │         │
    │   (nominal)     │◄───────────────────►│        │         │
    │                 │       BIT_ONE       │        │         │
    │                 │      (nominal)      │◄──────────────────►
    │                 │                     │   POSITION_MARK   │
    │                 │                     │     (nominal)     │
```

---

## Stage 6: Frame Synchronization

### WWV Frame Structure (60 seconds)

```
Second 0:  P0 (reference marker - start of minute)
Sec 1-8:   Minutes BCD (40, 20, 10, 0, 8, 4, 2, 1)
Second 9:  P1

Sec 10-18: Hours BCD (0, 0, 20, 10, 0, 8, 4, 2, 1)
Second 19: P2

Sec 20-28: Day of year hundreds/tens (200, 100, 0, 80, 40, 20, 10, 0)
Second 29: P3

Sec 30-38: Day units + DUT1 sign (8, 4, 2, 1, 0, +, +, -)
Second 39: P4

Sec 40-48: DUT1 magnitude + misc
Second 49: P5

Sec 50-58: Year + leap second/DST warnings
Second 59: P0 (may be omitted on leap second insertion)
```

### Position Marker Pattern

Position markers (P) occur at seconds: **0, 9, 19, 29, 39, 49, 59**

This creates a unique pattern that can be used for frame synchronization.

```python
# Frame sync state machine
symbol_buffer = RingBuffer(60)
sync_state = SEARCHING

# Expected position marker locations
P_POSITIONS = [0, 9, 19, 29, 39, 49, 59]

def process_symbol(symbol, second_tick):
    """
    Process incoming symbol and maintain frame sync.
    
    Args:
        symbol: Classified symbol (0, 1, or P)
        second_tick: Second boundary indicator
        
    Returns:
        Decoded UTC time if frame complete, else None
    """
    global sync_state
    
    symbol_buffer.push(symbol)
    
    if sync_state == SEARCHING:
        # Look for P markers at expected positions
        if detect_frame_pattern(symbol_buffer):
            sync_state = SYNCED
            frame_offset = calculate_offset()
    
    elif sync_state == SYNCED:
        if is_frame_complete():
            return decode_frame(symbol_buffer)
    
    return None

def detect_frame_pattern(buffer):
    """
    Check if buffer contains valid position marker pattern.
    
    Returns:
        True if valid P marker pattern detected
    """
    # Count P markers and verify spacing
    p_count = 0
    for i, symbol in enumerate(buffer):
        if symbol == POSITION_MARK:
            # Check if this P is at expected offset from previous
            p_count += 1
    
    # Need at least 3 consecutive P markers at correct spacing
    return p_count >= 3 and verify_spacing(buffer)
```

---

## Stage 7: BCD Decode

```python
def decode_frame(symbols):
    """
    Decode complete 60-symbol frame into UTC time.
    
    Args:
        symbols: Array of 60 symbols (0, 1, or P)
        
    Returns:
        UTC_Time object with hours, minutes, day of year
    """
    # Minutes: seconds 1-8 
    # Bit weights: 40, 20, 10, (unused), 8, 4, 2, 1
    minutes = (symbols[1] * 40 + 
               symbols[2] * 20 + 
               symbols[3] * 10 + 
               # symbols[4] unused (always 0)
               symbols[5] * 8 + 
               symbols[6] * 4 + 
               symbols[7] * 2 + 
               symbols[8] * 1)
    
    # Hours: seconds 12-18
    # Bit weights: (unused), (unused), 20, 10, (unused), 8, 4, 2, 1
    hours = (symbols[12] * 20 + 
             symbols[13] * 10 + 
             # symbols[14] unused
             symbols[15] * 8 + 
             symbols[16] * 4 + 
             symbols[17] * 2 + 
             symbols[18] * 1)
    
    # Day of year: seconds 22-33
    # Hundreds and tens: 200, 100, (unused), 80, 40, 20, 10, (unused)
    # Units: 8, 4, 2, 1
    doy = (symbols[22] * 200 + 
           symbols[23] * 100 +
           # symbols[24] unused
           symbols[25] * 80 + 
           symbols[26] * 40 +
           symbols[27] * 20 + 
           symbols[28] * 10 +
           # symbols[29] is P3
           symbols[30] * 8 + 
           symbols[31] * 4 +
           symbols[32] * 2 + 
           symbols[33] * 1)
    
    return UTC_Time(hours=hours, minutes=minutes, day_of_year=doy)
```

### BCD Frame Map

```
Sec  | Bit Weight | Field
-----|------------|------------------
  0  | P          | Position Marker 0 (minute reference)
  1  | 40         | Minutes (tens)
  2  | 20         | Minutes (tens)
  3  | 10         | Minutes (tens)
  4  | 0          | (unused)
  5  | 8          | Minutes (units)
  6  | 4          | Minutes (units)
  7  | 2          | Minutes (units)
  8  | 1          | Minutes (units)
  9  | P          | Position Marker 1
 10  | 0          | (unused)
 11  | 0          | (unused)
 12  | 20         | Hours (tens)
 13  | 10         | Hours (tens)
 14  | 0          | (unused)
 15  | 8          | Hours (units)
 16  | 4          | Hours (units)
 17  | 2          | Hours (units)
 18  | 1          | Hours (units)
 19  | P          | Position Marker 2
 20  | 0          | (unused)
 21  | 0          | (unused)
 22  | 200        | Day of year (hundreds)
 23  | 100        | Day of year (hundreds)
 24  | 0          | (unused)
 25  | 80         | Day of year (tens)
 26  | 40         | Day of year (tens)
 27  | 20         | Day of year (tens)
 28  | 10         | Day of year (tens)
 29  | P          | Position Marker 3
 30  | 8          | Day of year (units)
 31  | 4          | Day of year (units)
 32  | 2          | Day of year (units)
 33  | 1          | Day of year (units)
 34  | 0          | (unused)
 35  | +          | DUT1 sign (+)
 36  | +          | DUT1 sign (+)
 37  | -          | DUT1 sign (-)
 38  | 0          | (unused)
 39  | P          | Position Marker 4
 40  | 0.8        | DUT1 magnitude
 41  | 0.4        | DUT1 magnitude
 42  | 0.2        | DUT1 magnitude
 43  | 0.1        | DUT1 magnitude
 44  | 0          | (reserved)
 45  | 0          | (reserved)
 46  | 0          | (reserved)
 47  | 0          | (reserved)
 48  | 0          | (reserved)
 49  | P          | Position Marker 5
 50  | 0          | (unused)
 51  | 80         | Year (tens)
 52  | 40         | Year (tens)
 53  | 20         | Year (tens)
 54  | 10         | Year (tens)
 55  | 8          | Year (units)
 56  | 4          | Year (units)
 57  | 2          | Year (units)
 58  | 1          | Year (units)
 59  | P          | Position Marker 0 (next minute)
```

---

## Key Design Decisions

### 1. Goertzel vs FFT

**Decision:** Use Goertzel algorithm

**Rationale:**
- Single frequency of interest (100 Hz)
- O(N) complexity vs O(N log N) for FFT
- Lower memory footprint
- Lower latency
- Simpler implementation

### 2. Envelope Resolution

**Decision:** 10 ms resolution (100 samples/second)

**Rationale:**
- 20 samples across shortest pulse (200 ms)
- Sufficient margin for width measurement
- Low computational overhead

### 3. Hysteresis Thresholding

**Decision:** 3 dB separation between ON/OFF thresholds

**Rationale:**
- Prevents chatter on noisy edges
- Provides stable state transitions
- Simple to implement and tune

### 4. Parallel Processing Paths

**Decision:** Waterfall display and BCD decoder run independently

**Rationale:**
- Display can run at different update rate
- Decoder failure doesn't affect display
- Easier debugging/testing

---

## Implementation Notes

### Noise Floor Estimation

```python
def estimate_noise_floor(envelope_buffer, percentile=10):
    """
    Estimate noise floor from envelope history.
    
    Uses lower percentile of recent values to avoid
    including pulse energy in estimate.
    """
    sorted_values = sorted(envelope_buffer)
    idx = int(len(sorted_values) * percentile / 100)
    return sorted_values[idx]
```

### Timing Considerations

- At 2400 Hz sample rate, each sample is 416.67 µs
- Goertzel block (24 samples) = 10 ms
- Minimum pulse (200 ms) = 20 blocks
- Maximum pulse (800 ms) = 80 blocks

### Error Handling

1. **Lost sync:** If 3 consecutive P markers are missed, return to SEARCHING state
2. **Invalid BCD:** If decoded value exceeds valid range (e.g., minutes > 59), discard frame
3. **Noise burst:** Ignore pulses shorter than 100 ms or longer than 1000 ms

---

## References

- NIST Special Publication 432: NIST Time and Frequency Services
- WWV/WWVH broadcast format documentation
- SDRuno User Manual v1.42
- SDRplay API documentation

---

## Revision History

| Date | Author | Description |
|------|--------|-------------|
| 2024-12-16 | Claude/Rayven | Initial algorithm design based on SDRuno SP2 analysis |
