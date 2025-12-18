#ifndef TICK_COMB_FILTER_H
#define TICK_COMB_FILTER_H

#ifdef __cplusplus
extern "C" {
#endif

#define COMB_STAGES 50000  // 1 second at 50 kHz = 200 KB

typedef struct {
    float *delay_line;
    int idx;
    float alpha;
    float output;
} comb_filter_t;

/**
 * Initialize comb filter
 */
void comb_init(comb_filter_t *cf);

/**
 * Create comb filter (allocates delay line)
 */
comb_filter_t *comb_create(void);

/**
 * Process one sample
 */
float comb_process(comb_filter_t *cf, float input);

/**
 * Reset comb filter state
 */
void comb_reset(comb_filter_t *cf);

/**
 * Destroy comb filter (frees delay line)
 */
void comb_destroy(comb_filter_t *cf);

#ifdef __cplusplus
}
#endif

#endif // TICK_COMB_FILTER_H
