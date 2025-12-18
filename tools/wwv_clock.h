/**
 * @file wwv_clock.h
 * @brief WWV broadcast cycle clock - disciplined from system time
 *
 * Simple clock that uses system time to determine position in WWV broadcast cycle.
 * Knows the WWV/WWVH schedule to predict expected events.
 *
 * WWV Minute Structure:
 *   Second 0:      800ms marker tone (1000Hz)
 *   Seconds 1-28:  5ms ticks
 *   Second 29:     NO TICK (hole before minute marker)
 *   Seconds 30-58: 5ms ticks
 *   Second 59:     NO TICK (hole before minute marker)
 *
 * WWV Hourly Structure:
 *   Minutes 0, 29, 30, 59: Station ID
 *   Minutes 18, 48: Geophysical alerts
 *   Voice time announcements overlap seconds 52-58
 *
 * WWVH uses 1200Hz tones instead of 1000Hz
 */

#ifndef WWV_CLOCK_H
#define WWV_CLOCK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Types
 *============================================================================*/

typedef enum {
    WWV_STATION_WWV,        /* Fort Collins, Colorado - 1000Hz */
    WWV_STATION_WWVH        /* Kauai, Hawaii - 1200Hz */
} wwv_station_t;

typedef enum {
    WWV_EVENT_MARKER,       /* Second 0: 800ms minute/hour marker */
    WWV_EVENT_TICK,         /* Normal 5ms tick expected */
    WWV_EVENT_SILENCE,      /* Seconds 29, 59: no tick expected */
    WWV_EVENT_VOICE,        /* Voice announcement may interfere */
    WWV_EVENT_GEOALERT,     /* Geophysical alert broadcast */
    WWV_EVENT_STATION_ID    /* Station identification */
} wwv_event_type_t;

typedef struct {
    int second;             /* 0-59 */
    int minute;             /* 0-59 */
    int hour;               /* 0-23 */
    wwv_event_type_t expected_event;
    bool is_hour_marker;    /* True if second 0 of minute 0 */
    bool tick_expected;     /* True if a tick should occur this second */
} wwv_time_t;

/**
 * Clock mode determines how minute tracking works
 */
typedef enum {
    WWV_CLOCK_MODE_ABSOLUTE,    /* System time drives clock (normal mode) */
    WWV_CLOCK_MODE_RELATIVE     /* Disciplined by sync detector anchor */
} wwv_clock_mode_t;

typedef struct wwv_clock wwv_clock_t;

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Create a WWV clock for specified station
 */
wwv_clock_t *wwv_clock_create(wwv_station_t station);

/**
 * Destroy clock
 */
void wwv_clock_destroy(wwv_clock_t *clk);

/**
 * Get current position in broadcast cycle from system time
 */
wwv_time_t wwv_clock_now(wwv_clock_t *clk);

/**
 * Get position for specific second/minute (for lookups)
 */
wwv_time_t wwv_clock_at(wwv_clock_t *clk, int second, int minute, int hour);

/**
 * Check if tick is expected at given second
 */
bool wwv_tick_expected(int second);

/**
 * Get station frequency (1000Hz for WWV, 1200Hz for WWVH)
 */
int wwv_clock_get_freq(wwv_clock_t *clk);

/**
 * Get event type name string
 */
const char *wwv_event_name(wwv_event_type_t type);

/**
 * Get station name string
 */
const char *wwv_station_name(wwv_station_t station);

/*============================================================================
 * Mode Switching (Unified Sync Integration)
 *============================================================================*/

/**
 * Set clock mode (absolute vs relative)
 */
void wwv_clock_set_mode(wwv_clock_t *clk, wwv_clock_mode_t mode);

/**
 * Get current clock mode
 */
wwv_clock_mode_t wwv_clock_get_mode(wwv_clock_t *clk);

/**
 * Discipline clock to sync detector anchor (relative mode only)
 * @param anchor_ms Minute marker timestamp in milliseconds
 */
void wwv_clock_set_anchor(wwv_clock_t *clk, float anchor_ms);

/**
 * Get frame phase - milliseconds since last minute marker (0-60000)
 * Returns estimate based on system time in absolute mode,
 * or disciplined position in relative mode.
 */
float wwv_clock_get_frame_phase_ms(wwv_clock_t *clk);

/**
 * Check if current minute is "special" (station ID, geoalert)
 * Used by sync detector to discount evidence during noisy periods.
 */
bool wwv_clock_is_special_minute(wwv_clock_t *clk);

#ifdef __cplusplus
}
#endif

#endif /* WWV_CLOCK_H */
