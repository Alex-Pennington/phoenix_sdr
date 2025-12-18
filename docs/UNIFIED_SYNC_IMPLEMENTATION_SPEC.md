# Unified Sync Architecture - Implementation Specification v1.0

**Document:** Phoenix SDR Unified Sync Implementation Spec
**Version:** 1.0
**Date:** 2024-12-18
**Status:** APPROVED FOR IMPLEMENTATION
**Owner:** Modem Team (phoenix_sdr)

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           MODEM (phoenix_sdr)                               │
│                                                                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐│
│  │tick_detector│  │marker_det.  │  │bcd_time_det │  │bcd_freq_det         ││
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────────────┘│
│         │                │                │                │               │
│         │ ticks          │ markers        │ 800ms pulses   │               │
│         │                │                │                │               │
│         ▼                ▼                ▼                ▼               │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                        sync_detector (ENHANCED)                      │   │
│  │  ┌────────────────┐  ┌────────────────┐  ┌────────────────────────┐ │   │
│  │  │ tick_gap_track │  │ recovery_state │  │ evidence_accumulator   │ │   │
│  │  │ - consecutive  │  │ - retained_anc │  │ - mask                 │ │   │
│  │  │ - hole detect  │  │ - validation   │  │ - confidence           │ │   │
│  │  └────────────────┘  └────────────────┘  └────────────────────────┘ │   │
│  │                              │                                       │   │
│  │              ┌───────────────┴───────────────┐                       │   │
│  │              ▼                               ▼                       │   │
│  │     frame_time_t                     wwv_clock (optional)            │   │
│  │     - current_second                 - relative/absolute mode        │   │
│  │     - confidence                     - schedule awareness            │   │
│  │     - evidence_mask                                                  │   │
│  └──────────────────────────────────────────────┬──────────────────────┘   │
│                                                 │                          │
│                                                 ▼                          │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                        bcd_correlator                                 │  │
│  │  - Consumes frame_time_t (not raw anchor)                            │  │
│  │  - Position gating for P-markers                                     │  │
│  │  - Outputs: SYM,<char>,<second>,<duration>                           │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                 │                          │
└─────────────────────────────────────────────────┼──────────────────────────┘
                                                  │ UDP
                                                  ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                      CONTROLLER (phoenix_sdr_controller)                    │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                        bcd_decoder                                    │  │
│  │  - Receives symbols with resolved frame position                     │  │
│  │  - Assembles 60-symbol frames                                        │  │
│  │  - Decodes BCD → time                                                │  │
│  │  - Feeds absolute time back to wwv_clock (optional)                  │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. State Machine

```
                         ┌──────────────────────────────────┐
                         │                                  │
        ┌────────────────▼────────────────┐                 │
        │        SYNC_ACQUIRING           │                 │
        │   confidence = 0.0              │                 │
        │   No retained state             │                 │
        └────────────────┬────────────────┘                 │
                         │                                  │
                         │ tick + marker correlated         │
                         │ OR double tick-hole pattern      │
                         ▼                                  │
        ┌─────────────────────────────────┐                 │
        │        SYNC_TENTATIVE           │                 │
        │   confidence = 0.3 initial      │                 │
        │   Building evidence             │                 │
        └────────────────┬────────────────┘                 │
                         │                                  │
                         │ confidence > 0.7                 │
                         ▼                                  │
        ┌─────────────────────────────────┐                 │
        │         SYNC_LOCKED             │◄────────────┐   │
        │   High confidence tracking      │             │   │
        │   Exporting frame_time_t        │             │   │
        └────────────────┬────────────────┘             │   │
                         │                              │   │
                         │ signal_weak_count >= 3       │   │
                         │ (debounced)                  │   │
                         ▼                              │   │
        ┌─────────────────────────────────┐             │   │
        │       SYNC_RECOVERING           │             │   │
        │   Retain anchor, decay conf.    │             │   │
        │   Validate tick+marker+P        │─────────────┘   │
        └────────────────┬────────────────┘  validated      │
                         │                                  │
                         │ timeout OR conf < 0.05           │
                         │ OR validation failed             │
                         │ OR gap > 120s                    │
                         └──────────────────────────────────┘
                                    full reset

        ┌─────────────────────────────────┐
        │   Alternative: partial fail     │
        │   (tick OK, marker mismatch)    │
        │   → SYNC_TENTATIVE              │
        │   (preserves some state)        │
        └─────────────────────────────────┘
```

---

## 3. Configuration Constants

