# WWV Time Signal Generators and Simulators: Complete Implementation Landscape

The open-source community has developed **14+ significant WWV implementations**, though no official NIST reference code exists. The most complete generators are **jj1bdx/WWV** (C) and **kuremu/wwv_simulator** (Python), both implementing the full 100 Hz BCD subcarrier with UT1 corrections. Notably, no dedicated GNU Radio WWV signal generator has been created, representing a gap in the SDR ecosystem.

## GitHub repositories lead WWV implementation efforts

The most active development occurs on GitHub, where multiple projects implement WWV signal generation across different languages and platforms. The **jj1bdx/WWV** repository provides the most feature-complete C implementation, generating 16-bit PCM mono audio at 16 kHz with full format support including 100 Hz timecode, BCD encoding with position markers, UT1 corrections, and leap second insertion. This project uses pre-generated speech audio from kalafut's WWV simulator and supports both WWV and WWVH stations with proper voice differences.

For Python developers, **kuremu/wwv_simulator** offers comprehensive simulation capabilities including historical DUT1 data downloaded from IERS (3.5 MB spanning 1973 to present), geophysical alerts from NOAA, and ionospheric experiment simulation. The project implements second pulses as 5ms bursts of 1200 Hz and includes a **wwv_decoder.py** module for validation testing. Dependencies include SoX for audio generation and espeak for voice synthesis.

The most popular project by stars is **kalafut/wwv** (76 stars), a JavaScript implementation powering the live web demo at wwv.mcodes.org. This browser-based simulator uses webpack/NodeJS and includes bundled voice clips for announcements, making it accessible without local installation.

| Repository | Language | Stars | Key Features |
|------------|----------|-------|--------------|
| kalafut/wwv | JavaScript | 76 | Web-based, live demo |
| jj1bdx/WWV | C | 7 | Most complete, PCM output |
| kuremu/wwv_simulator | Python | 5 | IERS data, decoder included |
| vsergeev/radio-decoders | Python/Julia | 12 | Multi-protocol decoders |
| rgrokett/RaspiWWV | Python | 3 | Raspberry Pi with RTC |

## NIST provides specifications but no reference implementation

NIST publishes comprehensive documentation but has not released any official source code, reference implementations, or test signal generators for WWV/WWVH. The primary specification is **NIST Special Publication 432** (2002 Edition), with Chapter 3 (pages 29-55) covering WWV/WWVH in detail including the complete time code format, BCD weighting scheme, and all 60 bits annotated in Table 3.13.

For deeper technical details, **NIST Special Publication 250-67** ("NIST Time and Frequency Radio Stations: WWV, WWVH, and WWVB") spans 160+ pages covering the 100 Hz time code, UT1 corrections, modulation levels, spectrum allocation, and signal monitoring. The legal specification appears in **15 CFR § 200.107**, which defines WWV's "modified IRIG H time code" on the 100 Hz subcarrier with frequency accuracy of ±2 parts in 10^11.

## The 100 Hz BCD subcarrier encoding format

WWV's time code uses a modified IRIG-H format on a **100 Hz subcarrier** with pulse-width modulation. The encoding uses three distinct pulse durations: **200ms for binary 0**, **500ms for binary 1**, and **800ms for position markers**. Unlike WWVB which transmits MSB first with 8-4-2-1 weighting, WWV transmits **LSB first with 1-2-4-8 weighting**.

Each second begins with a 30ms protected zone for the audio tick, after which the 100 Hz subcarrier encodes one bit. The first 30ms remains silent (no subcarrier), then the high-amplitude portion extends for the duration corresponding to the bit value. Position markers P1-P5 occur at seconds 9, 19, 29, 39, and 49, with P0 at second 59. Two consecutive markers signal the start of a new minute frame.

**Complete bit assignments per minute:**
- Seconds 5-8, 10-12: Minutes (units: 1-2-4-8, tens: 10-20-40)
- Seconds 15-18, 20-21: Hours (units: 1-2-4-8, tens: 10-20)
- Seconds 25-28, 30-33, 35-36: Day of year (units, tens, hundreds)
- Seconds 45-48, 53-54, 56-57: Year (units and tens)
- Second 2: DST status at 00:00Z today
- Second 3: Leap second warning
- Second 55: DST status at 24:00Z today
- Seconds 50-52: DUT1 magnitude (0.1, 0.2, 0.4 second weights)
- Seconds 37-38, 43: DUT1 sign indicators

