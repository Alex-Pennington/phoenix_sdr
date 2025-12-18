/**
 * @file bcd_decoder.c
 * @brief WWV BCD Time Code Decoder Implementation
 *
 * Implements the algorithm from wwv_bcd_decoder_algorithm.md:
 * - Pulse width measurement with hysteresis thresholding
 * - Symbol classification (0/1/P markers)
 * - Frame synchronization using position marker pattern
 * - BCD decoding of hours, minutes, day of year, year
 */

#include "bcd_decoder.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/*============================================================================
 * Internal Constants
 *============================================================================*/

/* Position marker seconds within frame */
static const int P_MARKER_POSITIONS[] = {0, 9, 19, 29, 39, 49, 59};
#define NUM_P_MARKERS 7

/* Expected spacing between consecutive P markers */
static const int P_MARKER_SPACING[] = {9, 10, 10, 10, 10, 10};  /* P0->P1, P1->P2, etc. */

/*============================================================================
 * Internal State Structure
 *============================================================================*/

struct bcd_decoder {
    /* Pulse detection state */
    bool in_pulse;                  /* Currently detecting a pulse */
    float pulse_start_ms;           /* Timestamp when pulse started */
    float pulse_snr_sum;            /* Sum of SNR during pulse (for averaging) */
    int pulse_sample_count;         /* Samples in current pulse */
    
    /* Hysteresis state */
    float adaptive_threshold;       /* Adaptive noise floor tracking */
    
    /* Symbol buffer (circular, holds up to 2 frames for sync) */
    bcd_symbol_t symbol_buffer[120];
    float symbol_timestamps[120];   /* When each symbol was received */
    float symbol_widths[120];       /* Pulse width of each symbol */
    int symbol_write_idx;           /* Next write position */
    int symbol_count;               /* Total symbols received */
    
    /* Frame synchronization */
    bcd_sync_state_t sync_state;
    int frame_position;             /* Current position within frame (0-59) */
    int sync_confidence;            /* Consecutive good frames */
    int p_marker_miss_count;        /* Consecutive missed P markers */
    
    /* Current frame being decoded */
    bcd_symbol_t frame[BCD_FRAME_LENGTH];
    float frame_snr_sum;
    float frame_snr_min;
    int frame_symbol_count;
    
    /* Decoded time (most recent) */
    bcd_time_t last_time;
    
    /* Statistics */
    uint32_t frames_decoded;
    uint32_t frames_failed;
    uint32_t total_symbols;
    
    /* Timing */
    float last_timestamp_ms;
    bool first_sample;
    
    /* Callbacks */
    bcd_time_callback_fn time_callback;
    void *time_callback_data;
    bcd_symbol_callback_fn symbol_callback;
    void *symbol_callback_data;
};

/*============================================================================
 * Internal Helpers
 *============================================================================*/

/**
 * Classify pulse width into symbol type
 */
static bcd_symbol_t classify_pulse(float width_ms) {
    if (width_ms < BCD_PULSE_MIN_MS || width_ms > BCD_PULSE_MAX_MS) {
        return BCD_SYMBOL_NONE;  /* Invalid pulse */
    }
    
    if (width_ms < BCD_PULSE_ZERO_MAX_MS) {
        return BCD_SYMBOL_ZERO;  /* ~200ms = binary 0 */
    } else if (width_ms < BCD_PULSE_ONE_MAX_MS) {
        return BCD_SYMBOL_ONE;   /* ~500ms = binary 1 */
    } else {
        return BCD_SYMBOL_MARKER; /* ~800ms = position marker */
    }
}

/**
 * Check if a position is a P marker location
 */
static bool is_p_marker_position(int pos) {
    for (int i = 0; i < NUM_P_MARKERS; i++) {
        if (P_MARKER_POSITIONS[i] == pos) return true;
    }
    return false;
}

/**
 * Store a symbol in the circular buffer
 */
