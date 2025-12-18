/**
 * @file wwv_clock.c
 * @brief WWV broadcast cycle clock - disciplined from system time
 */

#include "wwv_clock.h"
#include <stdlib.h>
#include <time.h>

/*============================================================================
 * Internal Constants
 *============================================================================*/

#define WWV_FREQ_HZ     1000
#define WWVH_FREQ_HZ    1200

/* Seconds with no tick (holes before markers) */
#define HOLE_SECOND_29  29
#define HOLE_SECOND_59  59

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
    wwv_clock_mode_t mode;
    float anchor_ms;        /* Last minute marker timestamp (relative mode) */
};

/*============================================================================
 * Internal Functions
 *============================================================================*/

static wwv_event_type_t get_event_type(int second, int minute) {
    /* Second 0 is always marker */
    if (second == 0) {
        return WWV_EVENT_MARKER;
    }

    /* Holes before markers */
    if (second == HOLE_SECOND_29 || second == HOLE_SECOND_59) {
        return WWV_EVENT_SILENCE;
    }

    /* Station ID minutes - voice around seconds 45-52 */
    if (minute == MINUTE_STATION_ID_1 || minute == MINUTE_STATION_ID_2 ||
        minute == MINUTE_STATION_ID_3 || minute == MINUTE_STATION_ID_4) {
        if (second >= 45 && second <= 52) {
            return WWV_EVENT_STATION_ID;
        }
    }

    /* Geophysical alert minutes */
    if (minute == MINUTE_GEOALERT_1 || minute == MINUTE_GEOALERT_2) {
        if (second >= 15 && second <= 45) {
            return WWV_EVENT_GEOALERT;
        }
    }

    /* Voice time announcements (most minutes, seconds 52-58) */
    if (second >= 52 && second <= 58) {
        return WWV_EVENT_VOICE;
    }

    return WWV_EVENT_TICK;
}

/*============================================================================
 * Public API
 *============================================================================*/

wwv_clock_t *wwv_clock_create(wwv_station_t station) {
    wwv_clock_t *clk = (wwv_clock_t *)calloc(1, sizeof(wwv_clock_t));
    if (!clk) return NULL;

    clk->station = station;
    clk->mode = WWV_CLOCK_MODE_ABSOLUTE;  /* Start in absolute mode */
    clk->anchor_ms = 0.0f;
    return clk;
}

void wwv_clock_destroy(wwv_clock_t *clk) {
    free(clk);
}

wwv_time_t wwv_clock_now(wwv_clock_t *clk) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    return wwv_clock_at(clk, tm->tm_sec, tm->tm_min, tm->tm_hour);
}

wwv_time_t wwv_clock_at(wwv_clock_t *clk, int second, int minute, int hour) {
    wwv_time_t t = {0};

    t.second = second;
    t.minute = minute;
    t.hour = hour;
    t.expected_event = get_event_type(second, minute);
    t.is_hour_marker = (second == 0 && minute == 0);
    t.tick_expected = wwv_tick_expected(second);

    (void)clk; /* Station doesn't affect timing, only frequency */

    return t;
}

bool wwv_tick_expected(int second) {
    /* Ticks on all seconds except 29 and 59 */
    return (second != HOLE_SECOND_29 && second != HOLE_SECOND_59);
}

int wwv_clock_get_freq(wwv_clock_t *clk) {
    if (!clk) return WWV_FREQ_HZ;
    return (clk->station == WWV_STATION_WWV) ? WWV_FREQ_HZ : WWVH_FREQ_HZ;
}

const char *wwv_event_name(wwv_event_type_t type) {
    switch (type) {
        case WWV_EVENT_MARKER:     return "MARKER";
        case WWV_EVENT_TICK:       return "TICK";
        case WWV_EVENT_SILENCE:    return "SILENCE";
        case WWV_EVENT_VOICE:      return "VOICE";
        case WWV_EVENT_GEOALERT:   return "GEOALERT";
        case WWV_EVENT_STATION_ID: return "STATION_ID";
        default:                   return "UNKNOWN";
    }
}

const char *wwv_station_name(wwv_station_t station) {
    return (station == WWV_STATION_WWV) ? "WWV" : "WWVH";
}

/*============================================================================
 * Mode Switching (Unified Sync Integration)
 *============================================================================*/

void wwv_clock_set_mode(wwv_clock_t *clk, wwv_clock_mode_t mode) {
    if (!clk) return;
    clk->mode = mode;
}

wwv_clock_mode_t wwv_clock_get_mode(wwv_clock_t *clk) {
    if (!clk) return WWV_CLOCK_MODE_ABSOLUTE;
    return clk->mode;
}

void wwv_clock_set_anchor(wwv_clock_t *clk, float anchor_ms) {
    if (!clk) return;
    clk->anchor_ms = anchor_ms;
}

float wwv_clock_get_frame_phase_ms(wwv_clock_t *clk) {
    if (!clk) return 0.0f;

    if (clk->mode == WWV_CLOCK_MODE_RELATIVE && clk->anchor_ms > 0.0f) {
        /* Disciplined mode - use anchor time */
        /* Note: Caller must provide current time for this to work properly.
         * For now, return 0 as placeholder - will be updated when caller
         * provides current_ms parameter in future enhancement. */
        return 0.0f;  /* TODO: Needs current_ms parameter */
    } else {
        /* Absolute mode - use system time */
        time_t now_sec = time(NULL);
        struct tm *tm = localtime(&now_sec);

        /* Calculate milliseconds into current minute */
        float phase_ms = (float)(tm->tm_sec * 1000);

        /* Add sub-second precision if available (not standard in C) */
        /* Would need platform-specific code for true millisecond precision */

        return phase_ms;
    }
}

bool wwv_clock_is_special_minute(wwv_clock_t *clk) {
    if (!clk) return false;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int minute = tm->tm_min;

    /* Special minutes: station ID (0, 29, 30, 59) and geoalerts (18, 48) */
    return (minute == MINUTE_STATION_ID_1 ||
            minute == MINUTE_STATION_ID_2 ||
            minute == MINUTE_STATION_ID_3 ||
            minute == MINUTE_STATION_ID_4 ||
            minute == MINUTE_GEOALERT_1 ||
            minute == MINUTE_GEOALERT_2);
}
