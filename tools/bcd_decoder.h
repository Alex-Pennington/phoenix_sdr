/**
 * @file bcd_decoder.h
 * @brief WWV BCD Time Code Decoder
 *
 * Processes BCD1 messages from the subcarrier detector to decode
 * WWV/WWVH time code. Implements pulse width measurement, frame
 * synchronization, and BCD decoding per the algorithm specification.
 *
 * Input: BCD1 protocol messages (100 Hz envelope at 10ms resolution)
 * Output: Decoded UTC time (hours, minutes, day of year, year)
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

/* Frame parameters */
#define BCD_FRAME_LENGTH            60      /* Symbols per frame */
#define BCD_SYNC_MARKERS_REQUIRED   3       /* P markers needed for sync */

/* Position marker locations within frame */
#define BCD_P0_SECOND               0
#define BCD_P1_SECOND               9
#define BCD_P2_SECOND               19
#define BCD_P3_SECOND               29
#define BCD_P4_SECOND               39
#define BCD_P5_SECOND               49
#define BCD_P6_SECOND               59      /* Also next minute's P0 */

/*============================================================================
 * Types
 *============================================================================*/

/** Symbol types */
typedef enum {
    BCD_SYMBOL_NONE = -1,       /* No symbol yet */
    BCD_SYMBOL_ZERO = 0,        /* Binary 0 (~200ms pulse) */
    BCD_SYMBOL_ONE = 1,         /* Binary 1 (~500ms pulse) */
    BCD_SYMBOL_MARKER = 2       /* Position marker (~800ms pulse) */
} bcd_symbol_t;

/** Decoder sync state */
typedef enum {
    BCD_SYNC_SEARCHING,         /* Looking for P marker pattern */
    BCD_SYNC_CONFIRMING,        /* Found pattern, confirming */
    BCD_SYNC_LOCKED             /* Frame sync achieved */
} bcd_sync_state_t;

/** Subcarrier status (from BCD1 message) */
typedef enum {
    BCD_STATUS_ABSENT = 0,
    BCD_STATUS_WEAK,
    BCD_STATUS_PRESENT,
    BCD_STATUS_STRONG
} bcd_status_t;

/** Decoded time structure */
typedef struct {
    bool valid;                 /* Decode successful */
    int hours;                  /* 0-23 UTC */
    int minutes;                /* 0-59 */
    int day_of_year;            /* 1-366 */
    int year;                   /* 2-digit year (0-99) */
    int dut1_sign;              /* +1, 0, or -1 */
    float dut1_value;           /* 0.0 to 0.9 seconds */
    bool leap_second_pending;   /* Leap second warning */
    bool dst_pending;           /* DST change warning */
    uint64_t decode_timestamp_ms; /* When this was decoded */
} bcd_time_t;

/** Frame quality metrics */
typedef struct {
    int symbols_received;       /* Total symbols in frame */
    int markers_found;          /* Position markers detected */
    int bit_errors;             /* Symbols that failed validation */
    float avg_snr_db;           /* Average SNR during frame */
    float min_snr_db;           /* Minimum SNR during frame */
} bcd_frame_quality_t;

/** Callback for decoded time */
typedef void (*bcd_time_callback_fn)(const bcd_time_t *time,
                                     const bcd_frame_quality_t *quality,
                                     void *user_data);

/** Callback for symbol events (for debugging/display) */
typedef void (*bcd_symbol_callback_fn)(bcd_symbol_t symbol,
                                       int frame_position,
                                       float pulse_width_ms,
                                       void *user_data);

/** Opaque decoder state */
typedef struct bcd_decoder bcd_decoder_t;

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Create a new BCD decoder instance
 * @return Decoder instance or NULL on failure
 */
bcd_decoder_t *bcd_decoder_create(void);

/**
 * Destroy a BCD decoder instance
 */
void bcd_decoder_destroy(bcd_decoder_t *dec);

/**
 * Process a BCD1 protocol message
 * 
 * Parses the message and feeds data to the decoder state machine.
 * Format: "BCD1,HH:MM:SS,timestamp_ms,envelope,snr_db,noise_floor_db,status\n"
 *
 * @param dec       Decoder instance
 * @param message   Null-terminated BCD1 message string
 * @return          true if message was valid and processed
 */
bool bcd_decoder_process_message(bcd_decoder_t *dec, const char *message);

/**
 * Process raw envelope data directly (alternative to message parsing)
 *
 * @param dec           Decoder instance
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
 * Set callback for decoded time events
 */
void bcd_decoder_set_time_callback(bcd_decoder_t *dec,
                                   bcd_time_callback_fn callback,
                                   void *user_data);

/**
 * Set callback for symbol events (debugging)
 */
void bcd_decoder_set_symbol_callback(bcd_decoder_t *dec,
                                     bcd_symbol_callback_fn callback,
                                     void *user_data);

/**
 * Reset decoder state (clear frame buffer, lose sync)
 */
void bcd_decoder_reset(bcd_decoder_t *dec);

/*============================================================================
 * Status Getters
 *============================================================================*/

/**
 * Get current sync state
 */
bcd_sync_state_t bcd_decoder_get_sync_state(bcd_decoder_t *dec);

/**
 * Get current frame position (0-59, or -1 if not synced)
 */
int bcd_decoder_get_frame_position(bcd_decoder_t *dec);

/**
 * Get most recent decoded time (may not be valid)
 */
const bcd_time_t *bcd_decoder_get_last_time(bcd_decoder_t *dec);

/**
 * Get decode statistics
 */
void bcd_decoder_get_stats(bcd_decoder_t *dec,
                           uint32_t *frames_decoded,
                           uint32_t *frames_failed,
                           uint32_t *total_symbols);

/**
 * Check if currently in a pulse
 */
bool bcd_decoder_is_in_pulse(bcd_decoder_t *dec);

/**
 * Print decoder status to stdout
 */
void bcd_decoder_print_status(bcd_decoder_t *dec);

#ifdef __cplusplus
}
#endif

#endif /* BCD_DECODER_H */