static void store_symbol(bcd_decoder_t *dec, bcd_symbol_t symbol, 
                         float timestamp_ms, float width_ms) {
    dec->symbol_buffer[dec->symbol_write_idx] = symbol;
    dec->symbol_timestamps[dec->symbol_write_idx] = timestamp_ms;
    dec->symbol_widths[dec->symbol_write_idx] = width_ms;
    
    dec->symbol_write_idx = (dec->symbol_write_idx + 1) % 120;
    dec->symbol_count++;
    dec->total_symbols++;
}

/**
 * Get symbol from buffer (0 = most recent, negative = older)
 */
static bcd_symbol_t get_symbol(bcd_decoder_t *dec, int offset) {
    int idx = dec->symbol_write_idx - 1 + offset;
    while (idx < 0) idx += 120;
    idx %= 120;
    return dec->symbol_buffer[idx];
}

/**
 * Look for P marker pattern in recent symbols to establish sync
 * Returns frame offset if pattern found, -1 otherwise
 */
static int find_sync_pattern(bcd_decoder_t *dec) {
    if (dec->symbol_count < 20) return -1;  /* Need enough history */
    
    /* Look for at least 3 P markers with correct spacing */
    int p_markers_found = 0;
    int last_p_offset = -1;
    int inferred_frame_start = -1;
    
    /* Scan recent symbols (most recent first) */
    for (int i = 0; i < 70 && i < dec->symbol_count; i++) {
        bcd_symbol_t sym = get_symbol(dec, -i);
        
        if (sym == BCD_SYMBOL_MARKER) {
            if (last_p_offset < 0) {
                /* First P marker found */
                last_p_offset = i;
                p_markers_found = 1;
            } else {
                /* Check spacing to previous P marker */
                int spacing = last_p_offset - i;  /* Positive, going backwards */
                
                /* Valid spacings are 9 (P0->P1) or 10 (all others) */
                if (spacing == 9 || spacing == 10) {
                    p_markers_found++;
                    
                    /* Infer which P marker this is based on spacing */
                    if (p_markers_found >= BCD_SYNC_MARKERS_REQUIRED) {
                        /* We have enough markers - calculate frame start */
                        /* The most recent P marker (at last_p_offset) */
                        /* needs to be mapped to a frame position */
                        
                        /* Count back through spacings to find P0 */
                        int pos = 0;
                        int scan = last_p_offset;
                        
                        /* This is simplified - just use the pattern */
                        inferred_frame_start = last_p_offset;
                        return inferred_frame_start;
                    }
                    
                    last_p_offset = i;
                } else if (spacing > 15) {
                    /* Gap too large, reset search */
                    last_p_offset = i;
                    p_markers_found = 1;
                }
            }
        }
    }
    
    return -1;
}

/**
 * Decode BCD value from symbol array
 * @param symbols   Array of symbols (BCD_SYMBOL_ZERO or BCD_SYMBOL_ONE)
 * @param weights   Array of BCD weights for each position
 * @param count     Number of bits to decode
 */
static int decode_bcd_field(bcd_symbol_t *symbols, const int *weights, int count) {
    int value = 0;
    for (int i = 0; i < count; i++) {
        if (symbols[i] == BCD_SYMBOL_ONE) {
            value += weights[i];
        } else if (symbols[i] != BCD_SYMBOL_ZERO) {
            /* Unexpected symbol in data position */
            return -1;
        }
    }
    return value;
}

/**
 * Decode a complete frame into time values
 */
