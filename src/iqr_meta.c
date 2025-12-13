/**
 * @file iqr_meta.c
 * @brief Metadata file implementation
 *
 * Simple key=value format, one per line. Easy to read/parse.
 */

#include "iqr_meta.h"
#include "ntp_time.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Get .meta filename from .iqr filename */
static void get_meta_filename(const char *iqr_filename, char *meta_filename, size_t len) {
    strncpy(meta_filename, iqr_filename, len - 6);
    meta_filename[len - 6] = '\0';
    
    /* Replace .iqr extension with .meta */
    char *ext = strrchr(meta_filename, '.');
    if (ext && (strcmp(ext, ".iqr") == 0 || strcmp(ext, ".IQR") == 0)) {
        strcpy(ext, ".meta");
    } else {
        strncat(meta_filename, ".meta", len - strlen(meta_filename) - 1);
    }
}

int iqr_meta_init_time(iqr_meta_t *meta, const char *ntp_server) {
    if (!meta) return -1;
    
    const char *server = ntp_server ? ntp_server : "time.nist.gov";
    strncpy(meta->ntp_server, server, sizeof(meta->ntp_server) - 1);
    meta->ntp_server[sizeof(meta->ntp_server) - 1] = '\0';
    
    ntp_time_t t = ntp_get_utc(server, 3000);
    if (!t.valid) {
        fprintf(stderr, "[META] NTP query failed, using system time\n");
        /* Fallback to system time */
        struct timespec ts;
        timespec_get(&ts, TIME_UTC);
        meta->start_time_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
        
        time_t now = ts.tv_sec;
        struct tm *utc = gmtime(&now);
        snprintf(meta->start_time_iso, sizeof(meta->start_time_iso),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                 utc->tm_hour, utc->tm_min, utc->tm_sec,
                 (int)(ts.tv_nsec / 1000000));
        meta->start_second = utc->tm_sec;
        strcpy(meta->ntp_server, "(system clock)");
    } else {
        meta->start_time_us = (int64_t)(t.unix_time * 1000000.0);
        ntp_format_iso(&t, meta->start_time_iso, sizeof(meta->start_time_iso));
        meta->start_second = (int)(t.unix_seconds % 60);
    }
    
    /* Calculate offset to next minute marker */
    int ms_in_sec = (int)((meta->start_time_us % 1000000) / 1000);
    meta->offset_to_next_minute = (60 - meta->start_second) - (ms_in_sec / 1000.0);
    if (meta->offset_to_next_minute < 0) meta->offset_to_next_minute += 60.0;
    
    meta->recording_complete = false;
    meta->end_time_us = 0;
    meta->end_time_iso[0] = '\0';
    meta->sample_count = 0;
    meta->duration_sec = 0.0;
    
    return 0;
}

int iqr_meta_write_start(const char *iqr_filename, const iqr_meta_t *meta) {
    if (!iqr_filename || !meta) return -1;
    
    char meta_filename[512];
    get_meta_filename(iqr_filename, meta_filename, sizeof(meta_filename));
    
    FILE *f = fopen(meta_filename, "w");
    if (!f) {
        fprintf(stderr, "[META] Failed to create %s\n", meta_filename);
        return -1;
    }
    
    fprintf(f, "# Phoenix SDR Recording Metadata\n");
    fprintf(f, "# Created at recording start, updated at end\n");
    fprintf(f, "\n");
    
    fprintf(f, "[recording]\n");
    fprintf(f, "sample_rate_hz = %.0f\n", meta->sample_rate_hz);
    fprintf(f, "center_freq_hz = %.0f\n", meta->center_freq_hz);
    fprintf(f, "bandwidth_khz = %u\n", meta->bandwidth_khz);
    fprintf(f, "gain_reduction = %d\n", meta->gain_reduction);
    fprintf(f, "lna_state = %u\n", meta->lna_state);
    fprintf(f, "\n");
    
    fprintf(f, "[timing]\n");
    fprintf(f, "ntp_server = %s\n", meta->ntp_server);
    fprintf(f, "start_time_us = %lld\n", (long long)meta->start_time_us);
    fprintf(f, "start_time_utc = %s\n", meta->start_time_iso);
    fprintf(f, "start_second = %d\n", meta->start_second);
    fprintf(f, "offset_to_next_minute = %.6f\n", meta->offset_to_next_minute);
    fprintf(f, "\n");
    
    fprintf(f, "[status]\n");
    fprintf(f, "recording_complete = false\n");
    fprintf(f, "sample_count = 0\n");
    fprintf(f, "duration_sec = 0.0\n");
    fprintf(f, "end_time_us = 0\n");
    fprintf(f, "end_time_utc = \n");
    
    fclose(f);
    fprintf(stderr, "[META] Created %s\n", meta_filename);
    return 0;
}

