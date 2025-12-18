/**
 * @file bcd_decoder.c
 * @brief WWV BCD Pulse Detector Implementation (Modem Side)
 *
 * Simple pulse detector that:
 * 1. Detects pulses using hysteresis thresholding
 * 2. Measures pulse width
 * 3. Classifies as 0/1/P symbol
 * 4. Fires callback with timestamp
 *
 * Frame sync and BCD decode happen in the controller.
 */

#include "bcd_decoder.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*============================================================================
 * Internal State Structure
 *============================================================================*/

struct bcd_decoder {
    /* Pulse detection state */
    bool in_pulse;                  /* Currently detecting a pulse */
    float pulse_start_ms;           /* Timestamp when pulse started */
    float pulse_snr_sum;            /* Sum of SNR during pulse (for averaging) */
    int pulse_sample_count;         /* Samples in current pulse */
    
    /* Lockout to prevent duplicate detections */
    float last_symbol_time_ms;      /* When last symbol was detected */
    bool in_lockout;                /* Currently in lockout period */
    
    /* Timing */
    float last_timestamp_ms;
    bool first_sample;
    
    /* Statistics */
    uint32_t total_symbols;
    uint32_t rejected_lockout;      /* Symbols rejected due to lockout */
    
    /* Callback */
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

/*============================================================================
 * Public API Implementation
 *============================================================================*/

bcd_decoder_t *bcd_decoder_create(void) {
    bcd_decoder_t *dec = calloc(1, sizeof(bcd_decoder_t));
    if (!dec) return NULL;
    
    dec->first_sample = true;
    
    return dec;
}

void bcd_decoder_destroy(bcd_decoder_t *dec) {
    free(dec);
}

void bcd_decoder_process_sample(bcd_decoder_t *dec,
                                float timestamp_ms,
                                float envelope,
                                float snr_db,
                                bcd_status_t status) {
    if (!dec) return;
    
    (void)envelope;  /* Not used directly - we use SNR/status */
    
    /* Track timing */
    if (dec->first_sample) {
        dec->first_sample = false;
        dec->last_symbol_time_ms = -1000;  /* Allow first symbol */
    }
    dec->last_timestamp_ms = timestamp_ms;
    
    /* Check/update lockout state */
    if (dec->in_lockout) {
        float since_last = timestamp_ms - dec->last_symbol_time_ms;
        if (since_last >= BCD_SYMBOL_LOCKOUT_MS) {
            dec->in_lockout = false;
        }
    }
    
    /* Hysteresis pulse detection */
    bool signal_present = (status >= BCD_STATUS_PRESENT) || 
                          (snr_db >= BCD_SNR_THRESHOLD_ON);
    bool signal_gone = (status <= BCD_STATUS_WEAK) && 
                       (snr_db < BCD_SNR_THRESHOLD_OFF);
    
    if (!dec->in_pulse && signal_present && !dec->in_lockout) {
        /* Rising edge - start of pulse (only if not in lockout) */
        dec->in_pulse = true;
        dec->pulse_start_ms = timestamp_ms;
        dec->pulse_snr_sum = snr_db;
        dec->pulse_sample_count = 1;
        
    } else if (dec->in_pulse && signal_gone) {
        /* Falling edge - end of pulse */
        dec->in_pulse = false;
        float pulse_width_ms = timestamp_ms - dec->pulse_start_ms;
        
        /* Classify the pulse */
        bcd_symbol_t symbol = classify_pulse(pulse_width_ms);
        
        if (symbol != BCD_SYMBOL_NONE) {
            dec->total_symbols++;
            dec->last_symbol_time_ms = timestamp_ms;
            dec->in_lockout = true;  /* Start lockout period */
            
            /* Fire callback */
            if (dec->symbol_callback) {
                dec->symbol_callback(symbol, timestamp_ms, pulse_width_ms,
                                    dec->symbol_callback_data);
            }
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
    } else if (!dec->in_pulse && signal_present && dec->in_lockout) {
        /* Would have started pulse but in lockout - track for debugging */
        dec->rejected_lockout++;
    }
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
    
    dec->in_pulse = false;
    dec->first_sample = true;
    dec->total_symbols = 0;
}

uint32_t bcd_decoder_get_symbol_count(bcd_decoder_t *dec) {
    return dec ? dec->total_symbols : 0;
}

bool bcd_decoder_is_in_pulse(bcd_decoder_t *dec) {
    return dec ? dec->in_pulse : false;
}