```c
/*============================================================================
 * Timing Thresholds
 *============================================================================*/
#define TICK_INTERVAL_MIN_MS         950.0f     /* Normal tick spacing min */
#define TICK_INTERVAL_MAX_MS         1050.0f    /* Normal tick spacing max */
#define TICK_HOLE_MIN_GAP_MS         1700.0f    /* Missing tick threshold */
#define TICK_HOLE_MAX_GAP_MS         2200.0f    /* Beyond = signal loss */
#define TICK_DOUBLE_MAX_GAP_MS       100.0f     /* DUT1 double-tick spacing */

#define SYNC_SIGNAL_LOSS_GAP_MS      2500.0f    /* Transition to RECOVERING */
#define SYNC_RETENTION_WINDOW_MS     120000.0f  /* 2 minutes max retention */
#define SYNC_RECOVERY_TIMEOUT_MS     10000.0f   /* 10s to validate or reset */

/*============================================================================
 * Evidence & Confidence
 *============================================================================*/
#define EVIDENCE_TICK                (1 << 0)
#define EVIDENCE_MARKER              (1 << 1)
#define EVIDENCE_P_MARKER            (1 << 2)
#define EVIDENCE_TICK_HOLE           (1 << 3)

/* Base weights (reduced 50% during special minutes if wwv_clock available) */
#define WEIGHT_TICK                  0.05f
#define WEIGHT_MARKER                0.40f
#define WEIGHT_P_MARKER              0.15f
#define WEIGHT_TICK_HOLE             0.20f
#define WEIGHT_COMBINED_HOLE_MARKER  0.50f      /* :59 hole + marker */

#define CONFIDENCE_LOCKED_THRESHOLD  0.70f
#define CONFIDENCE_MIN_RETAIN        0.05f      /* Below = full reset */
#define CONFIDENCE_TENTATIVE_INIT    0.30f

#define CONFIDENCE_DECAY_NORMAL      0.995f     /* Per 100ms */
#define CONFIDENCE_DECAY_RECOVERING  0.980f     /* Faster during recovery */

/*============================================================================
 * Validation Tolerances
 *============================================================================*/
#define TICK_PHASE_TOLERANCE_MS      100.0f     /* Tick vs anchor alignment */
#define MARKER_TOLERANCE_MS          500.0f     /* Marker vs anchor */
#define P_MARKER_TOLERANCE_MS        200.0f     /* P-marker position */
#define LEAP_SECOND_EXTRA_MS         1000.0f    /* Added when pending */

/*============================================================================
 * Debounce & Filtering
 *============================================================================*/
#define MIN_TICKS_FOR_HOLE           20         /* Before trusting hole */
#define SIGNAL_WEAK_DEBOUNCE         3          /* Checks before RECOVERING */
#define MIN_CONSECUTIVE_LOW_FRAMES   3          /* Phase 9: pulse end debounce */

/*============================================================================
 * BCD Symbol Thresholds
 *============================================================================*/
#define BCD_SYMBOL_ZERO_MAX_MS       350.0f
#define BCD_SYMBOL_ONE_MAX_MS        650.0f
#define BCD_SYMBOL_MARKER_MAX_MS     900.0f

/* Valid P-marker positions */
static const int VALID_P_POSITIONS[] = {0, 9, 19, 29, 39, 49, 59};
#define NUM_VALID_P_POSITIONS        7
```

---

## 4. Data Structures

```c
/*============================================================================
 * Exported Types
 *============================================================================*/

/* Frame time - exported to consumers */
typedef struct {
    int current_second;             /* 0-59, authoritative */
    float second_start_ms;          /* When this second began */
    float confidence;               /* 0.0 - 1.0 */
    uint32_t evidence_mask;         /* Which signals contributed */
    sync_state_t state;             /* Current sync state */
} frame_time_t;

/* Sync state enumeration */
typedef enum {
    SYNC_ACQUIRING,
    SYNC_TENTATIVE,
    SYNC_LOCKED,
    SYNC_RECOVERING
} sync_state_t;

/*============================================================================
 * Internal Structures (sync_detector)
 *============================================================================*/

typedef struct {
    int consecutive_tick_count;
    float last_tick_ms;
    float prev_hole_ms;
    float last_hole_ms;
    int hole_count;
} tick_gap_tracker_t;

typedef struct {
    float retained_anchor_ms;
    float signal_lost_ms;
    float recovery_start_ms;
    bool has_retained_state;
    bool recovery_tick_seen;
    bool recovery_marker_seen;
    bool recovery_p_marker_seen;
} recovery_state_t;

typedef struct {
    int p_marker_positions[7];
    int p_marker_count;
    int frame_offset_estimate;
} p_marker_tracker_t;

/* Main sync detector struct additions */
struct sync_detector {
    /* Existing fields... */

    /* New fields */
    sync_state_t state;
    float confidence;
    uint32_t evidence_mask;

    tick_gap_tracker_t tick_gap;
    recovery_state_t recovery;
    p_marker_tracker_t p_tracker;

    int signal_weak_count;          /* Debounce for RECOVERING transition */
    bool expecting_marker_soon;     /* After :59 tick hole */
    float expected_marker_ms;

    bool leap_second_pending;       /* From BCD decode */

    wwv_clock_t *wwv_clock;         /* Optional, NULL-safe */
};
```

