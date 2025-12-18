#include "tick_comb_filter.h"
#include <stdlib.h>
#include <string.h>

void comb_init(comb_filter_t *cf) {
    if (!cf || !cf->delay_line) return;
    memset(cf->delay_line, 0, COMB_STAGES * sizeof(float));
    cf->idx = 0;
    cf->alpha = 0.99f;  // ~100 second time constant
    cf->output = 0.0f;
}

comb_filter_t *comb_create(void) {
    comb_filter_t *cf = (comb_filter_t *)calloc(1, sizeof(comb_filter_t));
    if (!cf) return NULL;

    cf->delay_line = (float *)calloc(COMB_STAGES, sizeof(float));
    if (!cf->delay_line) {
        free(cf);
        return NULL;
    }

    comb_init(cf);
    return cf;
}

float comb_process(comb_filter_t *cf, float input) {
    if (!cf || !cf->delay_line) return 0.0f;

    float delayed = cf->delay_line[cf->idx];
    cf->output = cf->alpha * cf->output + (1.0f - cf->alpha) * (input + delayed) / 2.0f;
    cf->delay_line[cf->idx] = input;
    cf->idx = (cf->idx + 1) % COMB_STAGES;
    return cf->output;
}

void comb_reset(comb_filter_t *cf) {
    if (!cf || !cf->delay_line) return;
    memset(cf->delay_line, 0, COMB_STAGES * sizeof(float));
    cf->idx = 0;
    cf->output = 0.0f;
}

void comb_destroy(comb_filter_t *cf) {
    if (!cf) return;
    if (cf->delay_line) {
        free(cf->delay_line);
    }
    free(cf);
}
