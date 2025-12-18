/**
 * @file bcd_decoder.h
 * @brief WWV BCD Pulse Detector (Modem Side)
 *
 * Detects pulses from 100 Hz BCD envelope and classifies them as symbols.
 * Outputs symbols with timestamps to controller for frame assembly/decode.
 *
 * Input: Envelope samples from bcd_envelope.c
 * Output: Symbol callbacks (0/1/P with timestamp and width)
 */

#ifndef BCD_DECODER_H
#define BCD_DECODER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

/* Pulse width thresholds (milliseconds) */
#define BCD_PULSE_MIN_MS            100     /* Ignore pulses shorter than this */
#define BCD_PULSE_MAX_MS            1000    /* Ignore pulses longer than this */
#define BCD_PULSE_ZERO_MAX_MS       350     /* 0-350ms = binary 0 */
#define BCD_PULSE_ONE_MAX_MS        650     /* 350-650ms = binary 1 */
                                            /* 650-1000ms = position marker */

/* Hysteresis thresholds (SNR in dB) */
#define BCD_SNR_THRESHOLD_ON        6.0f    /* Pulse ON threshold */
#define BCD_SNR_THRESHOLD_OFF       3.0f    /* Pulse OFF threshold (hysteresis) */

/* Lockout to prevent multiple detections per second */
#define BCD_SYMBOL_LOCKOUT_MS       200     /* Ignore new pulses for this long after symbol */

/*============================================================================
 * Types
 *============================================================================*/

/** Symbol types */
typedef enum {
    BCD_SYMBOL_NONE = -1,       /* No symbol / invalid pulse */
    BCD_SYMBOL_ZERO = 0,        /* Binary 0 (~200ms pulse) */
    BCD_SYMBOL_ONE = 1,         /* Binary 1 (~500ms pulse) */
    BCD_SYMBOL_MARKER = 2       /* Position marker (~800ms pulse) */
} bcd_symbol_t;

/** Subcarrier status (from envelope detector) */
typedef enum {
    BCD_STATUS_ABSENT = 0,
    BCD_STATUS_WEAK,
    BCD_STATUS_PRESENT,
    BCD_STATUS_STRONG
} bcd_status_t;

/** Callback for symbol events */
typedef void (*bcd_symbol_callback_fn)(bcd_symbol_t symbol,
                                       float timestamp_ms,
                                       float pulse_width_ms,
                                       void *user_data);

/** Opaque decoder state */
typedef struct bcd_decoder bcd_decoder_t;

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Create a new BCD pulse detector
 * @return Detector instance or NULL on failure
 */
bcd_decoder_t *bcd_decoder_create(void);

/**
 * Destroy a BCD pulse detector
 */
void bcd_decoder_destroy(bcd_decoder_t *dec);

/**
 * Process an envelope sample
 *
 * @param dec           Detector instance
 * @param timestamp_ms  Milliseconds since start
 * @param envelope      100 Hz envelope magnitude (linear)
 * @param snr_db        Signal-to-noise ratio in dB
 * @param status        Subcarrier status
 */
void bcd_decoder_process_sample(bcd_decoder_t *dec,
                                float timestamp_ms,
                                float envelope,
                                float snr_db,
                                bcd_status_t status);

/**
 * Set callback for symbol events
 */
void bcd_decoder_set_symbol_callback(bcd_decoder_t *dec,
                                     bcd_symbol_callback_fn callback,
                                     void *user_data);

/**
 * Reset detector state
 */
void bcd_decoder_reset(bcd_decoder_t *dec);

/**
 * Get total symbols detected
 */
uint32_t bcd_decoder_get_symbol_count(bcd_decoder_t *dec);

/**
 * Check if currently in a pulse
 */
bool bcd_decoder_is_in_pulse(bcd_decoder_t *dec);

#ifdef __cplusplus
}
#endif

#endif /* BCD_DECODER_H */
