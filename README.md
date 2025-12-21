# ⚠️ ARCHIVED — Phoenix SDR (Monolith)

**This repository has been archived and split into focused, modular repositories.**

---

## Migration Guide

The phoenix_sdr monolith has been reorganized into 7 separate repositories for better maintainability and reusability:

| New Repository | Description | Link |
|----------------|-------------|------|
| **phoenix-sdr-core** | SDRplay RSP2 Pro hardware interface, decimation, TCP control | [View](https://github.com/Alex-Pennington/phoenix-sdr-core) |
| **phoenix-waterfall** | SDL2 waterfall display with integrated WWV detection | [View](https://github.com/Alex-Pennington/phoenix-waterfall) |
| **phoenix-wwv** | WWV detection library (tick, marker, BCD decoding) | [View](https://github.com/Alex-Pennington/phoenix-wwv) |
| **phoenix-sdr-net** | Network streaming (sdr_server, signal_relay, splitter) | [View](https://github.com/Alex-Pennington/phoenix-sdr-net) |
| **phoenix-sdr-utils** | Utilities (iqr_play, wwv_analyze, telem_logger, etc.) | [View](https://github.com/Alex-Pennington/phoenix-sdr-utils) |
| **phoenix-kiss-fft** | FFT library for signal processing | [View](https://github.com/Alex-Pennington/phoenix-kiss-fft) |
| **phoenix-reference-library** | Technical documentation (WWV specs, NTP driver36) | [View](https://github.com/Alex-Pennington/phoenix-reference-library) |

---

## File Mapping

| Original Location | New Repository |
|-------------------|----------------|
| `src/main.c`, `src/sdr_*.c`, `src/decimator.c`, `src/tcp_commands.c` | phoenix-sdr-core |
| `tools/waterfall*.c`, `tools/*_detector.c`, `tools/*_correlator.c` | phoenix-waterfall |
| `tools/tick_detector.c`, `tools/bcd_*.c`, `tools/sync_detector.c` | phoenix-wwv |
| `tools/sdr_server.c`, `tools/signal_relay.c`, `tools/signal_splitter.c` | phoenix-sdr-net |
| `tools/iqr_play.c`, `tools/wwv_*.c`, `tools/telem_logger.c` | phoenix-sdr-utils |
| `src/kiss_fft.c`, `include/kiss_fft.h` | phoenix-kiss-fft |
| `docs/*.md` (technical references) | phoenix-reference-library |

---

## Why the Split?

1. **Modularity** — Use only what you need (e.g., just the WWV detection library)
2. **Clearer dependencies** — Each repo has defined inputs/outputs
3. **Easier contribution** — Smaller, focused codebases
4. **Better CI/CD** — Independent build and test pipelines
5. **Reusability** — phoenix-wwv can be used without the waterfall display

---

## Suite Index

For the complete project overview, see the **mars-suite** index repository:

👉 **[github.com/Alex-Pennington/mars-suite](https://github.com/Alex-Pennington/mars-suite)**

---

## Historical Reference

This repository is preserved as a historical reference. The code here represents the state as of December 2025 before the modular split.

**Do not submit issues or pull requests to this repository.**  
All development continues in the new repositories listed above.

---

*Phoenix Nest MARS Communications Suite*  
*Alex Pennington (KY4OLB)*  
*Archived: December 21, 2025*
