/**
 * @file wwv_clock.h
 * @brief WWV broadcast cycle clock and state tracker
 *
 * Tracks position within WWV broadcast cycle:
 *   - Second within minute (0-59)
 *   - Minute within hour (0-59)
 *   - Expected events (ticks, markers, voice, silence)
 *
 * WWV Minute Structure:
 *   Second 0:     800ms marker tone (1000Hz)
 *   Seconds 1-28: 5ms ticks (with 100Hz BCD subcarrier)
 *   Second 29:    NO TICK (hole before minute marker)
 *   Seconds 30-58: 5ms ticks
 *   Second 59:    NO TICK (hole before minute marker)
 *
 * WWV Hourly Structure:
 *   Minute 0:     Station ID ("WWV Fort Collins Colorado")
 *   Minute 1:     NIST reserved
 *   Minutes 2-8:  Available for special announcements
 *   Minute 18:    Geophysical alert (GPS status, solar flux, K-index)
 *   Minute 29:    Station ID
 *   Minute 30:    Station ID  
 *   Minute 43:    NIST reserved
 *   Minutes 44-51: Voice time announcements each minute
 *   Minute 48:    Geophysical alert
 *   Minute 59:    Station ID
 *
 * WWVH (Hawaii) uses 1200Hz tones instead of 1000Hz
 */

#ifndef WWV_CLOCK_H
#define WWV_CLOCK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Constants
 *============================================================================*/

#define WWV_SECONDS_PER_MINUTE  60
#define WWV_MINUTES_PER_HOUR    60

/* Frequencies */
#define WWV_TICK_FREQ_HZ        1000    /* WWV uses 1000Hz */
#define WWVH_TICK_FREQ_HZ       1200    /* WWVH uses 1200Hz */
#define WWV_HOUR_MARKER_FREQ_HZ 1500    /* Hour marker is 1500Hz */

/*============================================================================
 * Types
 *============================================================================*/

typedef enum {
    WWV_STATION_WWV,        /* Fort Collins, Colorado */
    WWV_STATION_WWVH        /* Kauai, Hawaii */
} wwv_station_t;

typedef enum {
    WWV_EVENT_TICK,         /* Normal 5ms tick */
    WWV_EVENT_MARKER,       /* 800ms minute/hour marker */
    WWV_EVENT_SILENCE,      /* Expected silence (seconds 29, 59) */
    WWV_EVENT_VOICE,        /* Voice announcement expected */
    WWV_EVENT_GEOALERT,     /* Geophysical alert expected */
    WWV_EVENT_STATION_ID    /* Station identification */
} wwv_event_type_t;

typedef enum {
    WWV_SYNC_NONE,          /* No sync - free running */
    WWV_SYNC_COARSE,        /* Synced to minute marker, tracking */
    WWV_SYNC_FINE           /* Multiple markers confirmed, locked */
} wwv_sync_state_t;

typedef struct {
    wwv_event_type_t type;
    int second;             /* 0-59 */
    int minute;             /* 0-59 */
    bool is_hour_marker;    /* True if second 0 of minute 0 */
} wwv_expected_event_t;

typedef struct wwv_clock wwv_clock_t;

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Create a WWV clock tracker
 * @param station Which station to track (WWV or WWVH)
 * @return Clock handle or NULL on error
 */
wwv_clock_t *wwv_clock_create(wwv_station_t station);

/**
 * Destroy clock and free resources
 */
void wwv_clock_destroy(wwv_clock_t *clk);

/**
 * Report a detected tick to the clock
 * @param clk Clock handle
 * @param timestamp_ms Timestamp in milliseconds since start
 * @param is_marker True if this was a minute marker (800ms pulse)
 */
void wwv_clock_report_tick(wwv_clock_t *clk, float timestamp_ms, bool is_marker);

/**
 * Update clock with elapsed time (call periodically)
 * @param clk Clock handle
 * @param timestamp_ms Current timestamp in milliseconds
 */
void wwv_clock_update(wwv_clock_t *clk, float timestamp_ms);

/**
 * Get current position in broadcast cycle
 * @param clk Clock handle
 * @param second Output: current second (0-59)
 * @param minute Output: current minute (0-59)
 */
void wwv_clock_get_position(wwv_clock_t *clk, int *second, int *minute);

/**
 * Get expected event for current position
 */
wwv_expected_event_t wwv_clock_get_expected(wwv_clock_t *clk);

/**
 * Get sync state
 */
wwv_sync_state_t wwv_clock_get_sync_state(wwv_clock_t *clk);

/**
 * Get timing error from last marker (ms, positive = late)
 */
float wwv_clock_get_timing_error(wwv_clock_t *clk);

/**
 * Get number of consecutive markers locked
 */
int wwv_clock_get_lock_count(wwv_clock_t *clk);

/**
 * Print clock status
 */
void wwv_clock_print_status(wwv_clock_t *clk);

/**
 * Get event type name string
 */
const char *wwv_event_type_name(wwv_event_type_t type);

/**
 * Get sync state name string
 */
const char *wwv_sync_state_name(wwv_sync_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* WWV_CLOCK_H */
