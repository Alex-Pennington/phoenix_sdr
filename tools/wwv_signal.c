/**
 * @file wwv_signal.c
 * @brief WWV/WWVH composite signal generator implementation
 *
 * Reference: WWV_Test_Signal_Generator_Specification.md
 */

#include "wwv_signal.h"
#include "oscillator.h"
#include "bcd_encoder.h"
#include "am_modulator.h"
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 2000000  // 2 Msps
#define SAMPLES_PER_SECOND 2000000
#define SAMPLES_PER_MINUTE 120000000ULL

// Guard zone boundaries (samples)
#define GUARD1_START 0
#define GUARD1_END 20000      // 10 ms
#define TICK_START 20000      // 10 ms
#define TICK_END 30000        // 15 ms (5 ms duration)
#define GUARD2_START 30000    // 15 ms
#define GUARD2_END 80000      // 40 ms
#define TONE_START 80000      // 40 ms

// Marker timing
#define MARKER_START 20000    // 10 ms
#define MARKER_END 1620000    // 810 ms (800 ms duration)
#define MARKER_GUARD2_END 1670000  // 835 ms

struct wwv_signal {
    // Timing state
    uint64_t sample_counter;
    int current_minute;
    int current_hour;
    int current_day;
    int current_year;
    wwv_station_t station;

    // Oscillators
    oscillator_t *tick_osc;      // 1000 Hz (WWV) or 1200 Hz (WWVH)
    oscillator_t *hour_osc;      // 1500 Hz
    oscillator_t *tone_500_osc;  // 500 Hz
    oscillator_t *tone_600_osc;  // 600 Hz
    oscillator_t *tone_440_osc;  // 440 Hz
    oscillator_t *bcd_osc;       // 100 Hz subcarrier

    // BCD encoder
    bcd_encoder_t *bcd_enc;

    // BCD lowpass filter state (2nd order Butterworth at 150 Hz)
    float bcd_lpf_x1, bcd_lpf_x2;  // Input history
    float bcd_lpf_y1, bcd_lpf_y2;  // Output history

    // Tone schedule (500 Hz minutes)
    bool tone_500_minutes[60];
    bool tone_600_minutes[60];
};

// WWV tone schedule per spec Section 6
static void init_tone_schedule(wwv_signal_t *sig) {
    memset(sig->tone_500_minutes, 0, sizeof(sig->tone_500_minutes));
    memset(sig->tone_600_minutes, 0, sizeof(sig->tone_600_minutes));

    // 500 Hz minutes
    int min_500[] = {4, 6, 12, 14, 16, 20, 22, 24, 26, 28, 32, 34, 36, 38, 40, 42, 52, 54, 56, 58};
    for (int i = 0; i < 20; i++) {
        sig->tone_500_minutes[min_500[i]] = true;
    }

    // 600 Hz minutes
    int min_600[] = {1, 3, 5, 7, 11, 13, 15, 17, 19, 21, 23, 25, 27, 31, 33, 35, 37, 39, 41, 53, 55, 57};
    for (int i = 0; i < 22; i++) {
        sig->tone_600_minutes[min_600[i]] = true;
    }

    // 440 Hz at minute 2 (handled separately in logic)
}

