# WWV Minute Marker Detector - Design Document

## Status: ON HOLD
**Blocked by:** Waterfall resolution needs to match SDRuno quality first

---

## Observations from SDRuno Waterfall

### SDRuno Display Quality
- Individual 5ms ticks clearly visible as distinct horizontal bright spots
- Fine time resolution - each tick is a sharp line, not a smear
- Frequency axis shows ±3000 Hz with clear detail
- Minute markers appear as long vertical stripes at ±1000Hz positions

### Our Current Waterfall
- 1024-point FFT at 48kHz decimated rate
- 46.9 Hz/bin frequency resolution
- 21.3ms frame time
- No overlap between frames
- Ticks appear smeared compared to SDRuno

### Gap Analysis
| Parameter | SDRuno (estimated) | Ours | Impact |
|-----------|-------------------|------|--------|
| FFT Size | 4096-8192? | 1024 | Coarser frequency resolution |
| Overlap | 50-75%? | 0% | Choppy time display |
| Update Rate | Higher | ~47 fps | Less smooth |

---

## Minute Marker Characteristics

### Signal Properties (from WWV spec + waterfall observation)
- **Duration:** 800ms (vs 5ms for regular ticks)
- **Primary frequency:** 1000 Hz tone
- **Bandwidth:** Wider than regular ticks - spreads across harmonics
- **Additional components:** Visible energy at other frequencies (circled in waterfall)
  - Possible locations: ~500Hz, ~1500Hz, ~2000Hz (need confirmation)
- **Total energy:** Much higher than regular ticks due to 160x duration

### Why Current Tick Detector Misses Markers
1. `TICK_MAX_DURATION_MS = 50ms` - pulses >50ms are REJECTED
2. Current matched filter template is 5ms (240 samples) - optimized for ticks
3. Looking at narrow bandwidth around 1000Hz only

---

## Proposed Marker Detector Design

### Follow Existing Pattern
Create `marker_detector.h` and `marker_detector.c` following `tick_detector` pattern:
- Own FFT configuration (or shared)
- Own sample buffer
- Own state machine
- Own CSV logging
- Reports to main waterfall.c

### Detection Strategy

#### Frequency Domain
- **Center frequency:** 1000 Hz
- **Bandwidth:** ±200-300 Hz (wider than tick detector's ±100 Hz)
- **Additional buckets (future):** Add more frequency bins as we identify them

#### Time Domain - Sliding Window Accumulator
```
Option A - Sliding Window:
- Circular buffer of last ~1000ms of bucket energy values
- ~190 frames at 5.3ms per frame (if using 256-pt FFT)
- Every frame: compute sum of buffer
- When sum exceeds threshold → marker detected
- Pro: Catches marker regardless of when it starts
- Con: More memory, continuous computation
```

#### Detection Logic
```c
// Pseudocode
float energy_buffer[WINDOW_FRAMES];  // ~190 frames for 1 second
int buffer_idx = 0;
float accumulated_energy = 0;

// Each frame:
accumulated_energy -= energy_buffer[buffer_idx];  // Remove oldest
energy_buffer[buffer_idx] = current_1000hz_energy;
accumulated_energy += current_1000hz_energy;      // Add newest
buffer_idx = (buffer_idx + 1) % WINDOW_FRAMES;

if (accumulated_energy > MARKER_THRESHOLD) {
    // Marker detected!
}
```

### Output Format
Same CSV as tick detector, marker entries distinguished by:
- `tick_num` field: "M1", "M2", etc.
- `wwv_sec` field: Should be 0 if detection is working correctly (debug verification only)
- `expected` field: Should be "MARKER" (debug verification only)

---

## Next Steps

### Before Implementing Marker Detector
1. **Match SDRuno waterfall quality**
   - Investigate FFT size increase (2048? 4096?)
   - Implement frame overlap (50%?)
   - Verify individual ticks are sharp and distinct
   
2. **Study minute marker in improved waterfall**
   - Confirm exact frequencies of additional components
   - Measure actual bandwidth spread
   - Verify 800ms duration is visible as expected

### Implementation Order
1. Improve waterfall resolution
2. Create `marker_detector.h` with configuration constants
3. Create `marker_detector.c` with sliding window accumulator
4. Add to build.ps1
5. Integrate into waterfall.c sample loop
6. Test and tune thresholds
7. Add additional frequency buckets as identified

---

## Open Questions

1. What FFT size does SDRuno use for that display quality?
2. What overlap percentage gives best time resolution vs CPU cost?
3. Exact frequencies of the additional marker components (±500Hz? ±1500Hz?)
4. Should marker detector share FFT with tick detector or have dedicated FFT?

---

## References

- WWV Spec: https://www.nist.gov/pml/time-and-frequency-division/time-distribution/radio-station-wwv/wwv-and-wwvh-digital-time-code
- Second 0 each minute: 800ms pulse @ 1000Hz
- Second 0 each hour: 800ms pulse @ 1500Hz (hour marker - different frequency)
- WWVH uses 1200Hz instead of 1000Hz

---

*Document created: 2025-12-16*
*Last updated: 2025-12-16*
