# Addendum: MS-DMT Modem Core Compliance

**Reference:** Phoenix SDR Timing Requirements Document  
**Subject:** MIL-188-110A Modem Core Implementation

## The De Facto Standard

The modem core used in MS-DMT (and available for Phoenix Nest integration) was developed by **Charles Brain (G4GUO)** and later maintained and extended by **Steve Hajducek (N2CKH)**. This implementation has become the de facto standard for MIL-188-110A in the amateur and MARS communities.

## Compliance Summary

Direct inspection of the source code (`Cm110s.h`, `txm110a.cpp`, `rxm110a.cpp`) confirms the implementation matches MIL-188-110A requirements:

| Requirement | MIL-188-110A Spec | MS-DMT Implementation | Status |
|-------------|-------------------|----------------------|--------|
| Symbol Rate | 2400 baud | 9600 Hz ÷ 4 samples = 2400 baud | ✓ |
| Sample Rate | 9600 Hz | `M1_SAMPLE_RATE = 9600` | ✓ |
| Center Frequency | 1800 Hz | `M1_CENTER_FREQUENCY = 1800` | ✓ |
| Modulation | PSK (BPSK/QPSK/8PSK) | All three implemented | ✓ |
| FEC | Rate 1/2, K=7 Convolutional | Viterbi encoder/decoder present | ✓ |
| Interleaver | Block interleaver | 40×576 matrix in `Interleaver` struct | ✓ |

## Why It Works

The implementation correctly follows the MIL-188-110A signal chain:

**Transmit:** The `tx_symbol()` function generates exactly 4 audio samples per symbol through an FIR filter, producing the 2400 baud waveform at 9600 Hz sample rate. The Viterbi encoder and interleaver in `ptx110a.cpp` provide the required error protection.

**Receive:** The `rx_process_block()` function in `rxm110a.cpp` processes 1920-sample blocks (200 ms), performing carrier recovery, AGC, equalization, and preamble detection. The adaptive equalizer handles the multipath distortion characteristic of HF channels.

## Relevance to Phoenix SDR

The Phoenix SDR waterfall's 0.5 ms timing resolution (HOP_SIZE = 24) exceeds what the modem core requires. This ensures that when we integrate the modem core with the SDR front-end, the timing granularity will not be a limiting factor.

The SDR provides the RF-to-baseband conversion at 48 kHz; the modem core expects 9600 Hz audio. A simple 5:1 decimation bridges the two, preserving all timing information needed for symbol recovery.

---

*Charles Brain's original work on PC-ALE and the 110A core, combined with Steve Hajducek's continued development through MS-DMT, gives us a proven, interoperable foundation to build upon.*
