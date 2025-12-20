# Tick-Chain Epoch Testing Instructions

## Quick Test (Without Hardware)

### Expected Console Output
After tick-chain epoch is established, you should see:

```
[EPOCH] Set from MARKER: offset=450.0ms confidence=0.700   (at startup)
[EPOCH] Set from CHAIN: offset=452.3ms confidence=0.950    (after 5+ ticks)
```

### Verification
1. Gate should NOT cycle into recovery mode
2. Tick intervals should stabilize to ~1000ms ±10ms
3. Console shows tick chain epoch messages after ~5-10 seconds

## Full Test with WWV Signal

### Prerequisites
- SDRplay RSP2 Pro connected
- WWV signal tuned (5/10/15/20 MHz)
- `sdr_server.exe` and `waterfall.exe` built

### Test Procedure

**Terminal 1: Start SDR Server**
```powershell
cd d:\claude_sandbox\phoenix_sdr
.\bin\sdr_server.exe -f 5.000450 -g 59 -l 0
```

**Terminal 2: Start Waterfall**
```powershell
cd d:\claude_sandbox\phoenix_sdr
.\bin\waterfall.exe --tcp localhost:4536
```

**Terminal 3: Start Telemetry Logger (Optional)**
```powershell
cd d:\claude_sandbox\phoenix_sdr
.\bin\telem_logger.exe
# Creates logs/ directory with telem_*.csv files
# Now captures CONS channel (console messages)
```

### Expected Behavior Timeline

**0-5 seconds (Startup):**
- Marker detector finds minute marker
- Console: `[EPOCH] Set from MARKER: offset=XXX.Xms confidence=0.700`
- Gate enabled from marker epoch

**5-15 seconds (Tick Chain Formation):**
- Tick detector receives 5+ consecutive ticks at ~1000ms intervals
- Tick correlator calculates std_dev from recent intervals
- If std_dev < 10ms (confidence > 0.8):
  - Console: `[EPOCH] Set from CHAIN: offset=XXX.Xms confidence=0.9XX`
  - Gate updated with tick-chain precision

**After 15 seconds (Steady State):**
- Tick intervals: ~1000ms ±5ms
- No gate recovery cycling
- Gate accepts ticks within 0-100ms window
- Epoch from tick chain, markers used for minute framing only

### Success Criteria
✅ Console shows `[EPOCH] Set from CHAIN` messages
✅ No `Gate recovery mode ENABLED` messages after tick chain established
✅ Tick intervals consistent: 998-1002ms (not 5985ms, 8084ms like before)
✅ Waterfall shows regular tick detections every second
✅ telem_logger captures CONS channel in `logs/telem_CONS_*.csv`

### Troubleshooting

**Issue:** No `[EPOCH] Set from CHAIN` messages
**Cause:** Weak signal, tick chain length < 5, or confidence < 0.8
**Fix:** Increase gain, improve antenna, wait longer for chain to establish

**Issue:** Still seeing gate recovery cycling
**Cause:** Tick chain not formed yet, still using marker epoch
**Fix:** Wait for 5+ consecutive ticks, verify signal strength

**Issue:** telem_logger not capturing CONS messages
**Cause:** Old telem_logger.exe without CONS channel
**Fix:** Rebuild with `.\build.ps1 -Target tools`

## Debug Output Channels

### Console (waterfall.exe)
- `[EPOCH]` - Epoch updates (MARKER or CHAIN)
- `[TICK]` - Tick detector events
- `[MARK]` - Marker detector events
- `[SYNC]` - Sync state changes

### UDP Telemetry (port 3005)
- `TICK,` - Tick events with timing
- `MARK,` - Marker events
- `CORR,` - Tick correlation chain stats
- `SYNC,` - Sync detector state
- `CONS,` - Console messages (captured by telem_logger)

### CSV Files (with --log-csv flag)
- `wwv_ticks.csv` - All tick detections
- `wwv_markers_corr.csv` - Marker correlation
- `wwv_tick_corr.csv` - Tick correlation chains
- `wwv_sync.csv` - Sync detector log

## Next Steps

After verifying tick-chain epoch works:

1. **Monitor gate performance:**
   - Should see zero gate recovery cycles
   - Tick acceptance rate should be 100% (except seconds 29, 59)

2. **Compare precision:**
   - Fast path (tick chain): ±5ms
   - Slow path (marker): ±50ms
   - Both should agree within ~50ms

3. **Extend telemetry (optional):**
   - Add epoch_offset_ms, std_dev_ms, confidence to `TELEM_CORR` CSV
   - Track epoch source transitions over time

4. **Consider tightening gate window:**
   - Current: 0-100ms (to handle ±50ms marker jitter)
   - With tick-chain: could reduce to 0-50ms (±5ms precision)
   - Better rejection of BCD 10th harmonic interference
