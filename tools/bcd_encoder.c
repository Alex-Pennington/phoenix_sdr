/**
 * @file bcd_encoder.c
 * @brief WWV/WWVH BCD time code encoder implementation
 *
 * Reference: WWV_Test_Signal_Generator_Specification.md Section 4
 */

#include "bcd_encoder.h"
#include <stdlib.h>
#include <string.h>

#define SAMPLES_PER_SECOND 2000000  // 2 Msps
#define BCD_HIGH_LEVEL 0.18f        // 18% modulation depth
#define BCD_LOW_LEVEL 0.03f         // 3% modulation depth

struct bcd_encoder {
    int minute;         // 0-59
    int hour;           // 0-23
    int day_of_year;    // 1-366
    int year;           // 0-99
    bcd_symbol_t frame[60];  // Pre-computed symbol for each second
};

// WWV frame structure per spec Section 4.2
static void encode_frame(bcd_encoder_t *enc) {
    memset(enc->frame, BCD_ZERO, sizeof(enc->frame));

    // Frame markers (always position markers)
    enc->frame[0] = BCD_MARKER;   // FRM
    enc->frame[9] = BCD_MARKER;   // P1
    enc->frame[19] = BCD_MARKER;  // P2
    enc->frame[29] = BCD_MARKER;  // P3
    enc->frame[39] = BCD_MARKER;  // P4
    enc->frame[49] = BCD_MARKER;  // P5
    enc->frame[59] = BCD_MARKER;  // P0

    // Minutes (seconds 5-8: units, 10-12: tens)
    int min_units = enc->minute % 10;
    int min_tens = enc->minute / 10;
    if (min_units & 1) enc->frame[5] = BCD_ONE;
    if (min_units & 2) enc->frame[6] = BCD_ONE;
    if (min_units & 4) enc->frame[7] = BCD_ONE;
    if (min_units & 8) enc->frame[8] = BCD_ONE;
    if (min_tens & 1) enc->frame[10] = BCD_ONE;
    if (min_tens & 2) enc->frame[11] = BCD_ONE;
    if (min_tens & 4) enc->frame[12] = BCD_ONE;

    // Hours (seconds 15-18: units, 20-21: tens)
    int hour_units = enc->hour % 10;
    int hour_tens = enc->hour / 10;
    if (hour_units & 1) enc->frame[15] = BCD_ONE;
    if (hour_units & 2) enc->frame[16] = BCD_ONE;
    if (hour_units & 4) enc->frame[17] = BCD_ONE;
    if (hour_units & 8) enc->frame[18] = BCD_ONE;
    if (hour_tens & 1) enc->frame[20] = BCD_ONE;
    if (hour_tens & 2) enc->frame[21] = BCD_ONE;

    // Day of year (seconds 25-28: units, 30-33: tens, 35-36: hundreds)
    int day_units = enc->day_of_year % 10;
    int day_tens = (enc->day_of_year / 10) % 10;
    int day_hundreds = enc->day_of_year / 100;
    if (day_units & 1) enc->frame[25] = BCD_ONE;
    if (day_units & 2) enc->frame[26] = BCD_ONE;
    if (day_units & 4) enc->frame[27] = BCD_ONE;
    if (day_units & 8) enc->frame[28] = BCD_ONE;
    if (day_tens & 1) enc->frame[30] = BCD_ONE;
    if (day_tens & 2) enc->frame[31] = BCD_ONE;
    if (day_tens & 4) enc->frame[32] = BCD_ONE;
    if (day_tens & 8) enc->frame[33] = BCD_ONE;
    if (day_hundreds & 1) enc->frame[35] = BCD_ONE;
    if (day_hundreds & 2) enc->frame[36] = BCD_ONE;

    // Year (seconds 45-48: units, 53-54, 56-57: tens)
    int year_units = enc->year % 10;
    int year_tens = enc->year / 10;
    if (year_units & 1) enc->frame[45] = BCD_ONE;
    if (year_units & 2) enc->frame[46] = BCD_ONE;
    if (year_units & 4) enc->frame[47] = BCD_ONE;
    if (year_units & 8) enc->frame[48] = BCD_ONE;
    if (year_tens & 1) enc->frame[53] = BCD_ONE;
    if (year_tens & 2) enc->frame[54] = BCD_ONE;
    if (year_tens & 4) enc->frame[56] = BCD_ONE;
    if (year_tens & 8) enc->frame[57] = BCD_ONE;

    // DUT1 and DST bits remain zero (simplified implementation)
}

bcd_encoder_t *bcd_encoder_create(void) {
    bcd_encoder_t *enc = (bcd_encoder_t *)calloc(1, sizeof(bcd_encoder_t));
    if (!enc) {
        return NULL;
    }
    return enc;
}

void bcd_encoder_destroy(bcd_encoder_t *enc) {
    free(enc);
}

void bcd_encoder_set_time(bcd_encoder_t *enc, int minute, int hour, int day_of_year, int year) {
    if (!enc) {
        return;
    }
    enc->minute = minute;
    enc->hour = hour;
    enc->day_of_year = day_of_year;
    enc->year = year;
    encode_frame(enc);
}

bcd_symbol_t bcd_encoder_get_symbol(const bcd_encoder_t *enc, int second) {
    if (!enc || second < 0 || second >= 60) {
        return BCD_ZERO;
    }
    return enc->frame[second];
}

int bcd_get_pulse_width_ms(bcd_symbol_t symbol) {
    switch (symbol) {
        case BCD_ZERO:   return 200;
        case BCD_ONE:    return 500;
        case BCD_MARKER: return 800;
        default:         return 0;
    }
}

float bcd_encoder_get_modulation(const bcd_encoder_t *enc, int second, int sample_in_second) {
    if (!enc || second < 0 || second >= 60) {
        return BCD_LOW_LEVEL;
    }

    bcd_symbol_t symbol = enc->frame[second];
    int pulse_width_ms = bcd_get_pulse_width_ms(symbol);

    // Convert milliseconds to sample count at 2 Msps
    int pulse_width_samples = pulse_width_ms * 2000;  // 2000 samples per ms

    // First 30 ms is always low per spec
    if (sample_in_second < 60000) {  // 30 ms * 2000 samples/ms
        return BCD_LOW_LEVEL;
    }

    // High period after initial 30 ms
    int adjusted_sample = sample_in_second - 60000;
    if (adjusted_sample < pulse_width_samples) {
        return BCD_HIGH_LEVEL;
    }

    // Low for remainder of second
    return BCD_LOW_LEVEL;
}