static bool decode_frame(bcd_decoder_t *dec, bcd_time_t *time_out,
                         bcd_frame_quality_t *quality_out) {
    bcd_symbol_t *f = dec->frame;
    
    /* Verify position markers are present */
    int markers_found = 0;
    for (int i = 0; i < NUM_P_MARKERS; i++) {
        if (f[P_MARKER_POSITIONS[i]] == BCD_SYMBOL_MARKER) {
            markers_found++;
        }
    }
    
    if (quality_out) {
        quality_out->markers_found = markers_found;
        quality_out->symbols_received = dec->frame_symbol_count;
        quality_out->avg_snr_db = dec->frame_snr_sum / 
                                  (dec->frame_symbol_count > 0 ? dec->frame_symbol_count : 1);
        quality_out->min_snr_db = dec->frame_snr_min;
        quality_out->bit_errors = 0;
    }
    
    /* Need at least 5 of 7 P markers for valid frame */
    if (markers_found < 5) {
        return false;
    }
    
    /* Decode minutes: seconds 1-8 */
    /* Bit weights: 40, 20, 10, 0, 8, 4, 2, 1 */
    static const int min_weights[] = {40, 20, 10, 0, 8, 4, 2, 1};
    int minutes = decode_bcd_field(&f[1], min_weights, 8);
    if (minutes < 0 || minutes > 59) {
        if (quality_out) quality_out->bit_errors++;
        return false;
    }
    
    /* Decode hours: seconds 12-18 */
    /* Bit weights: 20, 10, 0, 8, 4, 2, 1 (seconds 10-11 unused) */
    static const int hour_weights[] = {20, 10, 0, 8, 4, 2, 1};
    int hours = decode_bcd_field(&f[12], hour_weights, 7);
    if (hours < 0 || hours > 23) {
        if (quality_out) quality_out->bit_errors++;
        return false;
    }
    
    /* Decode day of year: seconds 22-28 (hundreds/tens), 30-33 (units) */
    /* Hundreds/tens: 200, 100, 0, 80, 40, 20, 10 */
    static const int doy_ht_weights[] = {200, 100, 0, 80, 40, 20, 10};
    int doy_ht = decode_bcd_field(&f[22], doy_ht_weights, 7);
    
    /* Units: 8, 4, 2, 1 */
    static const int doy_u_weights[] = {8, 4, 2, 1};
    int doy_u = decode_bcd_field(&f[30], doy_u_weights, 4);
    
    int day_of_year = doy_ht + doy_u;
    if (day_of_year < 1 || day_of_year > 366) {
        if (quality_out) quality_out->bit_errors++;
        return false;
    }
    
    /* Decode DUT1 sign: seconds 35-37 (+, +, -) */
    int dut1_sign = 0;
    if (f[35] == BCD_SYMBOL_ONE || f[36] == BCD_SYMBOL_ONE) {
        dut1_sign = 1;  /* Positive */
    }
    if (f[37] == BCD_SYMBOL_ONE) {
        dut1_sign = -1;  /* Negative (overrides positive) */
    }
    
    /* Decode DUT1 magnitude: seconds 40-43 (0.8, 0.4, 0.2, 0.1) */
    float dut1_value = 0.0f;
    if (f[40] == BCD_SYMBOL_ONE) dut1_value += 0.8f;
    if (f[41] == BCD_SYMBOL_ONE) dut1_value += 0.4f;
    if (f[42] == BCD_SYMBOL_ONE) dut1_value += 0.2f;
    if (f[43] == BCD_SYMBOL_ONE) dut1_value += 0.1f;
    
    /* Decode year: seconds 51-58 (80, 40, 20, 10, 8, 4, 2, 1) */
    static const int year_weights[] = {80, 40, 20, 10, 8, 4, 2, 1};
    int year = decode_bcd_field(&f[51], year_weights, 8);
    if (year < 0 || year > 99) {
        year = -1;  /* Invalid but don't fail decode */
    }
    
    /* Leap second warning: second 56 (bit 2 of year, but also LS warning) */
    /* DST warning: seconds 57-58 */
    bool leap_second_pending = false;  /* Would need separate bit check */
    bool dst_pending = false;
    
    /* Fill output structure */
    time_out->valid = true;
    time_out->hours = hours;
    time_out->minutes = minutes;
    time_out->day_of_year = day_of_year;
    time_out->year = year;
    time_out->dut1_sign = dut1_sign;
    time_out->dut1_value = dut1_value;
    time_out->leap_second_pending = leap_second_pending;
    time_out->dst_pending = dst_pending;
    time_out->decode_timestamp_ms = (uint64_t)dec->last_timestamp_ms;
    
    return true;
}

