/**
 * @file marker_correlator.h
 * @brief Correlates fast and slow marker detector outputs
 */

#ifndef MARKER_CORRELATOR_H
#define MARKER_CORRELATOR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MARKER_CONF_NONE = 0,
    MARKER_CONF_LOW,        /* One path triggered */
    MARKER_CONF_HIGH        /* Both paths agree */
} marker_confidence_t;

typedef struct {
    int marker_number;
    float timestamp_ms;
    float duration_ms;          /* From fast path */
    float energy;               /* From slow path */
    float snr_db;               /* From slow path */
    marker_confidence_t confidence;
} correlated_marker_t;

typedef struct marker_correlator marker_correlator_t;

typedef void (*correlated_marker_callback_fn)(const correlated_marker_t *marker, void *user_data);

marker_correlator_t *marker_correlator_create(const char *csv_path);
void marker_correlator_destroy(marker_correlator_t *mc);

/* Called when fast path detects marker end */
void marker_correlator_fast_event(marker_correlator_t *mc,
                                   float timestamp_ms,
                                   float duration_ms);

/* Called every slow path frame */
void marker_correlator_slow_frame(marker_correlator_t *mc,
                                   float timestamp_ms,
                                   float energy,
                                   float snr_db,
                                   bool above_threshold);

void marker_correlator_set_callback(marker_correlator_t *mc,
                                     correlated_marker_callback_fn cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* MARKER_CORRELATOR_H */
