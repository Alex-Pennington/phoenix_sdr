/**
 * @file wwv_clock.c
 * @brief WWV broadcast cycle clock implementation
 *
 * Tracks position within WWV/WWVH broadcast cycle by:
 *   1. Detecting minute markers to establish sync
 *   2. Counting ticks to track seconds
 *   3. Predicting expected events
 *   4. Measuring timing error for drift correction
 */

#include "wwv_clock.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/*============================================================================
 * Internal Constants
 *============================================================================*/

/* Timing tolerances */
#define TICK_INTERVAL_MS        1000.0f     /* Expected tick interval */
#define TICK_TOLERANCE_MS       100.0f      /* Acceptable timing error */
#define MARKER_INTERVAL_MS      60000.0f    /* Expected marker interval */
#define MARKER_TOLERANCE_MS     500.0f      /* Marker timing tolerance */

/* Sync requirements */
#define SYNC_COARSE_MARKERS     1           /* Markers needed for coarse sync */
#define SYNC_FINE_MARKERS       3           /* Consecutive markers for fine sync */
#define SYNC_LOST_TOLERANCE_MS  2000.0f     /* Lose sync if off by this much */

/* Seconds with no tick (holes before markers) */
#define HOLE_SECOND_29          29
#define HOLE_SECOND_59          59

/* Special minutes */
#define MINUTE_STATION_ID_1     0
#define MINUTE_STATION_ID_2     29
#define MINUTE_STATION_ID_3     30
#define MINUTE_STATION_ID_4     59
#define MINUTE_GEOALERT_1       18
#define MINUTE_GEOALERT_2       48

/*============================================================================
 * Internal State
 *============================================================================*/

struct wwv_clock {
    wwv_station_t station;
    
    /* Current position (estimated) */
    int current_second;         /* 0-59 */
    int current_minute;         /* 0-59 */
    float fractional_second;    /* 0.0-1.0 within current second */
    
    /* Timing reference */
    float last_marker_timestamp_ms;     /* When we last saw a marker */
    float last_tick_timestamp_ms;       /* When we last saw any tick */
    float clock_base_ms;                /* Timestamp when second was 0 */
    
    /* Sync state */
    wwv_sync_state_t sync_state;
    int consecutive_markers;    /* How many markers in a row at expected time */
    int total_markers;
    int total_ticks;
    int missed_ticks;
    int unexpected_ticks;
    
    /* Timing error tracking */
    float last_timing_error_ms; /* Error at last marker (positive = late) */
    float accumulated_drift_ms; /* Total drift since sync */
    
    /* Statistics */
    float avg_tick_interval_ms;
    int tick_interval_count;
};

/*============================================================================
 * Internal Functions
 *============================================================================*/

/**
 * Check if a second should have a tick
 */
static bool second_has_tick(int second) {
    return (second != HOLE_SECOND_29 && second != HOLE_SECOND_59);
}

/**
 * Get expected event type for a given second/minute
 */
static wwv_event_type_t get_event_type(int second, int minute) {
    if (second == 0) {
        return WWV_EVENT_MARKER;
    }
    
    if (second == HOLE_SECOND_29 || second == HOLE_SECOND_59) {
        return WWV_EVENT_SILENCE;
    }
    
    /* Check for special minutes */
    if (minute == MINUTE_STATION_ID_1 || minute == MINUTE_STATION_ID_2 ||
        minute == MINUTE_STATION_ID_3 || minute == MINUTE_STATION_ID_4) {
        /* Station ID typically at seconds 52-59 area */
        if (second >= 45 && second <= 52) {
            return WWV_EVENT_STATION_ID;
        }
    }
    
    if (minute == MINUTE_GEOALERT_1 || minute == MINUTE_GEOALERT_2) {
        /* Geophysical alerts in middle of minute */
        if (second >= 15 && second <= 45) {
            return WWV_EVENT_GEOALERT;
        }
    }
    
    /* Voice time announcements */
    if (second >= 52 && second <= 58) {
        return WWV_EVENT_VOICE;
    }
    
    return WWV_EVENT_TICK;
}

/**
 * Advance clock by elapsed milliseconds
 */
