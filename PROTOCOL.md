# MIL-STD-188-110A Protocol Reference

## Overview

MIL-STD-188-110A defines interoperable HF data modems for military communications. This document summarizes key protocol elements.

## Physical Layer

### Symbol Rate
- **2400 baud** for all modes
- Symbol period: 416.67 μs

### Carrier Frequency
- Nominal: 1800 Hz
- Range: 500-3000 Hz (voice band)
- Recommended: 1500-2100 Hz

### Modulation
| Mode | Modulation | Bits/Symbol |
|------|------------|-------------|
| 75 bps | Walsh/8-PSK | Special |
| 150 bps | BPSK | 1 |
| 300 bps | QPSK | 2 |
| 600+ bps | 8-PSK | 3 |

### 8-PSK Constellation

```
        90° (010)
          │
  135°────┼────45°
  (011)   │   (001)
          │
180°──────┼──────0° (000)
  (110)   │   (111)
          │
  225°────┼────315°
  (101)   │   (100)
          │
       270° (111)
```

Phase = tribit × 45°

---

## Forward Error Correction

### Convolutional Code
- Rate: 1/2
- Constraint length: K=7
- Generators: G1=0x6D (1101101), G2=0x4F (1001111)
- Tail bits: 6 zeros to flush encoder

### Coding Gain
~5 dB at BER = 10^-5

### 4800 bps Mode
- **Uncoded** (no FEC)
- Higher throughput, lower robustness

---

## Interleaving

Block interleaver for burst error protection.

### Structure
- Helical write (row by row with offset)
- Sequential read (column by column)

### Parameters
| Interleave | Rows | Columns | Bits | Duration |
|------------|------|---------|------|----------|
| SHORT | 40 | 576 | 23,040 | 4.8 sec |
| LONG | 40 | 4608 | 184,320 | 38.4 sec |

### Latency
- SHORT: ~4.8 seconds
- LONG: ~38.4 seconds

---

## Scrambling

### Purpose
- Spectral whitening
- Crypto compatibility
- Symbol timing recovery aid

### LFSR
- 12-bit shift register
- Polynomial: x^12 + x^11 + x^10 + x^9 + x^7 + x^4 + 1
- Initial state: 0x0BAD
- **Continuous** across entire transmission

### Application
```
scrambled_tribit = (gray_coded_tribit + scrambler_output) mod 8
```

---

## Frame Structure

### Data Frames (150-2400 bps)
```
[32 data symbols][16 probe symbols] = 48 symbols/frame
```
- Data: User information (scrambled)
- Probes: Known sequence for channel estimation

### Data Frames (4800 bps)
```
[20 data symbols][20 probe symbols] = 40 symbols/frame
```

### 75 bps (Walsh)
```
[32 Walsh-coded symbols] = 32 symbols/frame
```
No probes in 75 bps mode.

---

## Preamble

Three segments, each 480 symbols (0.2 seconds).

### Segment 1-2: Common Sync
- Same for all modes
- Enables synchronization before mode known
- 9 D-value blocks × 32 symbols × 2

### Segment 3: Mode ID
- D1, D2 values encode mode
- Receiver detects mode from this segment

### D-Value Encoding
| D1 | D2 | Data Rate |
|----|----|-----------| 
| 0 | 0 | 75 bps |
| 1 | 0 | 150 bps |
| 2 | 0 | 300 bps |
| 3 | 0 | 600 bps |
| 0 | 1 | 1200 bps |
| 1 | 1 | 2400 bps |
| 2 | 2 | 4800 bps |

D1, D2 also indicate interleave length.

### Preamble Pattern
Each D-value maps to 32-symbol pattern:
```cpp
const uint8_t psymbol[8][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},  // D=0
    {0, 4, 0, 4, 0, 4, 0, 4},  // D=1
    {0, 0, 4, 4, 0, 0, 4, 4},  // D=2
    {0, 4, 4, 0, 0, 4, 4, 0},  // D=3
    {0, 0, 0, 0, 4, 4, 4, 4},  // D=4
    {0, 4, 0, 4, 4, 0, 4, 0},  // D=5
    {0, 0, 4, 4, 4, 4, 0, 0},  // D=6
    {0, 4, 4, 0, 4, 0, 0, 4},  // D=7
};
```

---

## Probe Symbols

### Purpose
- Channel estimation
- Equalizer training
- SNR measurement

### Generation
```
probe_symbol = PSK8[scrambler_output]
```
Data contribution is zero (scrambler only).

### Known Sequence
Receiver knows probe positions and expected values, enabling:
- Least-squares channel estimation
- DFE training
- Phase/amplitude reference

---

## EOM (End of Message)

### Structure
4 "flush" frames after data:
- Data portion: all zeros (scrambled)
- Probe portion: normal scrambler

### Detection
- Count trailing zeros in decoded data
- Threshold: 40+ bytes indicates EOM

### Purpose
- Clean transmission boundary
- Flush interleaver/decoder state
- Signal to receiver that transmission is complete

---

## Bit/Byte Ordering

### Within Bytes
- **LSB first** transmission
- Bit 0 transmitted before Bit 7

### Tribit Formation
For 8-PSK (3 bits/symbol):
```
byte[0] bits 0,1,2 → tribit 0
byte[0] bits 3,4,5 → tribit 1
byte[0] bits 6,7 + byte[1] bit 0 → tribit 2
...
```

---

## Gray Coding

Maps binary tribits to constellation points to minimize bit errors.

### 8-PSK Gray Code (MGD3)
```
Input → Gray → Phase Index
  0   →   0  →    0
  1   →   1  →    1
  2   →   3  →    3
  3   →   2  →    2
  4   →   7  →    7
  5   →   6  →    6
  6   →   4  →    4
  7   →   5  →    5
```

Adjacent constellation points differ by 1 bit.

---

## 75 bps Mode (Walsh)

### Overview
- Uses Walsh-Hadamard coding
- 64 orthogonal Walsh functions
- Superior low-SNR performance

### Symbol Structure
- 32 Walsh chips per tribit
- MNS (Walsh 0) for sync
- MES pattern for diversity

### Implementation Status
This implementation has full 75 bps support:
- Walsh-Hadamard encoding/decoding
- Loopback verified
- Reference sample compatibility confirmed

---

## References

1. MIL-STD-188-110A, "Interoperability and Performance Standards for Data Modems"
2. STANAG 4539, NATO equivalent standard
3. CCIR Report 549, HF channel models

---

## Compatibility Notes

### MS-DMT Compatibility
This implementation verified compatible with MS-DMT:
- Scrambler polynomial and init
- FEC generator order (G1/G2 swapped from some docs)
- Gray code tables
- Interleaver parameters
- Bit ordering (LSB first)

### Known Differences
Some implementations may differ in:
- Preamble timing tolerances
- AGC settling time
- Equalizer algorithms (not standardized)