/**
 * Process a newly received symbol
 */
static void process_symbol(bcd_decoder_t *dec, bcd_symbol_t symbol,
                           float timestamp_ms, float width_ms, float snr_db) {
    /* Store in history buffer */
    store_symbol(dec, symbol, timestamp_ms, width_ms);
    
    /* Fire symbol callback */
    if (dec->symbol_callback) {
        dec->symbol_callback(symbol, timestamp_ms, width_ms, 
                            dec->symbol_callback_data);
    }
    
    /* State machine */
    switch (dec->sync_state) {
        case BCD_SYNC_SEARCHING:
            /* Look for P marker pattern */
            if (symbol == BCD_SYMBOL_MARKER) {
                int sync_offset = find_sync_pattern(dec);
                if (sync_offset >= 0) {
                    /* Found pattern - determine frame position */
                    /* The most recent symbol is a P marker */
                    /* Need to figure out which one (P0-P6) */
                    
                    /* For now, assume it's P0 and start fresh */
                    dec->sync_state = BCD_SYNC_CONFIRMING;
                    dec->frame_position = 0;
                    dec->sync_confidence = 0;
                    
                    /* Initialize frame buffer */
                    memset(dec->frame, BCD_SYMBOL_NONE, sizeof(dec->frame));
                    dec->frame[0] = symbol;
                    dec->frame_symbol_count = 1;
                    dec->frame_snr_sum = snr_db;
                    dec->frame_snr_min = snr_db;
                    
                    printf("[bcd_decoder] Sync pattern found, confirming...\n");
                }
            }
            break;
            
        case BCD_SYNC_CONFIRMING:
        case BCD_SYNC_LOCKED:
            /* Advance frame position */
            dec->frame_position = (dec->frame_position + 1) % BCD_FRAME_LENGTH;
            
            /* Store symbol in frame */
            dec->frame[dec->frame_position] = symbol;
            dec->frame_symbol_count++;
            dec->frame_snr_sum += snr_db;
            if (snr_db < dec->frame_snr_min) dec->frame_snr_min = snr_db;
            
            /* Check P marker alignment */
            bool expect_marker = is_p_marker_position(dec->frame_position);
            bool got_marker = (symbol == BCD_SYMBOL_MARKER);
            
            if (expect_marker && !got_marker) {
                dec->p_marker_miss_count++;
                if (dec->p_marker_miss_count >= 3) {
                    /* Lost sync */
                    printf("[bcd_decoder] Lost sync (missed %d P markers)\n",
                           dec->p_marker_miss_count);
                    dec->sync_state = BCD_SYNC_SEARCHING;
                    dec->frame_position = -1;
                    dec->sync_confidence = 0;
                }
            } else if (expect_marker && got_marker) {
                dec->p_marker_miss_count = 0;
                dec->sync_confidence++;
                
                if (dec->sync_state == BCD_SYNC_CONFIRMING && 
                    dec->sync_confidence >= BCD_SYNC_MARKERS_REQUIRED) {
                    dec->sync_state = BCD_SYNC_LOCKED;
                    printf("[bcd_decoder] Frame sync LOCKED\n");
                }
            }
            
            /* End of frame - attempt decode */
            if (dec->frame_position == 59) {
                bcd_time_t decoded_time;
                bcd_frame_quality_t quality;
                
                if (decode_frame(dec, &decoded_time, &quality)) {
                    dec->frames_decoded++;
                    dec->last_time = decoded_time;
                    
                    printf("[bcd_decoder] Decoded: %02d:%02d DOY=%03d Year=%02d "
                           "(DUT1=%+.1fs, markers=%d/%d)\n",
                           decoded_time.hours, decoded_time.minutes,
                           decoded_time.day_of_year, decoded_time.year,
                           decoded_time.dut1_sign * decoded_time.dut1_value,
                           quality.markers_found, NUM_P_MARKERS);
                    
                    if (dec->time_callback) {
                        dec->time_callback(&decoded_time, &quality, 
                                          dec->time_callback_data);
                    }
                } else {
                    dec->frames_failed++;
                    printf("[bcd_decoder] Frame decode failed\n");
                }
                
                /* Reset for next frame */
                memset(dec->frame, BCD_SYMBOL_NONE, sizeof(dec->frame));
                dec->frame_symbol_count = 0;
                dec->frame_snr_sum = 0;
                dec->frame_snr_min = 100.0f;
            }
            break;
    }
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

bcd_decoder_t *bcd_decoder_create(void) {
    bcd_decoder_t *dec = calloc(1, sizeof(bcd_decoder_t));
    if (!dec) return NULL;
    
    dec->sync_state = BCD_SYNC_SEARCHING;
    dec->frame_position = -1;
    dec->first_sample = true;
    dec->frame_snr_min = 100.0f;
    dec->adaptive_threshold = BCD_SNR_THRESHOLD_ON;
    
    /* Initialize symbol buffer */
    for (int i = 0; i < 120; i++) {
        dec->symbol_buffer[i] = BCD_SYMBOL_NONE;
    }
    
    /* Initialize frame buffer */
    for (int i = 0; i < BCD_FRAME_LENGTH; i++) {
        dec->frame[i] = BCD_SYMBOL_NONE;
    }
    
    printf("[bcd_decoder] Created, searching for sync...\n");
    return dec;
}

void bcd_decoder_destroy(bcd_decoder_t *dec) {
    if (dec) {
        free(dec);
    }
}

void bcd_decoder_process_sample(bcd_decoder_t *dec,
                                float timestamp_ms,
                                float envelope,
                                float snr_db,
                                bcd_status_t status) {
    if (!dec) return;
    
    /* Track timing */
    if (dec->first_sample) {
        dec->first_sample = false;
    }
    dec->last_timestamp_ms = timestamp_ms;
    
    /* Hysteresis pulse detection */
    bool signal_present = (status >= BCD_STATUS_PRESENT) || 
                          (snr_db >= BCD_SNR_THRESHOLD_ON);
    bool signal_gone = (status <= BCD_STATUS_WEAK) && 
                       (snr_db < BCD_SNR_THRESHOLD_OFF);
    
    if (!dec->in_pulse && signal_present) {
        /* Rising edge - start of pulse */
        dec->in_pulse = true;
        dec->pulse_start_ms = timestamp_ms;
        dec->pulse_snr_sum = snr_db;
        dec->pulse_sample_count = 1;
        
    } else if (dec->in_pulse && signal_gone) {
        /* Falling edge - end of pulse */
        dec->in_pulse = false;
        float pulse_width_ms = timestamp_ms - dec->pulse_start_ms;
        float avg_snr = dec->pulse_snr_sum / dec->pulse_sample_count;
        
        /* Classify the pulse */
        bcd_symbol_t symbol = classify_pulse(pulse_width_ms);
        
        if (symbol != BCD_SYMBOL_NONE) {
            process_symbol(dec, symbol, timestamp_ms, pulse_width_ms, avg_snr);
        }
        
    } else if (dec->in_pulse) {
        /* Still in pulse - accumulate */
        dec->pulse_snr_sum += snr_db;
        dec->pulse_sample_count++;
        
        /* Timeout check - if pulse exceeds max, force end */
        float current_width = timestamp_ms - dec->pulse_start_ms;
        if (current_width > BCD_PULSE_MAX_MS + 200) {
            /* Stuck high - reset */
            dec->in_pulse = false;
        }
    }
}

bool bcd_decoder_process_message(bcd_decoder_t *dec, const char *message) {
    if (!dec || !message) return false;
    
    /* Parse: BCD1,HH:MM:SS,timestamp_ms,envelope,snr_db,noise_floor_db,status */
    char time_str[16];
    float timestamp_ms, envelope, snr_db, noise_floor_db;
    char status_str[16];
    
    int parsed = sscanf(message, "BCD1,%15[^,],%f,%f,%f,%f,%15s",
                        time_str, &timestamp_ms, &envelope, 
                        &snr_db, &noise_floor_db, status_str);
    
    if (parsed < 6) {
        return false;  /* Invalid format */
    }
    
    /* Convert status string to enum */
    bcd_status_t status = BCD_STATUS_ABSENT;
    if (strcmp(status_str, "WEAK") == 0) {
        status = BCD_STATUS_WEAK;
    } else if (strcmp(status_str, "PRESENT") == 0) {
        status = BCD_STATUS_PRESENT;
    } else if (strcmp(status_str, "STRONG") == 0) {
        status = BCD_STATUS_STRONG;
    }
    
    /* Process the sample */
    bcd_decoder_process_sample(dec, timestamp_ms, envelope, snr_db, status);
    return true;
}

void bcd_decoder_set_time_callback(bcd_decoder_t *dec,
                                   bcd_time_callback_fn callback,
                                   void *user_data) {
    if (!dec) return;
    dec->time_callback = callback;
    dec->time_callback_data = user_data;
}

void bcd_decoder_set_symbol_callback(bcd_decoder_t *dec,
                                     bcd_symbol_callback_fn callback,
                                     void *user_data) {
    if (!dec) return;
    dec->symbol_callback = callback;
    dec->symbol_callback_data = user_data;
}

void bcd_decoder_reset(bcd_decoder_t *dec) {
    if (!dec) return;
    
    dec->sync_state = BCD_SYNC_SEARCHING;
    dec->frame_position = -1;
    dec->in_pulse = false;
    dec->sync_confidence = 0;
    dec->p_marker_miss_count = 0;
    dec->symbol_write_idx = 0;
    dec->symbol_count = 0;
    
    for (int i = 0; i < 120; i++) {
        dec->symbol_buffer[i] = BCD_SYMBOL_NONE;
    }
    for (int i = 0; i < BCD_FRAME_LENGTH; i++) {
        dec->frame[i] = BCD_SYMBOL_NONE;
    }
    
    printf("[bcd_decoder] Reset, searching for sync...\n");
}

bcd_sync_state_t bcd_decoder_get_sync_state(bcd_decoder_t *dec) {
    return dec ? dec->sync_state : BCD_SYNC_SEARCHING;
}

int bcd_decoder_get_frame_position(bcd_decoder_t *dec) {
    return dec ? dec->frame_position : -1;
}

const bcd_time_t *bcd_decoder_get_last_time(bcd_decoder_t *dec) {
    return dec ? &dec->last_time : NULL;
}

void bcd_decoder_get_stats(bcd_decoder_t *dec,
                           uint32_t *frames_decoded,
                           uint32_t *frames_failed,
                           uint32_t *total_symbols) {
    if (!dec) return;
    if (frames_decoded) *frames_decoded = dec->frames_decoded;
    if (frames_failed) *frames_failed = dec->frames_failed;
    if (total_symbols) *total_symbols = dec->total_symbols;
}

bool bcd_decoder_is_in_pulse(bcd_decoder_t *dec) {
    return dec ? dec->in_pulse : false;
}

void bcd_decoder_print_status(bcd_decoder_t *dec) {
    if (!dec) return;
    
    const char *sync_str;
    switch (dec->sync_state) {
        case BCD_SYNC_SEARCHING:  sync_str = "SEARCHING";  break;
        case BCD_SYNC_CONFIRMING: sync_str = "CONFIRMING"; break;
        case BCD_SYNC_LOCKED:     sync_str = "LOCKED";     break;
        default: sync_str = "UNKNOWN";
    }
    
    printf("[bcd_decoder] Sync: %s, Frame pos: %d, Symbols: %u, "
           "Decoded: %u, Failed: %u\n",
           sync_str, dec->frame_position, dec->total_symbols,
           dec->frames_decoded, dec->frames_failed);
    
    if (dec->last_time.valid) {
        printf("[bcd_decoder] Last time: %02d:%02d DOY=%03d Year=%02d\n",
               dec->last_time.hours, dec->last_time.minutes,
               dec->last_time.day_of_year, dec->last_time.year);
    }
}
