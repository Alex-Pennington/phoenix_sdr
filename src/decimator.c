/**
 * @file decimator.c
 * @brief Multi-stage decimation implementation
 *
 * Converts 2 MSPS to 48 kHz using cascaded FIR filters.
 *
 * Decimation chain: 2M → 250k → 50k → 48k
 *   Stage 1: ÷8 with lowpass (cutoff ~100 kHz)
 *   Stage 2: ÷5 with lowpass (cutoff ~20 kHz)
 *   Stage 3: Rational resample 48/50 (polyphase)
 */

#include "decimator.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Define M_PI if not available (MinGW strict mode) */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*============================================================================
 * Constants
 *============================================================================*/

/* Stage 1: Decimate by 8 (2M → 250k)
 * Lowpass FIR, cutoff ~100 kHz at 2 MSPS
 * 31-tap filter designed for -60 dB stopband
 */
#define STAGE1_DECIM    8
#define STAGE1_TAPS     31

static const float stage1_coeffs[STAGE1_TAPS] = {
    -0.000452f, -0.001051f, -0.001892f, -0.002566f, -0.002423f,
    -0.000551f,  0.003824f,  0.010894f,  0.020219f,  0.030829f,
     0.041369f,  0.050373f,  0.056516f,  0.058893f,  0.057122f,
     0.051417f,  0.042531f,  0.031609f,  0.020058f,  0.009284f,
     0.000456f, -0.005761f, -0.009157f, -0.009895f, -0.008491f,
    -0.005700f, -0.002398f,  0.000502f,  0.002517f,  0.003462f,
     0.003403f
};

/* Stage 2: Decimate by 5 (250k → 50k)
 * Lowpass FIR, cutoff ~20 kHz at 250 kHz
 * 25-tap filter
 */
#define STAGE2_DECIM    5
#define STAGE2_TAPS     25

static const float stage2_coeffs[STAGE2_TAPS] = {
    -0.001205f, -0.003412f, -0.006018f, -0.005891f,  0.001243f,
     0.017842f,  0.043251f,  0.073182f,  0.100718f,  0.119209f,
     0.124512f,  0.119209f,  0.100718f,  0.073182f,  0.043251f,
     0.017842f,  0.001243f, -0.005891f, -0.006018f, -0.003412f,
    -0.001205f,  0.000000f,  0.000000f,  0.000000f,  0.000000f
};

/* Stage 3: Resample 48/50 (50k → 48k)
 * Polyphase filter bank with 48 phases
 * This is a rational resampler: output 48 samples for every 50 input
 */
#define STAGE3_UP       48
#define STAGE3_DOWN     50
#define STAGE3_TAPS_PER_PHASE  8
#define STAGE3_TOTAL_TAPS      (STAGE3_UP * STAGE3_TAPS_PER_PHASE)

/* Polyphase coefficients generated for 48/50 resampling
 * Lowpass at 0.96 * Nyquist to prevent aliasing
 */
static float stage3_polyphase[STAGE3_UP][STAGE3_TAPS_PER_PHASE];
static bool stage3_initialized = false;

/*============================================================================
 * Internal State
 *============================================================================*/

typedef struct {
    float i[64];  /* Enough for largest filter */
    float q[64];
    int pos;
    int count;
} filter_state_t;

struct decim_state {
    double input_rate;
    double output_rate;
    double actual_output_rate;

    /* Stage 1 state */
    filter_state_t stage1;
    int stage1_phase;

    /* Stage 2 state */
    filter_state_t stage2;
    int stage2_phase;

    /* Stage 3 state (polyphase resampler) */
    filter_state_t stage3;
    int stage3_in_phase;   /* 0 to 49 */
    int stage3_out_phase;  /* 0 to 47 */

    /* Intermediate buffers */
    decim_complex_t *buf1;  /* After stage 1 */
    decim_complex_t *buf2;  /* After stage 2 */
    size_t buf1_size;
    size_t buf2_size;
};

/*============================================================================
 * Error Strings
 *============================================================================*/

static const char *error_strings[] = {
    [DECIM_OK]              = "Success",
    [DECIM_ERR_ALLOC]       = "Memory allocation failed",
    [DECIM_ERR_INVALID_ARG] = "Invalid argument",
    [DECIM_ERR_BUFFER_FULL] = "Output buffer full"
};

const char* decim_strerror(decim_error_t err) {
    if (err < 0 || err > DECIM_ERR_BUFFER_FULL) {
        return "Unknown error";
    }
    return error_strings[err];
}

/*============================================================================
 * Polyphase Filter Initialization
 *============================================================================*/

