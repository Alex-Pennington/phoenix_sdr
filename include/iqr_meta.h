/**
 * @file iqr_meta.h
 * @brief Metadata file handling for IQR recordings
 *
 * Creates a .meta file alongside each .iqr recording with human-readable
 * info. Written at start, updated at end. Survives crashes.
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
    
    /* Timing - from NTP at recording start */
    int64_t  start_time_us;      /* Unix time, microseconds */
    char     start_time_iso[32]; /* ISO 8601 string */
    int      start_second;       /* Second within minute (0-59) */
    
    /* Updated at recording end */
    int64_t  end_time_us;        /* Unix time, microseconds */
    char     end_time_iso[32];   /* ISO 8601 string */
    uint64_t sample_count;
    double   duration_sec;
    
    /* Derived - for WWV analysis */
    double   offset_to_next_minute; /* Seconds from start to next minute marker */
    
    /* Status */
    bool     recording_complete; /* False until properly closed */
    char     ntp_server[64];     /* Server used for time */
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

/**
 * Initialize metadata with NTP time
 * Queries NTP server and populates timing fields
 *
 * @param meta          Metadata to initialize
 * @param ntp_server    NTP server (NULL for default)
 * @return 0 on success, -1 on NTP failure
 */
int iqr_meta_init_time(iqr_meta_t *meta, const char *ntp_server);

#endif /* IQR_META_H */
