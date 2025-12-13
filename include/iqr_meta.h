/**
 * @file iqr_meta.h
 * @brief Metadata file handling for IQR recordings
 *
 * Creates a .meta file alongside each .iqr recording with human-readable
 * info. GPS PPS is the primary time source. Written at start, updated at end.
 */

#ifndef IQR_META_H
#define IQR_META_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Metadata structure for IQR recordings
 */
typedef struct {
    /* Recording parameters */
    double   sample_rate_hz;
    double   center_freq_hz;
    uint32_t bandwidth_khz;
    int32_t  gain_reduction;
    uint32_t lna_state;
    
    /* Primary timing (GPS preferred, system clock fallback) */
    int64_t  start_time_us;      /* Unix time, microseconds */
    char     start_time_iso[32]; /* ISO 8601 string */
    int      start_second;       /* Second within minute (0-59) */
    double   offset_to_next_minute; /* Seconds from start to next minute marker */
    
    /* GPS timing details (when available) */
    bool     gps_valid;          /* GPS had fix at start */
    char     gps_time_iso[32];   /* GPS UTC time ISO 8601 */
    int64_t  gps_time_us;        /* GPS Unix time, microseconds */
    int      gps_satellites;     /* Number of satellites */
    double   gps_pc_offset_ms;   /* PC time - GPS time (ms) */
    char     gps_port[32];       /* COM port used */
    double   gps_latency_ms;     /* Serial latency compensation */
    
    /* Updated at recording end */
    int64_t  end_time_us;        /* Unix time, microseconds */
    char     end_time_iso[32];   /* ISO 8601 string */
    uint64_t sample_count;
    double   duration_sec;
    
    /* Status */
    bool     recording_complete; /* False until properly closed */
} iqr_meta_t;

/**
 * Write metadata file at recording start
 * Creates filename.meta alongside filename.iqr
 *
 * @param iqr_filename  The .iqr filename (we derive .meta from it)
 * @param meta          Metadata to write
 * @return 0 on success, -1 on error
 */
int iqr_meta_write_start(const char *iqr_filename, const iqr_meta_t *meta);

/**
 * Update metadata file at recording end
 * Updates sample_count, duration, end_time, recording_complete
 *
 * @param iqr_filename  The .iqr filename
 * @param meta          Updated metadata
 * @return 0 on success, -1 on error
 */
int iqr_meta_write_end(const char *iqr_filename, const iqr_meta_t *meta);

/**
 * Read metadata from file
 *
 * @param iqr_filename  The .iqr filename
 * @param meta          Receives loaded metadata
 * @return 0 on success, -1 on error
 */
int iqr_meta_read(const char *iqr_filename, iqr_meta_t *meta);

#endif /* IQR_META_H */