int iqr_meta_write_end(const char *iqr_filename, const iqr_meta_t *meta) {
    if (!iqr_filename || !meta) return -1;
    
    char meta_filename[512];
    get_meta_filename(iqr_filename, meta_filename, sizeof(meta_filename));
    
    FILE *f = fopen(meta_filename, "w");
    if (!f) {
        fprintf(stderr, "[META] Failed to update %s\n", meta_filename);
        return -1;
    }
    
    fprintf(f, "# Phoenix SDR Recording Metadata\n");
    fprintf(f, "# Recording complete\n");
    fprintf(f, "\n");
    
    fprintf(f, "[recording]\n");
    fprintf(f, "sample_rate_hz = %.0f\n", meta->sample_rate_hz);
    fprintf(f, "center_freq_hz = %.0f\n", meta->center_freq_hz);
    fprintf(f, "bandwidth_khz = %u\n", meta->bandwidth_khz);
    fprintf(f, "gain_reduction = %d\n", meta->gain_reduction);
    fprintf(f, "lna_state = %u\n", meta->lna_state);
    fprintf(f, "\n");
    
    fprintf(f, "[timing]\n");
    fprintf(f, "ntp_server = %s\n", meta->ntp_server);
    fprintf(f, "start_time_us = %lld\n", (long long)meta->start_time_us);
    fprintf(f, "start_time_utc = %s\n", meta->start_time_iso);
    fprintf(f, "start_second = %d\n", meta->start_second);
    fprintf(f, "offset_to_next_minute = %.6f\n", meta->offset_to_next_minute);
    fprintf(f, "\n");
    
    fprintf(f, "[status]\n");
    fprintf(f, "recording_complete = true\n");
    fprintf(f, "sample_count = %llu\n", (unsigned long long)meta->sample_count);
    fprintf(f, "duration_sec = %.6f\n", meta->duration_sec);
    fprintf(f, "end_time_us = %lld\n", (long long)meta->end_time_us);
    fprintf(f, "end_time_utc = %s\n", meta->end_time_iso);
    
    fclose(f);
    fprintf(stderr, "[META] Updated %s (recording complete)\n", meta_filename);
    return 0;
}

int iqr_meta_read(const char *iqr_filename, iqr_meta_t *meta) {
    if (!iqr_filename || !meta) return -1;
    
    char meta_filename[512];
    get_meta_filename(iqr_filename, meta_filename, sizeof(meta_filename));
    
    FILE *f = fopen(meta_filename, "r");
    if (!f) {
        return -1;  /* No metadata file */
    }
    
    memset(meta, 0, sizeof(*meta));
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and section headers */
        if (line[0] == '#' || line[0] == '[' || line[0] == '\n') continue;
        
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        
        /* Trim whitespace */
        while (*key == ' ') key++;
        char *end = key + strlen(key) - 1;
        while (end > key && *end == ' ') *end-- = '\0';
        
        while (*val == ' ') val++;
        end = val + strlen(val) - 1;
        while (end > val && (*end == ' ' || *end == '\n' || *end == '\r')) *end-- = '\0';
        
        /* Parse fields */
        if (strcmp(key, "sample_rate_hz") == 0) meta->sample_rate_hz = atof(val);
        else if (strcmp(key, "center_freq_hz") == 0) meta->center_freq_hz = atof(val);
        else if (strcmp(key, "bandwidth_khz") == 0) meta->bandwidth_khz = (uint32_t)atoi(val);
        else if (strcmp(key, "gain_reduction") == 0) meta->gain_reduction = atoi(val);
        else if (strcmp(key, "lna_state") == 0) meta->lna_state = (uint32_t)atoi(val);
        else if (strcmp(key, "ntp_server") == 0) strncpy(meta->ntp_server, val, sizeof(meta->ntp_server) - 1);
        else if (strcmp(key, "start_time_us") == 0) meta->start_time_us = atoll(val);
        else if (strcmp(key, "start_time_utc") == 0) strncpy(meta->start_time_iso, val, sizeof(meta->start_time_iso) - 1);
        else if (strcmp(key, "start_second") == 0) meta->start_second = atoi(val);
        else if (strcmp(key, "offset_to_next_minute") == 0) meta->offset_to_next_minute = atof(val);
        else if (strcmp(key, "recording_complete") == 0) meta->recording_complete = (strcmp(val, "true") == 0);
        else if (strcmp(key, "sample_count") == 0) meta->sample_count = (uint64_t)atoll(val);
        else if (strcmp(key, "duration_sec") == 0) meta->duration_sec = atof(val);
        else if (strcmp(key, "end_time_us") == 0) meta->end_time_us = atoll(val);
        else if (strcmp(key, "end_time_utc") == 0) strncpy(meta->end_time_iso, val, sizeof(meta->end_time_iso) - 1);
    }
    
    fclose(f);
    return 0;
}
