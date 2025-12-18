# Documentation Alignment Roadmap

**Date:** December 18, 2025  
**Purpose:** Analyze gaps between deprecated design docs and current implementation, provide update roadmap

---

## Executive Summary

Good news: **Both protocols are substantially implemented** in the current codebase. The documents were marked "deprecated" incorrectly - they're actually accurate specifications that match the implementation. They need minor updates rather than major rewrites.

---

## 1. SDR_IQ_STREAMING_INTERFACE.md Analysis

### Implementation Status: ✅ ~95% Implemented

| Specification | Implementation | Source | Status |
|--------------|----------------|--------|--------|
| Port 4536 | `#define IQ_DEFAULT_PORT 4536` | [sdr_server.c#L45](tools/sdr_server.c#L45) | ✅ Match |
| MAGIC_PHXI (0x50485849) | `#define IQ_MAGIC_HEADER 0x50485849` | [sdr_server.c#L48](tools/sdr_server.c#L48) | ✅ Match |
| MAGIC_IQDQ (0x49514451) | `#define IQ_MAGIC_DATA 0x49514451` | [sdr_server.c#L49](tools/sdr_server.c#L49) | ✅ Match |
| MAGIC_META (0x4D455441) | `#define IQ_MAGIC_META 0x4D455441` | [sdr_server.c#L50](tools/sdr_server.c#L50) | ✅ Match |
| IQ_S16 format (code 1) | `#define IQ_FORMAT_S16 1` | [sdr_server.c#L53](tools/sdr_server.c#L53) | ✅ Match |
| IQ_F32 format (code 2) | `#define IQ_FORMAT_F32 2` | [sdr_server.c#L54](tools/sdr_server.c#L54) | ✅ Match |
| IQ_U8 format (code 3) | `#define IQ_FORMAT_U8 3` | [sdr_server.c#L55](tools/sdr_server.c#L55) | ✅ Match |
| Stream header (32 bytes) | `iq_stream_header_t` struct | [sdr_server.c#L58-L67](tools/sdr_server.c#L58-L67) | ⚠️ Minor diff |
| Data frame (16 byte header) | `iq_data_frame_t` struct | [sdr_server.c#L69-L75](tools/sdr_server.c#L69-L75) | ✅ Match |
| Metadata update | `iq_metadata_update_t` struct | [sdr_server.c#L77-L86](tools/sdr_server.c#L77-L86) | ⚠️ Minor diff |
| Ring buffer | `IQ_RING_BUFFER_SIZE (4 MB)` | [sdr_server.c#L46](tools/sdr_server.c#L46) | ✅ Match |
| Frame flags (OVERLOAD, FREQ_CHANGE, GAIN_CHANGE) | `IQ_FLAG_*` defines | [sdr_server.c#L89-L91](tools/sdr_server.c#L89-L91) | ✅ Match |
| Single client model | One g_iq_client_socket | [sdr_server.c#L102](tools/sdr_server.c#L102) | ✅ Match |

### Differences to Document

1. **Header struct has extra fields:** Implementation adds `gain_reduction` and `lna_state` fields
   - Doc spec: 32 bytes with 2 reserved fields
   - Implementation: Uses reserved space for gain/LNA info
   
2. **Metadata struct has extra fields:** Same as above
   - Doc spec: 32 bytes with 3 reserved fields  
   - Implementation: Uses reserved space for gain/LNA info

3. **Frame samples per frame:** Implementation uses 8192 samples
   - Doc doesn't specify, implementation uses `#define IQ_FRAME_SAMPLES 8192`

### Changes to Bring Doc into Alignment

```diff
struct iq_stream_header {
    uint32_t magic;           // 0x50485849 = "PHXI" (Phoenix IQ)
    uint32_t version;         // Protocol version (1)
    uint32_t sample_rate;     // Current sample rate in Hz
    uint32_t sample_format;   // Format code (see below)
    uint32_t center_freq_lo;  // Center frequency low 32 bits
    uint32_t center_freq_hi;  // Center frequency high 32 bits
-   uint32_t reserved[2];     // Future use (0)
+   uint32_t gain_reduction;  // IF gain reduction in dB
+   uint32_t lna_state;       // LNA state (0-8)
};  // Total: 32 bytes

struct iq_metadata_update {
    uint32_t magic;           // 0x4D455441 = "META"
    uint32_t sample_rate;     // New sample rate in Hz
    uint32_t sample_format;   // New format code
    uint32_t center_freq_lo;  // New center frequency low 32 bits
    uint32_t center_freq_hi;  // New center frequency high 32 bits
-   uint32_t reserved[3];     // Future use (0)
+   uint32_t gain_reduction;  // IF gain reduction in dB
+   uint32_t lna_state;       // LNA state (0-8)
+   uint32_t reserved;        // Future use (0)
};  // Total: 32 bytes
```

---

## 2. SDR_TCP_CONTROL_INTERFACE.md Analysis

### Implementation Status: ✅ ~90% Implemented

| Specification | Implementation | Source | Status |
|--------------|----------------|--------|--------|
| Port 4535 | `#define TCP_DEFAULT_PORT 4535` | [tcp_server.h#L25](include/tcp_server.h#L25) | ✅ Match |
| Text-based ASCII protocol | `char linebuf[TCP_MAX_LINE_LENGTH]` | [tcp_commands.c#L138](src/tcp_commands.c#L138) | ✅ Match |
| Line terminator `\n` | Parsed in tcp_commands.c | [tcp_commands.c](src/tcp_commands.c) | ✅ Match |
| Max line 256 bytes | `#define TCP_MAX_LINE_LENGTH 256` | [tcp_server.h#L26](include/tcp_server.h#L26) | ✅ Match |
| Single client | Enforced in sdr_server.c | [sdr_server.c](tools/sdr_server.c) | ✅ Match |
| **Frequency Commands** | | | |
| SET_FREQ | `CMD_SET_FREQ` | [tcp_commands.c#L25](src/tcp_commands.c#L25) | ✅ Match |
| GET_FREQ | `CMD_GET_FREQ` | [tcp_commands.c#L26](src/tcp_commands.c#L26) | ✅ Match |
| **Gain Commands** | | | |
| SET_GAIN | `CMD_SET_GAIN` | [tcp_commands.c#L29](src/tcp_commands.c#L29) | ✅ Match |
| GET_GAIN | `CMD_GET_GAIN` | [tcp_commands.c#L30](src/tcp_commands.c#L30) | ✅ Match |
| SET_LNA | `CMD_SET_LNA` | [tcp_commands.c#L31](src/tcp_commands.c#L31) | ✅ Match |
| GET_LNA | `CMD_GET_LNA` | [tcp_commands.c#L32](src/tcp_commands.c#L32) | ✅ Match |
| SET_AGC | `CMD_SET_AGC` | [tcp_commands.c#L33](src/tcp_commands.c#L33) | ✅ Match |
| GET_AGC | `CMD_GET_AGC` | [tcp_commands.c#L34](src/tcp_commands.c#L34) | ✅ Match |
| **Sample Rate/Bandwidth** | | | |
| SET_SRATE | `CMD_SET_SRATE` | [tcp_commands.c#L37](src/tcp_commands.c#L37) | ✅ Match |
| GET_SRATE | `CMD_GET_SRATE` | [tcp_commands.c#L38](src/tcp_commands.c#L38) | ✅ Match |
| SET_BW | `CMD_SET_BW` | [tcp_commands.c#L39](src/tcp_commands.c#L39) | ✅ Match |
| GET_BW | `CMD_GET_BW` | [tcp_commands.c#L40](src/tcp_commands.c#L40) | ✅ Match |
| **Hardware Commands** | | | |
| SET_ANTENNA | `CMD_SET_ANTENNA` | [tcp_commands.c#L43](src/tcp_commands.c#L43) | ✅ Match |
| GET_ANTENNA | `CMD_GET_ANTENNA` | [tcp_commands.c#L44](src/tcp_commands.c#L44) | ✅ Match |
| SET_BIAST | `CMD_SET_BIAST` | [tcp_commands.c#L45](src/tcp_commands.c#L45) | ✅ Match |
| SET_NOTCH | `CMD_SET_NOTCH` | [tcp_commands.c#L46](src/tcp_commands.c#L46) | ✅ Match |
| **Streaming Commands** | | | |
| START | `CMD_START` | [tcp_commands.c#L58](src/tcp_commands.c#L58) | ✅ Match |
| STOP | `CMD_STOP` | [tcp_commands.c#L59](src/tcp_commands.c#L59) | ✅ Match |
| STATUS | `CMD_STATUS` | [tcp_commands.c#L60](src/tcp_commands.c#L60) | ✅ Match |
| **Utility Commands** | | | |
| PING | `CMD_PING` | [tcp_commands.c#L63](src/tcp_commands.c#L63) | ✅ Match |
| VER | `CMD_VER` | [tcp_commands.c#L64](src/tcp_commands.c#L64) | ✅ Match |
| CAPS | `CMD_CAPS` | [tcp_commands.c#L65](src/tcp_commands.c#L65) | ✅ Match |
| HELP | `CMD_HELP` | [tcp_commands.c#L66](src/tcp_commands.c#L66) | ✅ Match |
| QUIT | `CMD_QUIT` | [tcp_commands.c#L67](src/tcp_commands.c#L67) | ✅ Match |
| **Error Codes** | | | |
| TCP_OK | `TCP_OK` | [tcp_server.h#L82](include/tcp_server.h#L82) | ✅ Match |
| SYNTAX | `TCP_ERR_SYNTAX` | [tcp_server.h#L83](include/tcp_server.h#L83) | ✅ Match |
| UNKNOWN | `TCP_ERR_UNKNOWN` | [tcp_server.h#L84](include/tcp_server.h#L84) | ✅ Match |
| PARAM | `TCP_ERR_PARAM` | [tcp_server.h#L85](include/tcp_server.h#L85) | ✅ Match |
| RANGE | `TCP_ERR_RANGE` | [tcp_server.h#L86](include/tcp_server.h#L86) | ✅ Match |
| STATE | `TCP_ERR_STATE` | [tcp_server.h#L87](include/tcp_server.h#L87) | ✅ Match |
| BUSY | `TCP_ERR_BUSY` | [tcp_server.h#L88](include/tcp_server.h#L88) | ✅ Match |
| HARDWARE | `TCP_ERR_HARDWARE` | [tcp_server.h#L89](include/tcp_server.h#L89) | ✅ Match |
| TIMEOUT | `TCP_ERR_TIMEOUT` | [tcp_server.h#L90](include/tcp_server.h#L90) | ✅ Match |

### Additional Commands in Implementation (Not in Doc)

These commands exist in implementation but aren't documented:

| Command | Source | Purpose |
|---------|--------|---------|
| SET_DECIM / GET_DECIM | [tcp_commands.c#L47-L48](src/tcp_commands.c#L47-L48) | Set/get decimation factor |
| SET_IFMODE / GET_IFMODE | [tcp_commands.c#L49-L50](src/tcp_commands.c#L49-L50) | Set/get IF mode (ZERO/LOW) |
| SET_DCOFFSET / GET_DCOFFSET | [tcp_commands.c#L51-L52](src/tcp_commands.c#L51-L52) | DC offset correction |
| SET_IQCORR / GET_IQCORR | [tcp_commands.c#L53-L54](src/tcp_commands.c#L53-L54) | IQ imbalance correction |
| SET_AGC_SETPOINT / GET_AGC_SETPOINT | [tcp_commands.c#L55-L56](src/tcp_commands.c#L55-L56) | AGC setpoint in dBFS |

### Changes to Bring Doc into Alignment

Add documentation for the additional commands above in Section 5 of the document.

---

## 3. Roadmap

### Phase 1: Move Documents Back from Deprecated ✅ COMPLETE

**Effort:** 5 minutes

1. ✅ Moved both files from `docs/deprecated/` back to `docs/`
2. ✅ Updated status from "DRAFT" to "IMPLEMENTED"

### Phase 2: Update SDR_IQ_STREAMING_INTERFACE.md ✅ COMPLETE

**Effort:** 30 minutes

| Task | Description | Status |
|------|-------------|--------|
| 2.1 | Updated header struct to include `gain_reduction` and `lna_state` fields | ✅ |
| 2.2 | Updated metadata struct similarly | ✅ |
| 2.3 | Added `IQ_FRAME_SAMPLES = 8192` constant documentation | ✅ |
| 2.4 | Updated status to "IMPLEMENTED" | ✅ |
| 2.5 | Added implementation reference section pointing to source files | ✅ |

### Phase 3: Update SDR_TCP_CONTROL_INTERFACE.md ✅ COMPLETE

**Effort:** 45 minutes

| Task | Description | Status |
|------|-------------|--------|
| 3.1 | Added Section 5.5 entries for SET_DECIM/GET_DECIM | ✅ |
| 3.2 | Added Section 5.5 entries for SET_IFMODE/GET_IFMODE | ✅ |
| 3.3 | Added Section 5.5 entries for SET_DCOFFSET/GET_DCOFFSET | ✅ |
| 3.4 | Added Section 5.5 entries for SET_IQCORR/GET_IQCORR | ✅ |
| 3.5 | Added Section 5.5 entries for SET_AGC_SETPOINT/GET_AGC_SETPOINT | ✅ |
| 3.6 | Updated status to "IMPLEMENTED" | ✅ |
| 3.7 | Updated async notification documentation (! GAIN_CHANGE format) | ✅ |
| 3.8 | Added implementation reference section pointing to source files | ✅ |
| 3.9 | Updated HELP, CAPS, and STATUS output examples | ✅ |

### Phase 4: Validate Client (waterfall.c) Alignment (Optional)

**Effort:** 1 hour

| Task | Description | Status |
|------|-------------|--------|
| 4.1 | Verify waterfall.c client matches server protocol | ⬜ Not Started |
| 4.2 | Document any client-specific behaviors | ⬜ Not Started |
| 4.3 | Add example usage section to docs | ⬜ Not Started |

---

## 4. Recommendation

**Action:** Execute Phase 1 immediately - move documents back to `docs/` folder. They are valid specifications that match the implementation.

The documents were incorrectly marked as deprecated. The implementation in `sdr_server.c` and `tcp_commands.c` closely follows these specifications.

---

## 5. Source File Quick Reference

| Component | File | Lines |
|-----------|------|-------|
| I/Q protocol structs | [tools/sdr_server.c](tools/sdr_server.c) | 45-91 |
| I/Q ring buffer | [tools/sdr_server.c](tools/sdr_server.c) | 241-322 |
| I/Q streaming thread | [tools/sdr_server.c](tools/sdr_server.c) | 356-549 |
| TCP command definitions | [include/tcp_server.h](include/tcp_server.h) | 32-76 |
| TCP error codes | [include/tcp_server.h](include/tcp_server.h) | 82-91 |
| TCP state structure | [include/tcp_server.h](include/tcp_server.h) | 145-185 |
| Command parser | [src/tcp_commands.c](src/tcp_commands.c) | 24-70 |
| Client implementation | [tools/waterfall.c](tools/waterfall.c) | 80-130 |
