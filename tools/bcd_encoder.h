/**
 * @file bcd_encoder.h
 * @brief WWV/WWVH BCD time code encoder
 *
 * Generates per-second BCD symbol patterns following WWV broadcast format.
 * Implements IRIG-H modified format with LSB-first encoding per NIST spec.
 */

#ifndef BCD_ENCODER_H
#define BCD_ENCODER_H

#include <stdbool.h>

typedef struct bcd_encoder bcd_encoder_t;

/** BCD symbol types */
typedef enum {
    BCD_ZERO = 0,      // Binary 0: 200ms high, 800ms low
    BCD_ONE = 1,       // Binary 1: 500ms high, 500ms low
    BCD_MARKER = 2     // Position marker: 800ms high, 200ms low
} bcd_symbol_t;

/**
 * Create BCD encoder instance
 * @return Allocated encoder, or NULL on error
 */
bcd_encoder_t *bcd_encoder_create(void);

/**
 * Destroy encoder and free resources
 * @param enc Encoder instance
 */
void bcd_encoder_destroy(bcd_encoder_t *enc);

/**
 * Set current time for encoding (call once per minute)
 * @param enc Encoder instance
 * @param minute Minute of hour (0-59)
 * @param hour Hour of day (0-23)
 * @param day_of_year Day of year (1-366)
 * @param year Year (0-99, last two digits)
 */
void bcd_encoder_set_time(bcd_encoder_t *enc, int minute, int hour, int day_of_year, int year);

/**
 * Get BCD symbol for given second
 * @param enc Encoder instance
 * @param second Second within minute (0-59)
 * @return BCD symbol type
 */
bcd_symbol_t bcd_encoder_get_symbol(const bcd_encoder_t *enc, int second);

/**
 * Get 100 Hz subcarrier modulation level for current sample
 * @param enc Encoder instance
 * @param second Second within minute (0-59)
 * @param sample_in_second Sample offset within second (0-1999999 at 2 Msps)
 * @return Modulation depth: 0.18 (high) or 0.03 (low)
 */
float bcd_encoder_get_modulation(const bcd_encoder_t *enc, int second, int sample_in_second);

/**
 * Get pulse width in milliseconds for a symbol
 * @param symbol BCD symbol type
 * @return High-level duration in milliseconds
 */
int bcd_get_pulse_width_ms(bcd_symbol_t symbol);

#endif // BCD_ENCODER_H