---

## 5. API Specification

```c
/*============================================================================
 * sync_detector.h - Enhanced API
 *============================================================================*/

/* Lifecycle */
sync_detector_t *sync_detector_create(const char *csv_path);
void sync_detector_destroy(sync_detector_t *sd);

/* Evidence inputs */
void sync_detector_tick_event(sync_detector_t *sd, float timestamp_ms);
void sync_detector_marker_event(sync_detector_t *sd, float timestamp_ms,
                                 float accum_energy, float duration_ms);
void sync_detector_p_marker_event(sync_detector_t *sd, float timestamp_ms,
                                   float duration_ms);

/* Periodic maintenance (call every ~100ms) */
void sync_detector_periodic_check(sync_detector_t *sd, float current_ms);

/* State queries */
sync_state_t sync_detector_get_state(sync_detector_t *sd);
frame_time_t sync_detector_get_frame_time(sync_detector_t *sd);
float sync_detector_get_confidence(sync_detector_t *sd);

/* Optional integrations */
void sync_detector_set_wwv_clock(sync_detector_t *sd, wwv_clock_t *clk);
void sync_detector_set_leap_second_pending(sync_detector_t *sd, bool pending);

/* Telemetry */
void sync_detector_set_state_callback(sync_detector_t *sd,
                                       void (*cb)(sync_state_t, float conf, void*),
                                       void *user_data);

/* Backward compatibility (keep existing API) */
void sync_detector_tick_marker(sync_detector_t *sd, float timestamp_ms,
                                float duration_ms, float corr_ratio);
float sync_detector_get_last_marker_ms(sync_detector_t *sd);
int sync_detector_get_confirmed_count(sync_detector_t *sd);
int sync_detector_get_good_intervals(sync_detector_t *sd);

/*============================================================================
 * wwv_clock.h - Mode-Aware API
 *============================================================================*/

typedef enum {
    WWV_CLOCK_MODE_RELATIVE,
    WWV_CLOCK_MODE_ABSOLUTE
} wwv_clock_mode_t;

wwv_clock_t *wwv_clock_create(wwv_station_t station);
void wwv_clock_destroy(wwv_clock_t *clk);

/* Mode setting */
void wwv_clock_set_frame_phase(wwv_clock_t *clk, int second, float second_start_ms);
void wwv_clock_set_time(wwv_clock_t *clk, int hour, int minute, int doy, int year);

/* Queries */
wwv_clock_mode_t wwv_clock_get_mode(wwv_clock_t *clk);
bool wwv_clock_is_special_minute(wwv_clock_t *clk);
bool wwv_clock_tick_expected(wwv_clock_t *clk, int second);

/*============================================================================
 * bcd_correlator.h - Simplified Output
 *============================================================================*/

/* Set sync source (required) */
void bcd_correlator_set_sync_source(bcd_correlator_t *corr, sync_detector_t *sd);

/* Symbol event - now includes resolved position */
typedef struct {
    bcd_corr_symbol_t symbol;       /* ZERO, ONE, MARKER, NONE */
    int frame_second;               /* 0-59, from sync_detector */
    float duration_ms;
    float confidence;
    sync_state_t sync_state;        /* For downstream filtering */
} bcd_symbol_event_t;
```

---

## 6. UDP Telemetry Updates

```
Current:  SYM,<char>,<timestamp_ms>,<duration_ms>
New:      SYM,<char>,<second>,<duration_ms>,<confidence>

Current:  SYNC,LOCKED,<anchor_ms>
New:      SYNC,<state>,<second>,<confidence>,<evidence_mask>

New:      SYNC,STATE,<old_state>,<new_state>,<confidence>
          (emitted on state transitions)
```

---

## 7. Implementation Order

