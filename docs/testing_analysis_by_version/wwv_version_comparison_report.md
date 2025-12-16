# Phoenix SDR WWV Timing Analysis Report
## Version Comparison: v0.3.2+125.b02e702 vs v0.3.2+120.644e8f9-dirty

---

## Executive Summary

| Metric | v0.3.2+120 | v0.3.2+125 | Change |
|--------|------------|------------|--------|
| **Test Duration** | 7.2 min | 9.0 min | +1.8 min |
| **Total Ticks** | 310 | 215 | -95 |
| **Good Intervals** | 22.6% | 21.9% | -0.7% |
| **Mean Interval** | 1,391 ms | 2,506 ms | **+1,115 ms** |
| **Minute Markers** | 5 | 7 | +2 |
| **Missed Markers** | 1 | 0 | **IMPROVED** |
| **Sync State** | TENTATIVE | **LOCKED** | **IMPROVED** |

### Overall Verdict: **MIXED RESULTS**

**Major Improvements:**
- Achieved LOCKED sync state (previously never got past TENTATIVE)
- Zero missed minute markers (previously missed 1)
- Higher correlation ratios when detecting (avg 50+ vs 20)

**Major Regressions:**
- Severe detection dropouts in minutes 2-7
- ~238 ticks lost in large gaps (5-42 second dropouts)
- Mean interval doubled due to gaps

---

## 1. Sync State Analysis

### Sync Log Progression
| Time | State | Interval | Delta |
|------|-------|----------|-------|
| 11:52:02 | TENTATIVE | 0.0s | 896ms |
| 11:54:04 | TENTATIVE | 121.9s | 869ms |
| 11:56:07 | TENTATIVE | 123.2s | 315ms |
| 11:57:08 | TENTATIVE | 60.9s | 277ms |
| 11:58:07 | **LOCKED** | 59.8s | 880ms |

**Key Finding:** Successfully achieved LOCKED state after ~7 minutes. Delta converged from 896ms → 277ms before lock. This is a significant improvement over v0.3.2+120 which never achieved lock.

---

## 2. Minute Marker Analysis

### All 7 Markers Detected (No Misses)
| Marker | Interval | Error | Corr Ratio |
|--------|----------|-------|------------|
| M1→M2 | 62,160ms | +2,160ms | 27.5 |
| M2→M3 | 59,787ms | -213ms | 52.4 |
| M3→M4 | 60,965ms | +965ms | 41.6 |
| M4→M5 | 62,208ms | +2,208ms | 192.5 |
| M5→M6 | 60,944ms | +944ms | 17.7 |
| M6→M7 | 59,781ms | -219ms | 107.6 |