static void advance_clock(wwv_clock_t *clk, float elapsed_ms) {
    if (elapsed_ms <= 0) return;
    
    float total_ms = clk->fractional_second * 1000.0f + elapsed_ms;
    int seconds_elapsed = (int)(total_ms / 1000.0f);
    clk->fractional_second = fmodf(total_ms, 1000.0f) / 1000.0f;
    
    /* Advance seconds */
    clk->current_second += seconds_elapsed;
    while (clk->current_second >= WWV_SECONDS_PER_MINUTE) {
        clk->current_second -= WWV_SECONDS_PER_MINUTE;
        clk->current_minute++;
        if (clk->current_minute >= WWV_MINUTES_PER_HOUR) {
            clk->current_minute = 0;
        }
    }
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

wwv_clock_t *wwv_clock_create(wwv_station_t station) {
    wwv_clock_t *clk = (wwv_clock_t *)calloc(1, sizeof(wwv_clock_t));
    if (!clk) return NULL;
    
    clk->station = station;
    clk->sync_state = WWV_SYNC_NONE;
    clk->current_second = 0;
    clk->current_minute = 0;
    clk->fractional_second = 0.0f;
    clk->last_marker_timestamp_ms = -1.0f;
    clk->last_tick_timestamp_ms = -1.0f;
    clk->clock_base_ms = 0.0f;
    
    printf("[WWV_CLK] Created %s clock tracker\n", 
           station == WWV_STATION_WWV ? "WWV" : "WWVH");
    
    return clk;
}

void wwv_clock_destroy(wwv_clock_t *clk) {
    if (clk) {
        printf("[WWV_CLK] Destroyed. Total: %d markers, %d ticks, %d missed\n",
               clk->total_markers, clk->total_ticks, clk->missed_ticks);
        free(clk);
    }
}

void wwv_clock_report_tick(wwv_clock_t *clk, float timestamp_ms, bool is_marker) {
    if (!clk) return;
    
    if (is_marker) {
        clk->total_markers++;
        
        /* Calculate timing error if we have a reference */
        if (clk->sync_state != WWV_SYNC_NONE && clk->last_marker_timestamp_ms > 0) {
            float expected_interval = MARKER_INTERVAL_MS;
            float actual_interval = timestamp_ms - clk->last_marker_timestamp_ms;
            clk->last_timing_error_ms = actual_interval - expected_interval;
            clk->accumulated_drift_ms += clk->last_timing_error_ms;
            
            /* Check if marker arrived when expected */
            if (fabsf(clk->last_timing_error_ms) < MARKER_TOLERANCE_MS) {
                clk->consecutive_markers++;
                if (clk->consecutive_markers >= SYNC_FINE_MARKERS) {
                    if (clk->sync_state != WWV_SYNC_FINE) {
                        clk->sync_state = WWV_SYNC_FINE;
                        printf("[WWV_CLK] FINE SYNC achieved! %d consecutive markers\n",
                               clk->consecutive_markers);
                    }
                }
            } else {
                /* Marker was unexpected - reset consecutive count */
                clk->consecutive_markers = 1;
                if (fabsf(clk->last_timing_error_ms) > SYNC_LOST_TOLERANCE_MS) {
                    printf("[WWV_CLK] Sync lost! Error %.0fms, resetting\n", 
                           clk->last_timing_error_ms);
                    clk->sync_state = WWV_SYNC_COARSE;
                }
            }
        } else {
            clk->consecutive_markers = 1;
        }
        
        /* Reset clock to second 0 on marker */
        clk->current_second = 0;
        clk->fractional_second = 0.0f;
        clk->clock_base_ms = timestamp_ms;
        clk->last_marker_timestamp_ms = timestamp_ms;
        clk->last_tick_timestamp_ms = timestamp_ms;
        
        if (clk->sync_state == WWV_SYNC_NONE) {
            clk->sync_state = WWV_SYNC_COARSE;
            printf("[WWV_CLK] COARSE SYNC - marker detected, tracking started\n");
        }
        
        printf("[WWV_CLK] Marker @ %.1fs -> %02d:%02d (err=%.0fms, lock=%d)\n",
               timestamp_ms / 1000.0f, clk->current_minute, clk->current_second,
               clk->last_timing_error_ms, clk->consecutive_markers);
        
    } else {
        /* Regular tick */
        clk->total_ticks++;
        
        if (clk->sync_state != WWV_SYNC_NONE && clk->last_tick_timestamp_ms > 0) {
            float interval = timestamp_ms - clk->last_tick_timestamp_ms;
            
            /* Update average interval */
            clk->avg_tick_interval_ms = (clk->avg_tick_interval_ms * clk->tick_interval_count + interval) /
                                        (clk->tick_interval_count + 1);
            clk->tick_interval_count++;
            
            /* Advance clock based on interval */
            advance_clock(clk, interval);
            
            /* Check if tick matches expected second */
            if (!second_has_tick(clk->current_second)) {
                clk->unexpected_ticks++;
            }
        }
        
        clk->last_tick_timestamp_ms = timestamp_ms;
    }
}

void wwv_clock_update(wwv_clock_t *clk, float timestamp_ms) {
    if (!clk || clk->sync_state == WWV_SYNC_NONE) return;
    
    if (clk->last_tick_timestamp_ms > 0) {
        float elapsed = timestamp_ms - clk->last_tick_timestamp_ms;
        if (elapsed > 0 && elapsed < 5000.0f) {  /* Sanity check */
            advance_clock(clk, elapsed);
            clk->last_tick_timestamp_ms = timestamp_ms;
        }
    }
}

void wwv_clock_get_position(wwv_clock_t *clk, int *second, int *minute) {
    if (!clk) {
        if (second) *second = 0;
        if (minute) *minute = 0;
        return;
    }
    if (second) *second = clk->current_second;
    if (minute) *minute = clk->current_minute;
}

wwv_expected_event_t wwv_clock_get_expected(wwv_clock_t *clk) {
    wwv_expected_event_t event = {0};
    if (!clk) return event;
    
    event.second = clk->current_second;
    event.minute = clk->current_minute;
    event.type = get_event_type(event.second, event.minute);
    event.is_hour_marker = (event.second == 0 && event.minute == 0);
    
    return event;
}

wwv_sync_state_t wwv_clock_get_sync_state(wwv_clock_t *clk) {
    return clk ? clk->sync_state : WWV_SYNC_NONE;
}

float wwv_clock_get_timing_error(wwv_clock_t *clk) {
    return clk ? clk->last_timing_error_ms : 0.0f;
}

int wwv_clock_get_lock_count(wwv_clock_t *clk) {
    return clk ? clk->consecutive_markers : 0;
}

void wwv_clock_print_status(wwv_clock_t *clk) {
    if (!clk) return;
    
    printf("\n=== WWV CLOCK STATUS ===\n");
    printf("Station: %s\n", clk->station == WWV_STATION_WWV ? "WWV" : "WWVH");
    printf("Sync: %s (lock=%d)\n", wwv_sync_state_name(clk->sync_state), 
           clk->consecutive_markers);
    printf("Position: %02d:%02d.%d\n", clk->current_minute, clk->current_second,
           (int)(clk->fractional_second * 10));
    printf("Timing error: %.1fms (drift: %.1fms)\n", 
           clk->last_timing_error_ms, clk->accumulated_drift_ms);
    printf("Stats: %d markers, %d ticks, %d missed, %d unexpected\n",
           clk->total_markers, clk->total_ticks, clk->missed_ticks, clk->unexpected_ticks);
    if (clk->tick_interval_count > 0) {
        printf("Avg tick interval: %.1fms\n", clk->avg_tick_interval_ms);
    }
    printf("========================\n\n");
}

const char *wwv_event_type_name(wwv_event_type_t type) {
    switch (type) {
        case WWV_EVENT_TICK:       return "TICK";
        case WWV_EVENT_MARKER:     return "MARKER";
        case WWV_EVENT_SILENCE:    return "SILENCE";
        case WWV_EVENT_VOICE:      return "VOICE";
        case WWV_EVENT_GEOALERT:   return "GEOALERT";
        case WWV_EVENT_STATION_ID: return "STATION_ID";
        default:                   return "UNKNOWN";
    }
}

const char *wwv_sync_state_name(wwv_sync_state_t state) {
    switch (state) {
        case WWV_SYNC_NONE:   return "NONE";
        case WWV_SYNC_COARSE: return "COARSE";
        case WWV_SYNC_FINE:   return "FINE";
        default:              return "UNKNOWN";
    }
}