## UT1 correction encoding uses dual methods

WWV encodes DUT1 corrections both audibly and in the BCD time code. The **double tick method** places additional 5ms ticks during seconds 1-16: doubled ticks in seconds 1-8 indicate positive DUT1, while doubled ticks in seconds 9-16 indicate negative DUT1. The number of doubled ticks multiplied by 0.1 seconds gives the magnitude, supporting corrections up to ±0.7 seconds.

The BCD method encodes the DUT1 sign in dedicated bits (seconds 37-38 for positive, second 43 for negative) and magnitude in seconds 50-52 using 0.1, 0.2, and 0.4 second weights. The **kuremu/wwv_simulator** project automatically downloads historical DUT1 values from IERS, maintaining 3.5 MB of correction data spanning back to 1973.

## SDR implementations focus on decoding, not generation

A significant gap exists in the SDR/GNU Radio ecosystem: **no dedicated GNU Radio WWV HF signal generator module exists**. Available SDR implementations focus primarily on decoding:

- **KE7KUS/WWV-Decoder**: GNU Radio flowgraph with Python processing for 100 Hz BCD decoding, compatible with RTL-SDR
- **tmiw/wwv_ntp_source**: C++ application using rtl_fm piped input, outputs to NTP/Chrony
- **SDR Adventure flowgraph**: Experimental GNU Radio decoder using bandpass filtering and threshold detection

The closest complete transmitter implementation is the **MathWorks Simulink WWV Digital Receiver** example, which includes both transmitter and receiver models operating at 8000 samples/sec. This implements full BCD encoding with symbol types MISS, ZERO (170ms), ONE (470ms), and MARK (770ms).

For related time signal generation, **hzeller/txtempus** (482 stars) generates WWVB/DCF77/MSF/JJY signals using Raspberry Pi GPIO, demonstrating the architecture for time code transmission. The **gr-dcf77-transmitter** GNU Radio module provides a template for time signal generation that could be adapted for WWV's HF frequencies.

## Standard audio tones follow station-specific schedules

WWV and WWVH broadcast **500 Hz** and **600 Hz** tones on alternating minutes, with **440 Hz** (A4 reference pitch) broadcast once per hour: minute 2 on WWV, minute 1 on WWVH. Second pulses use **1000 Hz** (WWV) or **1200 Hz** (WWVH) for 5ms duration, while minute markers extend to 800ms. Hour markers use **1500 Hz** for both stations.

Modulation levels specify: steady tones at 50%, BCD time code at 50%, second pulses at 100%, and voice announcements at 75%. The 100 Hz subcarrier operates at -15 dBc (18% modulation) for high amplitude and -30 dBc (3% modulation) for low amplitude.

## NTP Driver 36 provides decoding reference

The **NTP Radio WWV/H Audio Demodulator/Decoder** (Driver 36) represents the most mature decoding implementation, processing 8-kHz µ-law codec samples with maximum-likelihood techniques. Originally based on a TAPR DSP93 machine language program, it achieves better than 1ms timing accuracy and can decode 9 BCD digits plus 7 miscellaneous bits per minute. Technical documentation remains available at ntp.org/documentation/drivers/driver36/.

## Conclusion: Implementation gaps and opportunities

The WWV implementation landscape reveals mature decoder technology but limited signal generation capability. Key resources for developers include:

- **For complete simulation**: jj1bdx/WWV (C) or kuremu/wwv_simulator (Python)
- **For BCD specification**: NIST SP 432 Chapter 3 and SP 250-67
- **For web demonstration**: wwv.mcodes.org (kalafut/wwv)
- **For NTP integration**: tmiw/wwv_ntp_source or NTP Driver 36

The absence of a GNU Radio WWV signal generator represents the most significant gap. Creating one would require adapting gr-dcf77-transmitter's architecture for HF frequencies, implementing BCD encoding per NIST SP 432, and adding 100 Hz subcarrier modulation with proper PWM timing. Hardware options include HackRF One (1 MHz - 6 GHz native) or USRP with appropriate daughterboard for WWV's 2.5-20 MHz broadcast frequencies.