wwv_signal_t *wwv_signal_create(int start_minute, int start_hour, int start_day,
                                 int start_year, wwv_station_t station) {
    wwv_signal_t *sig = (wwv_signal_t *)calloc(1, sizeof(wwv_signal_t));
    if (!sig) {
        return NULL;
    }

    sig->sample_counter = 0;
    sig->current_minute = start_minute;
    sig->current_hour = start_hour;
    sig->current_day = start_day;
    sig->current_year = start_year;
    sig->station = station;

    // Create oscillators
    float tick_freq = (station == WWV_STATION_WWV) ? 1000.0f : 1200.0f;
    sig->tick_osc = oscillator_create(SAMPLE_RATE, tick_freq);
    sig->hour_osc = oscillator_create(SAMPLE_RATE, 1500.0f);
    sig->tone_500_osc = oscillator_create(SAMPLE_RATE, 500.0f);
    sig->tone_600_osc = oscillator_create(SAMPLE_RATE, 600.0f);
    sig->tone_440_osc = oscillator_create(SAMPLE_RATE, 440.0f);
    sig->bcd_osc = oscillator_create(SAMPLE_RATE, 100.0f);

    if (!sig->tick_osc || !sig->hour_osc || !sig->tone_500_osc ||
        !sig->tone_600_osc || !sig->tone_440_osc || !sig->bcd_osc) {
        wwv_signal_destroy(sig);
        return NULL;
    }

    // Create BCD encoder
    sig->bcd_enc = bcd_encoder_create();
    if (!sig->bcd_enc) {
        wwv_signal_destroy(sig);
        return NULL;
    }

    // Initialize BCD frame
    bcd_encoder_set_time(sig->bcd_enc, start_minute, start_hour, start_day, start_year);

    // Initialize BCD lowpass filter state
    sig->bcd_lpf_x1 = sig->bcd_lpf_x2 = 0.0f;
    sig->bcd_lpf_y1 = sig->bcd_lpf_y2 = 0.0f;

    // Initialize tone schedule
    init_tone_schedule(sig);

    return sig;
}

void wwv_signal_destroy(wwv_signal_t *sig) {
    if (!sig) {
        return;
    }

    oscillator_destroy(sig->tick_osc);
    oscillator_destroy(sig->hour_osc);
    oscillator_destroy(sig->tone_500_osc);
    oscillator_destroy(sig->tone_600_osc);
    oscillator_destroy(sig->tone_440_osc);
    oscillator_destroy(sig->bcd_osc);
    bcd_encoder_destroy(sig->bcd_enc);
    free(sig);
}