static void init_polyphase_coeffs(void) {
    if (stage3_initialized) return;

    /* Generate windowed sinc lowpass filter
     * Cutoff at 0.48 (slightly below Nyquist for 48/50 ratio)
     */
    float sinc[STAGE3_TOTAL_TAPS];
    float cutoff = 0.48f;
    int center = STAGE3_TOTAL_TAPS / 2;

    for (int i = 0; i < STAGE3_TOTAL_TAPS; i++) {
        float x = (float)(i - center);
        if (fabsf(x) < 0.0001f) {
            sinc[i] = 2.0f * cutoff;
        } else {
            sinc[i] = sinf(2.0f * (float)M_PI * cutoff * x) / ((float)M_PI * x);
        }

        /* Blackman window */
        float w = 0.42f - 0.5f * cosf(2.0f * (float)M_PI * i / (STAGE3_TOTAL_TAPS - 1))
                       + 0.08f * cosf(4.0f * (float)M_PI * i / (STAGE3_TOTAL_TAPS - 1));
        sinc[i] *= w;
    }

    /* Normalize */
    float sum = 0.0f;
    for (int i = 0; i < STAGE3_TOTAL_TAPS; i++) {
        sum += sinc[i];
    }
    for (int i = 0; i < STAGE3_TOTAL_TAPS; i++) {
        sinc[i] /= sum;
    }

    /* Distribute into polyphase branches */
    /* Note: For decimation (not interpolation), we don't multiply by STAGE3_UP */
    for (int phase = 0; phase < STAGE3_UP; phase++) {
        for (int tap = 0; tap < STAGE3_TAPS_PER_PHASE; tap++) {
            int idx = tap * STAGE3_UP + phase;
            if (idx < STAGE3_TOTAL_TAPS) {
                stage3_polyphase[phase][tap] = sinc[idx];
            } else {
                stage3_polyphase[phase][tap] = 0.0f;
            }
        }
    }

    stage3_initialized = true;
}

/*============================================================================
 * Filter Operations
 *============================================================================*/

static void filter_state_init(filter_state_t *fs, int taps) {
    memset(fs->i, 0, sizeof(fs->i));
    memset(fs->q, 0, sizeof(fs->q));
    fs->pos = 0;
    fs->count = taps;
}

static inline void filter_push(filter_state_t *fs, float i, float q) {
    fs->i[fs->pos] = i;
    fs->q[fs->pos] = q;
    fs->pos = (fs->pos + 1) % fs->count;
}

static inline void filter_apply(const filter_state_t *fs,
                                const float *coeffs, int taps,
                                float *out_i, float *out_q) {
    float sum_i = 0.0f, sum_q = 0.0f;
    int pos = fs->pos;

    for (int i = 0; i < taps; i++) {
        pos = (pos - 1 + fs->count) % fs->count;
        sum_i += fs->i[pos] * coeffs[i];
        sum_q += fs->q[pos] * coeffs[i];
    }

    *out_i = sum_i;
    *out_q = sum_q;
}

/*============================================================================
 * API Implementation
 *============================================================================*/

decim_error_t decim_create(decim_state_t **state,
                           double input_rate,
                           double output_rate) {
    if (!state) return DECIM_ERR_INVALID_ARG;

    /* For now, only support 2M → 48k */
    if (input_rate != 2000000.0 || output_rate != 48000.0) {
        /* Could add more flexible rate support later */
        return DECIM_ERR_INVALID_ARG;
    }

    init_polyphase_coeffs();

    decim_state_t *s = calloc(1, sizeof(decim_state_t));
    if (!s) return DECIM_ERR_ALLOC;

    s->input_rate = input_rate;
    s->output_rate = output_rate;
    s->actual_output_rate = 48000.0;  /* Exact for 2M → 250k → 50k → 48k */

    /* Initialize filter states */
    filter_state_init(&s->stage1, STAGE1_TAPS);
    filter_state_init(&s->stage2, STAGE2_TAPS);
    filter_state_init(&s->stage3, STAGE3_TAPS_PER_PHASE);

    s->stage1_phase = 0;
    s->stage2_phase = 0;
    s->stage3_in_phase = 0;
    s->stage3_out_phase = 0;

    /* Allocate intermediate buffers
     * Stage 1 output: input_count / 8
     * Stage 2 output: stage1_output / 5
     */
    s->buf1_size = 65536 / STAGE1_DECIM + 64;
    s->buf2_size = s->buf1_size / STAGE2_DECIM + 64;

    s->buf1 = calloc(s->buf1_size, sizeof(decim_complex_t));
    s->buf2 = calloc(s->buf2_size, sizeof(decim_complex_t));

    if (!s->buf1 || !s->buf2) {
        free(s->buf1);
        free(s->buf2);
        free(s);
        return DECIM_ERR_ALLOC;
    }

    *state = s;
    return DECIM_OK;
}

void decim_destroy(decim_state_t *state) {
    if (!state) return;
    free(state->buf1);
    free(state->buf2);
    free(state);
}

void decim_reset(decim_state_t *state) {
    if (!state) return;

    filter_state_init(&state->stage1, STAGE1_TAPS);
    filter_state_init(&state->stage2, STAGE2_TAPS);
    filter_state_init(&state->stage3, STAGE3_TAPS_PER_PHASE);

    state->stage1_phase = 0;
    state->stage2_phase = 0;
    state->stage3_in_phase = 0;
    state->stage3_out_phase = 0;
}