| Step | File(s) | Change | Dependency |
|------|---------|--------|------------|
| 1 | sync_detector.h | Add states, structs, new API | None |
| 2 | sync_detector.c | Add tick_gap_tracker, hole detection | Step 1 |
| 3 | sync_detector.c | Add recovery_state, state machine | Step 2 |
| 4 | sync_detector.c | Add confidence system, evidence weights | Step 3 |
| 5 | sync_detector.c | Add p_marker_tracker, frame offset | Step 4 |
| 6 | bcd_time_detector.c | Add consecutive_low_frames (Phase 9) | None |
| 7 | bcd_freq_detector.c | Add consecutive_low_frames (Phase 9) | None |
| 8 | bcd_correlator.c | Add P-marker position gating (Phase 8) | Step 5 |
| 9 | bcd_correlator.c | Consume frame_time_t, update output | Step 5 |
| 10 | wwv_clock.c | Add mode switching | None |
| 11 | sync_detector.c | Integrate wwv_clock (NULL-safe) | Step 10 |
| 12 | waterfall.c | Wire periodic check, P-marker routing | Steps 1-5 |
| 13 | waterfall.c | Wire marker_correlator callback | Step 12 |

---

## 8. Edge Cases Handled

| Edge Case | Handling |
|-----------|----------|
| DUT1 double-tick | Ignore gaps < 100ms in tick_gap_tracker |
| Voice/ID minutes | Reduce evidence weights 50% via wwv_clock |
| Leap second pending | Add 1000ms to timing tolerances |
| Rapid signal flutter | Debounce with signal_weak_count >= 3 |
| Partial recovery fail | → TENTATIVE instead of ACQUIRING |
| Long-session drift | Exponential smoothing of anchor on marker confirm |
| P-marker at wrong position | Reject in bcd_correlator, don't feed to sync |
| Noise pulse ending | Phase 9: require 3+ consecutive low frames |

---

## 9. Verification Criteria

| Milestone | Criteria |
|-----------|----------|
| Phase 8 complete | Zero P-markers at invalid positions in symbol output |
| Phase 9 complete | Reduced spurious short pulses, tighter duration histogram |
| Sync enhanced | SYNC_RECOVERING state transitions visible in telemetry |
| Tick-hole working | EVIDENCE_TICK_HOLE in mask when holes detected |
| Frame alignment | P-markers consistently at 0,9,19,29,39,49,59 |
| Full integration | Controller decodes time correctly across minute boundaries |

---

## 10. Files Modified

### Core sync_detector
- `tools/sync_detector.h` - API expansion
- `tools/sync_detector.c` - Evidence fusion, recovery, tick-holes

### BCD detectors (Phase 9)
- `tools/bcd_time_detector.h` - Add consecutive_low_frames API
- `tools/bcd_time_detector.c` - Debounce pulse end detection
- `tools/bcd_freq_detector.h` - Add consecutive_low_frames API
- `tools/bcd_freq_detector.c` - Debounce pulse end detection

### BCD correlator (Phase 8)
- `tools/bcd_correlator.h` - frame_time_t consumption, new symbol event
- `tools/bcd_correlator.c` - Position gating, simplified output

### WWV clock
- `tools/wwv_clock.h` - Mode switching API
- `tools/wwv_clock.c` - Relative/absolute mode implementation

### Integration
- `tools/waterfall.c` - Wire periodic check, P-marker routing, marker_correlator callback

---

## 11. Testing Strategy

### Unit Testing
- **sync_detector**: Test state transitions with mock evidence
- **tick_gap_tracker**: Verify hole detection with synthetic tick streams
- **recovery_state**: Test retention window and validation logic
- **wwv_clock**: Test mode switching and special minute detection

### Integration Testing
- Feed recorded I/Q data through full pipeline
- Verify frame_time_t.current_second matches expected from BCD decode
- Confirm P-markers only at valid positions
- Test signal loss → recovery → re-lock cycle

### Validation
- 24-hour capture with telemetry logging
- Check state transition patterns
- Measure confidence stability during LOCKED
- Validate position accuracy across minute boundaries

---

## 12. Rollback Plan

If issues arise during implementation:

1. **Phase 9 only**: Can be disabled with preprocessor flag without affecting rest
2. **Phase 8 only**: Can disable position gating, revert to timestamp-based
3. **Full rollback**: All changes are additive; existing API remains functional

---

## 13. Future Enhancements (Out of Scope)

- Maximum-likelihood accumulator (Phase 11)
- Matched filter correlation (Phase 12)
- Comb filter averaging (Phase 13)
- Multi-point sampling within second (Phase 10)

These remain in the roadmap but are not part of this implementation cycle.

---

**Ready to proceed with implementation. Modem team can begin Steps 1-7 in parallel.**