**Average drift:** +974ms/min (nearly identical to v0.3.2+120's +976ms/min)

**Improvement:** No missed markers (v0.3.2+120 missed 1 between M4→M5)

---

## 3. Critical Issue: Post-Marker Dropouts

### Detection Coverage After Each Minute Marker (first 30 seconds)
| After Marker | Ticks | Coverage |
|--------------|-------|----------|
| M1 | 22 | **88%** ✓ |
| M2 | 2 | 8% ✗ |
| M3 | 2 | 8% ✗ |
| M4 | 1 | 4% ✗ |
| M5 | 4 | 16% ✗ |
| M6 | 1 | 4% ✗ |
| M7 | 3 | 12% ✗ |

**Root Cause Identified:** After the first minute marker, the detector loses track of regular ticks almost completely. Only ~4-16% of expected ticks are detected in the 30 seconds following M2-M7.

---

## 4. Timeline Analysis (30-second blocks)

```
Block    | Ticks | Coverage | Status
---------+-------+----------+--------------
0:00     |    23 |     77%  | PARTIAL
0:30     |    20 |     67%  | PARTIAL
1:00     |    23 |     77%  | PARTIAL
1:30     |    20 |     67%  | PARTIAL
2:00     |     2 |      7%  | ** DROPOUT **
2:30     |    10 |     33%  | POOR
3:00     |     2 |      7%  | ** DROPOUT **
3:30     |    12 |     40%  | POOR
4:00     |     0 |      0%  | ** DROPOUT **
4:30     |    11 |     37%  | POOR
5:00     |     4 |     13%  | ** DROPOUT **
5:30     |    14 |     47%  | POOR
6:00     |     3 |     10%  | ** DROPOUT **
6:30     |    10 |     33%  | POOR
7:00     |     3 |     10%  | ** DROPOUT **
7:30     |    12 |     40%  | POOR
8:00     |    24 |     80%  | GOOD (recovered!)
8:30     |    22 |     73%  | PARTIAL
```

**Pattern:** Minutes 0-1 = GOOD, Minutes 2-7 = DROPOUT/POOR, Minute 8 = RECOVERED

---

## 5. Gap Analysis

### Large Gaps (>5 seconds): 15 occurrences
| Gap Duration | Approx Ticks Lost |
|--------------|-------------------|
| 42.2s | ~41 |
| 30.9s | ~29 |
| 22.8s | ~21 |
| 20.0s | ~18 |
| 17.3s | ~16 |
| 16.3s | ~15 |
| 16.1s | ~15 |
| 16.0s | ~14 |
| 15.8s | ~14 |
| 15.2s | ~14 |

**Total ticks lost in large gaps: ~238**

### Correlation Before Gaps
Gaps occur regardless of previous tick quality:
- Some preceded by high corr (55.9, 188.9)
- Some preceded by low corr (3.0, 3.9)
- **No clear predictor** for dropout onset

---

## 6. Quality Comparison by Phase

|  | Early (0-1 min) | Mid (2-7 min) | Late (8 min) |
|--|-----------------|---------------|--------------|
| Tick Count | 86 | 83 | 46 |
| Good Interval % | 27% | 16% | 24% |
| Mean Corr Ratio | 30.3 | 16.3 | 16.3 |

---

## 7. Voice Period Impact

| Metric | v0.3.2+120 | v0.3.2+125 |
|--------|------------|------------|
| VOICE bad intervals | 57.1% | 59.0% |
| Non-VOICE bad intervals | 49.3% | 58.5% |

**Finding:** Voice periods are no longer disproportionately problematic - both voice and non-voice now have similar bad interval rates (~58-59%). This suggests the new dropout behavior is **not voice-related**.

---

## 8. What Changed: Likely Hypothesis

Based on the data pattern:

1. **M1 (first marker) works correctly** - 88% coverage afterward
2. **M2+ (subsequent markers) cause detector to "stall"** - 4-16% coverage
3. **Recovery happens eventually** - minute 8 shows 80% coverage again

**Possible causes:**
- Minute marker detection may be leaving the detector in a bad state
- Threshold/baseline may be getting corrupted after marker processing
- State machine may be waiting for a condition that rarely triggers
- Buffer or timing issue introduced between commits 120 and 125

---

## 9. Recommendations

### Immediate Investigation
1. **Compare commits 120→125** - Look for changes to:
   - Post-marker state handling
   - Baseline/threshold recalculation after markers
   - Buffer management or timing code

2. **Add diagnostic logging** after minute markers to capture:
   - Threshold values
   - Baseline values  
   - Detection state machine position

### Possible Quick Fixes
1. **Force threshold recalculation** after each minute marker
2. **Reset detection state** after marker detection completes
3. **Add timeout** to recover from "stalled" state

---

## 10. Summary

| Aspect | Assessment |
|--------|------------|
| Sync Acquisition | **IMPROVED** ✓ |
| Minute Marker Detection | **IMPROVED** ✓ |
| Correlation Quality (when detecting) | **IMPROVED** ✓ |
| Tick Detection Continuity | **REGRESSED** ✗ |
| Overall Reliability | **REGRESSED** ✗ |

**v0.3.2+125 achieves sync lock (good!) but has a severe bug causing detection dropouts after minute markers (bad!)**

The changes that enabled sync locking appear to have introduced a side effect where the detector "stalls" after processing minute markers M2-M7, only recovering sporadically.
