# Telemetry Logger Tool

The `telem_logger.exe` tool listens for UDP telemetry broadcasts from `waterfall.exe` and writes them to CSV files organized by channel. On Windows, it runs in the system tray for convenient background operation.

## Quick Start

```powershell
# Start the logger (runs in system tray)
.\bin\telem_logger.exe -o logs/

# Start waterfall (telemetry enabled by default)
cmd /c ".\bin\simple_am_receiver.exe -f 5.000450 -g 59 -l 0 -o | .\bin\waterfall.exe"
```

Right-click the system tray icon to:
- View message count and channel statistics
- Pause/Resume logging
- Open the logs folder
- Exit the logger

## Usage

```
telem_logger.exe [options]

Options:
  -p <port>       UDP port to listen on (default: 3005)
  -o <dir>        Output directory for CSV files (default: current directory)
  -c <channels>   Comma-separated list of channels to log (default: all)
  -v              Verbose mode (print messages to console)
  --no-tray       Disable system tray icon (console only mode)
  -h              Show help
```

## Examples

```powershell
# Log all channels (runs in system tray)
.\bin\telem_logger.exe

# Log to logs/ folder with console output
.\bin\telem_logger.exe -o logs/ -v

# Log only tick, marker, and sync channels
.\bin\telem_logger.exe -c TICK,MARK,SYNC

# Log carrier and tone trackers to custom directory
.\bin\telem_logger.exe -c CARR,T500,T600 -o frequency_logs/

# Console-only mode (no system tray)
.\bin\telem_logger.exe --no-tray -v

# Custom port (if waterfall configured differently)
.\bin\telem_logger.exe -p 3010
```

## System Tray Features

When running in system tray mode (default on Windows):

| Feature | Description |
|---------|-------------|
| **Tooltip** | Shows message count and active channels |
| **Pause/Resume** | Temporarily stop logging without closing files |
| **Open Logs Folder** | Opens Explorer to the output directory |
| **Exit** | Cleanly shuts down and writes file footers |

To disable the system tray and run as a console application:
```powershell
.\bin\telem_logger.exe --no-tray
```

## Output Files

The logger creates one CSV file per channel, named:

```
telem_<CHANNEL>_YYYYMMDD_HHMMSS.csv
```

For example:
- `telem_CHAN_20251219_143215.csv` - Channel quality metrics
- `telem_TICK_20251219_143215.csv` - Tick pulse detections
- `telem_MARK_20251219_143215.csv` - Minute marker detections
- `telem_SYNC_20251219_143215.csv` - Synchronization state

Each file includes:
- Header comments with start time and source port
- Raw CSV data exactly as received from waterfall
- Footer comments with end time and message count

## Available Channels

| Channel | Description | Typical Rate |
|---------|-------------|--------------|
| `CHAN` | Channel quality (carrier, SNR, noise) | ~1/sec |
| `TICK` | Tick pulse events | 1/sec (when detected) |
| `MARK` | Minute marker events | 1/min (when detected) |
| `CARR` | Carrier frequency tracking | ~1/sec |
| `SYNC` | Sync state machine updates | On state change |
| `SUBC` | Subcarrier (500/600 Hz) analysis | ~1/sec |
| `T500` | 500 Hz tone tracker | ~1/sec |
| `T600` | 600 Hz tone tracker | ~1/sec |
| `BCDS` | BCD symbol decoder | On symbol decode |
| `SYM` | BCD symbol events | On symbol decode |
| `STATE` | Sync state transitions | On state change |
| `STATUS` | BCD decoder status | ~1/sec |

See [UDP_TELEMETRY_OUTPUT_PROTOCOL.md](UDP_TELEMETRY_OUTPUT_PROTOCOL.md) for detailed field descriptions.

## CSV Format

Each channel's CSV contains messages in the format documented in the telemetry protocol. Example `CHAN` output:

```csv
# Phoenix SDR Telemetry Log - Channel: CHAN
# Started: Thu Dec 19 14:32:15 2024
# Source: UDP port 3005
CHAN,14:32:15,85320.0,-45.2,18.3,-52.1,-58.4,-38.5,-56.8,GOOD
CHAN,14:32:16,86320.0,-44.8,19.1,-51.8,-57.9,-37.9,-57.0,GOOD
CHAN,14:32:17,87320.0,-45.0,18.7,-52.0,-58.2,-38.2,-56.9,GOOD
# Ended: Thu Dec 19 14:35:42 2024
# Messages logged: 207
```

## Graceful Shutdown

**From system tray:** Right-click the icon and select "Exit Telemetry Logger"

**From console (--no-tray mode):** Press `Ctrl+C`

On shutdown, the logger will:
1. Close all CSV files with footer comments
2. Print a summary of messages logged per channel
3. Exit cleanly

## Typical Workflow

### Background Logging (Recommended)

```powershell
# Create logs directory
New-Item -ItemType Directory -Path logs -Force

# Start logger (runs in system tray)
Start-Process -FilePath .\bin\telem_logger.exe -ArgumentList "-o logs/"

# Start waterfall
cmd /c ".\bin\simple_am_receiver.exe -f 5.000450 -g 59 -l 0 -o | .\bin\waterfall.exe"

# Logger runs in background - right-click tray icon to manage/exit
```

### Analyzing Sync Performance

```powershell
# Log only sync-related channels
.\bin\telem_logger.exe -c SYNC,MARK,TICK -o sync_analysis/ -v
```

### Frequency Stability Analysis

```powershell
# Log carrier and tone trackers
.\bin\telem_logger.exe -c CARR,T500,T600 -o freq_logs/ -v
```

## Building

The tool is built automatically with the main build:

```powershell
.\build.ps1
```

Or see [build.ps1](../build.ps1) for build configuration.

## Source

- [tools/telem_logger.c](../tools/telem_logger.c)