double decim_get_ratio(const decim_state_t *state) {
    if (!state) return 0.0;
    return state->input_rate / state->output_rate;
}

size_t decim_estimate_output(const decim_state_t *state, size_t in_count) {
    if (!state) return 0;
    /* Total decimation: 8 * 5 * (50/48) = 41.667 */
    return (size_t)(in_count / 41.0) + 10;  /* +10 for filter delay */
}

double decim_get_output_rate(const decim_state_t *state) {
    if (!state) return 0.0;
    return state->actual_output_rate;
}

decim_error_t decim_process_int16(
    decim_state_t *state,
    const int16_t *xi,
    const int16_t *xq,
    size_t in_count,
    decim_complex_t *out,
    size_t out_max,
    size_t *out_count
) {
    if (!state || !xi || !xq || !out || !out_count) {
        return DECIM_ERR_INVALID_ARG;
    }

    const float scale = 1.0f / 32768.0f;
    size_t stage1_out = 0;
    size_t stage2_out = 0;
    size_t final_out = 0;

    /* Stage 1: Decimate by 8 */
    for (size_t i = 0; i < in_count; i++) {
        float fi = xi[i] * scale;
        float fq = xq[i] * scale;

        filter_push(&state->stage1, fi, fq);
        state->stage1_phase++;

        if (state->stage1_phase >= STAGE1_DECIM) {
            state->stage1_phase = 0;

            float oi, oq;
            filter_apply(&state->stage1, stage1_coeffs, STAGE1_TAPS, &oi, &oq);

            if (stage1_out < state->buf1_size) {
                state->buf1[stage1_out].i = oi;
                state->buf1[stage1_out].q = oq;
                stage1_out++;
            }
        }
    }

    /* Stage 2: Decimate by 5 */
    for (size_t i = 0; i < stage1_out; i++) {
        filter_push(&state->stage2, state->buf1[i].i, state->buf1[i].q);
        state->stage2_phase++;

        if (state->stage2_phase >= STAGE2_DECIM) {
            state->stage2_phase = 0;

            float oi, oq;
            filter_apply(&state->stage2, stage2_coeffs, STAGE2_TAPS, &oi, &oq);

            if (stage2_out < state->buf2_size) {
                state->buf2[stage2_out].i = oi;
                state->buf2[stage2_out].q = oq;
                stage2_out++;
            }
        }
    }

    /* Stage 3: Resample 50k → 48k using polyphase filter */
    for (size_t i = 0; i < stage2_out; i++) {
        filter_push(&state->stage3, state->buf2[i].i, state->buf2[i].q);

        /* For each input sample, we might produce 0 or 1 output samples
         * Ratio is 48/50, so we output slightly less than we input
         */
        while (state->stage3_out_phase * STAGE3_DOWN <
               (state->stage3_in_phase + 1) * STAGE3_UP) {

            if (final_out >= out_max) {
                *out_count = final_out;
                return DECIM_ERR_BUFFER_FULL;
            }

            int phase = state->stage3_out_phase % STAGE3_UP;
            float oi, oq;
            filter_apply(&state->stage3, stage3_polyphase[phase],
                        STAGE3_TAPS_PER_PHASE, &oi, &oq);

            out[final_out].i = oi;
            out[final_out].q = oq;
            final_out++;

            state->stage3_out_phase++;
        }

        state->stage3_in_phase++;

        /* Reset phase counters to prevent overflow */
        if (state->stage3_in_phase >= STAGE3_DOWN &&
            state->stage3_out_phase >= STAGE3_UP) {
            state->stage3_in_phase -= STAGE3_DOWN;
            state->stage3_out_phase -= STAGE3_UP;
        }
    }

    *out_count = final_out;
    return DECIM_OK;
}

decim_error_t decim_process_float(
    decim_state_t *state,
    const decim_complex_t *in,
    size_t in_count,
    decim_complex_t *out,
    size_t out_max,
    size_t *out_count
) {
    if (!state || !in || !out || !out_count) {
        return DECIM_ERR_INVALID_ARG;
    }

    /* Convert to temporary int16 arrays and use main path
     * (Not optimal, but keeps code simple for now)
     */
    int16_t *xi = malloc(in_count * sizeof(int16_t));
    int16_t *xq = malloc(in_count * sizeof(int16_t));

    if (!xi || !xq) {
        free(xi);
        free(xq);
        return DECIM_ERR_ALLOC;
    }

    for (size_t i = 0; i < in_count; i++) {
        xi[i] = (int16_t)(in[i].i * 32767.0f);
        xq[i] = (int16_t)(in[i].q * 32767.0f);
    }

    decim_error_t err = decim_process_int16(state, xi, xq, in_count,
                                            out, out_max, out_count);

    free(xi);
    free(xq);
    return err;
}