void wwv_signal_get_sample_int16(wwv_signal_t *sig, int16_t *i, int16_t *q) {
    if (!sig || !i || !q) {
        return;
    }

    // Calculate time position
    uint64_t sample_in_minute = sig->sample_counter % SAMPLES_PER_MINUTE;
    int second = (int)(sample_in_minute / SAMPLES_PER_SECOND);
    int sample_in_second = (int)(sample_in_minute % SAMPLES_PER_SECOND);

    // Check for minute boundary
    if (second == 0 && sample_in_second == 0 && sig->sample_counter > 0) {
        sig->current_minute++;
        if (sig->current_minute >= 60) {
            sig->current_minute = 0;
            sig->current_hour++;
            if (sig->current_hour >= 24) {
                sig->current_hour = 0;
                sig->current_day++;
                // Simplified: don't handle year rollover
            }
        }
        bcd_encoder_set_time(sig->bcd_enc, sig->current_minute, sig->current_hour,
                            sig->current_day, sig->current_year);
    }

    // Initialize composite signal
    float mod_i = 0.0f, mod_q = 0.0f;
    float total_modulation = 0.0f;

    // Determine pulse/tone state
    bool is_tick = false;
    bool is_marker = false;
    bool is_tone_active = false;
    bool is_silent = false;

    // Check for marker (second 0)
    if (second == 0) {
        if (sample_in_second >= MARKER_START && sample_in_second < MARKER_END) {
            is_marker = true;
        } else if (sample_in_second < MARKER_START ||
                  (sample_in_second >= MARKER_END && sample_in_second < MARKER_GUARD2_END)) {
            is_silent = true;
        } else {
            is_tone_active = true;
        }
    }
    // Check for normal tick (not seconds 29 or 59)
    else if (second != 29 && second != 59) {
        if (sample_in_second >= GUARD1_START && sample_in_second < GUARD1_END) {
            is_silent = true;
        } else if (sample_in_second >= TICK_START && sample_in_second < TICK_END) {
            is_tick = true;
        } else if (sample_in_second >= GUARD2_START && sample_in_second < GUARD2_END) {
            is_silent = true;
        } else {
            is_tone_active = true;
        }
    }
    // Silent seconds 29 and 59 (no tick, just tone)
    else {
        if (sample_in_second < GUARD2_END) {
            is_silent = true;
        } else {
            is_tone_active = true;
        }
    }

    // Generate pulse layer (tick or marker)
    if (is_tick) {
        float tick_i, tick_q;
        oscillator_get_iq(sig->tick_osc, &tick_i, &tick_q);
        total_modulation += 1.0f * tick_i;  // 100% modulation
    } else if (is_marker) {
        // Hour marker (minute 0) uses 1500 Hz, otherwise 1000 Hz
        if (sig->current_minute == 0) {
            float hour_i, hour_q;
            oscillator_get_iq(sig->hour_osc, &hour_i, &hour_q);
            total_modulation += 1.0f * hour_i;  // 100% modulation
        } else {
            float tick_i, tick_q;
            oscillator_get_iq(sig->tick_osc, &tick_i, &tick_q);
            total_modulation += 1.0f * tick_i;  // 100% modulation
        }
    }

    // Generate tone layer (outside guard zones)
    if (is_tone_active && !is_silent) {
        if (sig->current_minute == 2 && sig->station == WWV_STATION_WWV) {
            // 440 Hz tone
            float tone_i, tone_q;
            oscillator_get_iq(sig->tone_440_osc, &tone_i, &tone_q);
            total_modulation += 0.5f * tone_i;  // 50% modulation
        } else if (sig->tone_500_minutes[sig->current_minute]) {
            float tone_i, tone_q;
            oscillator_get_iq(sig->tone_500_osc, &tone_i, &tone_q);
            total_modulation += 0.5f * tone_i;  // 50% modulation
        } else if (sig->tone_600_minutes[sig->current_minute]) {
            float tone_i, tone_q;
            oscillator_get_iq(sig->tone_600_osc, &tone_i, &tone_q);
            total_modulation += 0.5f * tone_i;  // 50% modulation
        }
    }

    // Generate BCD subcarrier (always active)
    float bcd_i, bcd_q;
    oscillator_get_iq(sig->bcd_osc, &bcd_i, &bcd_q);
    float bcd_depth = bcd_encoder_get_modulation(sig->bcd_enc, second, sample_in_second);

    // Apply 2nd order Butterworth lowpass at 150 Hz to kill harmonics
    // Coefficients for fc=150Hz, fs=2MHz
    // This removes the 10th harmonic (1000 Hz) that bleeds into tick detector
    const float b0 = 1.7588e-7f;
    const float b1 = 3.5176e-7f;
    const float b2 = 1.7588e-7f;
    const float a1 = -1.9991f;
    const float a2 = 0.9991f;

    float bcd_raw = bcd_depth * bcd_i;
    float bcd_filt = b0 * bcd_raw + b1 * sig->bcd_lpf_x1 + b2 * sig->bcd_lpf_x2
                   - a1 * sig->bcd_lpf_y1 - a2 * sig->bcd_lpf_y2;

    sig->bcd_lpf_x2 = sig->bcd_lpf_x1;
    sig->bcd_lpf_x1 = bcd_raw;
    sig->bcd_lpf_y2 = sig->bcd_lpf_y1;
    sig->bcd_lpf_y1 = bcd_filt;

    total_modulation += bcd_filt;

    // Apply AM modulation to unit carrier (DC baseband)
    float envelope = 1.0f + total_modulation;
    if (envelope > 2.0f) envelope = 2.0f;  // Prevent over-modulation
    if (envelope < 0.0f) envelope = 0.0f;

    // Normalize to ±1.0 range
    float output = (envelope - 1.0f);  // Convert [0,2] to [-1,1]

    // Convert to int16_t (I and Q both carry same signal for DC baseband)
    *i = am_mod_float_to_int16(output);
    *q = 0;  // Pure real signal at DC

    // Advance sample counter
    sig->sample_counter++;
}

uint64_t wwv_signal_get_sample_count(const wwv_signal_t *sig) {
    return sig ? sig->sample_counter : 0;
}

void wwv_signal_reset_minute(wwv_signal_t *sig, int minute, int hour) {
    if (!sig) {
        return;
    }
    sig->current_minute = minute;
    sig->current_hour = hour;
    sig->sample_counter = 0;
    bcd_encoder_set_time(sig->bcd_enc, minute, hour, sig->current_day, sig->current_year);
}
