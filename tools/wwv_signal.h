/**
 * @file wwv_signal.h
 * @brief WWV/WWVH composite signal generator
 *
 * Generates complete WWV time signal with ticks, markers, BCD subcarrier,
 * and audio tones following NIST broadcast format.
 */

#ifndef WWV_SIGNAL_H
#define WWV_SIGNAL_H

#include <stdint.h>
#include <stdbool.h>

typedef struct wwv_signal wwv_signal_t;

/** Station type */
typedef enum {
    WWV_STATION_WWV,    // WWV (Colorado) - 1000 Hz tick
    WWV_STATION_WWVH    // WWVH (Hawaii) - 1200 Hz tick
} wwv_station_t;

/**
 * Create WWV signal generator
 * @param start_minute Starting minute (0-59)
 * @param start_hour Starting hour (0-23)
 * @param start_day Starting day of year (1-366)
 * @param start_year Starting year (0-99, last two digits)
 * @param station Station type (WWV or WWVH)
 * @return Allocated generator, or NULL on error
 */
wwv_signal_t *wwv_signal_create(int start_minute, int start_hour, int start_day,
                                 int start_year, wwv_station_t station);

/**
 * Destroy signal generator and free resources
 * @param sig Signal generator instance
 */
void wwv_signal_destroy(wwv_signal_t *sig);

/**
 * Get next I/Q sample pair as int16_t
 * @param sig Signal generator instance
 * @param i Output: in-phase component (scaled to ±32767)
 * @param q Output: quadrature component (scaled to ±32767)
 */
void wwv_signal_get_sample_int16(wwv_signal_t *sig, int16_t *i, int16_t *q);

/**
 * Get current sample counter position
 * @param sig Signal generator instance
 * @return Total samples generated
 */
uint64_t wwv_signal_get_sample_count(const wwv_signal_t *sig);

/**
 * Reset generator to start of new minute
 * @param sig Signal generator instance
 * @param minute New minute (0-59)
 * @param hour New hour (0-23)
 */
void wwv_signal_reset_minute(wwv_signal_t *sig, int minute, int hour);

#endif // WWV_SIGNAL_H
