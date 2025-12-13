/**
 * @file iqr_meta.c
 * @brief Metadata file implementation
 *
 * Simple key=value format, one per line. GPS PPS is primary time source.
 */

#include "iqr_meta.h"
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
    fprintf(f, "time_source = %s\n", meta->gps_valid ? "GPS_PPS" : "system_clock");
    fprintf(f, "start_time_us = %lld\n", (long long)meta->start_time_us);
    fprintf(f, "start_time_utc = %s\n", meta->start_time_iso);
    fprintf(f, "start_second = %d\n", meta->start_second);
    fprintf(f, "offset_to_next_minute = %.6f\n", meta->offset_to_next_minute);
    fprintf(f, "\n");
    
    if (meta->gps_valid) {
        fprintf(f, "[gps]\n");
        fprintf(f, "gps_time_utc = %s\n", meta->gps_time_iso);
        fprintf(f, "gps_time_us = %lld\n", (long long)meta->gps_time_us);
        fprintf(f, "satellites = %d\n", meta->gps_satellites);
        fprintf(f, "pc_offset_ms = %.1f\n", meta->gps_pc_offset_ms);
        fprintf(f, "port = %s\n", meta->gps_port);
        fprintf(f, "latency_ms = %.1f\n", meta->gps_latency_ms);
        fprintf(f, "\n");
    }
    
    fprintf(f, "[status]\n");
    fprintf(f, "recording_complete = false\n");
    fprintf(f, "sample_count = 0\n");
    fprintf(f, "duration_sec = 0.0\n");
    fprintf(f, "end_time_us = 0\n");
    fprintf(f, "end_time_utc = \n");
    
    fclose(f);
    printf("[META] Created %s\n", meta_filename);
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
    fprintf(f, "time_source = %s\n", meta->gps_valid ? "GPS_PPS" : "system_clock");
    fprintf(f, "start_time_us = %lld\n", (long long)meta->start_time_us);
    fprintf(f, "start_time_utc = %s\n", meta->start_time_iso);
    fprintf(f, "start_second = %d\n", meta->start_second);
    fprintf(f, "offset_to_next_minute = %.6f\n", meta->offset_to_next_minute);
    fprintf(f, "\n");
    
    if (meta->gps_valid) {
        fprintf(f, "[gps]\n");
        fprintf(f, "gps_time_utc = %s\n", meta->gps_time_iso);
        fprintf(f, "gps_time_us = %lld\n", (long long)meta->gps_time_us);
        fprintf(f, "satellites = %d\n", meta->gps_satellites);
        fprintf(f, "pc_offset_ms = %.1f\n", meta->gps_pc_offset_ms);
        fprintf(f, "port = %s\n", meta->gps_port);
        fprintf(f, "latency_ms = %.1f\n", meta->gps_latency_ms);
        fprintf(f, "\n");
    }
    
    fprintf(f, "[status]\n");
    fprintf(f, "recording_complete = true\n");
    fprintf(f, "sample_count = %llu\n", (unsigned long long)meta->sample_count);
    fprintf(f, "duration_sec = %.6f\n", meta->duration_sec);
    fprintf(f, "end_time_us = %lld\n", (long long)meta->end_time_us);
    fprintf(f, "end_time_utc = %s\n", meta->end_time_iso);
    
    fclose(f);
    printf("[META] Updated %s (recording complete)\n", meta_filename);
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
        else if (strcmp(key, "time_source") == 0) meta->gps_valid = (strcmp(val, "GPS_PPS") == 0);
        else if (strcmp(key, "start_time_us") == 0) meta->start_time_us = atoll(val);
        else if (strcmp(key, "start_time_utc") == 0) strncpy(meta->start_time_iso, val, sizeof(meta->start_time_iso) - 1);
        else if (strcmp(key, "start_second") == 0) meta->start_second = atoi(val);
        else if (strcmp(key, "offset_to_next_minute") == 0) meta->offset_to_next_minute = atof(val);
        else if (strcmp(key, "recording_complete") == 0) meta->recording_complete = (strcmp(val, "true") == 0);
        else if (strcmp(key, "sample_count") == 0) meta->sample_count = (uint64_t)atoll(val);
        else if (strcmp(key, "duration_sec") == 0) meta->duration_sec = atof(val);
        else if (strcmp(key, "end_time_us") == 0) meta->end_time_us = atoll(val);
        else if (strcmp(key, "end_time_utc") == 0) strncpy(meta->end_time_iso, val, sizeof(meta->end_time_iso) - 1);
        /* GPS fields */
        else if (strcmp(key, "gps_time_utc") == 0) strncpy(meta->gps_time_iso, val, sizeof(meta->gps_time_iso) - 1);
        else if (strcmp(key, "gps_time_us") == 0) meta->gps_time_us = atoll(val);
        else if (strcmp(key, "satellites") == 0) meta->gps_satellites = atoi(val);
        else if (strcmp(key, "pc_offset_ms") == 0) meta->gps_pc_offset_ms = atof(val);
        else if (strcmp(key, "port") == 0) strncpy(meta->gps_port, val, sizeof(meta->gps_port) - 1);
        else if (strcmp(key, "latency_ms") == 0) meta->gps_latency_ms = atof(val);
        /* Legacy NTP fields for backwards compatibility */
        else if (strcmp(key, "ntp_server") == 0) { /* ignore */ }
        else if (strcmp(key, "gps_valid") == 0) meta->gps_valid = (strcmp(val, "true") == 0);
        else if (strcmp(key, "gps_satellites") == 0) meta->gps_satellites = atoi(val);
        else if (strcmp(key, "gps_pc_offset_ms") == 0) meta->gps_pc_offset_ms = atof(val);
        else if (strcmp(key, "gps_port") == 0) strncpy(meta->gps_port, val, sizeof(meta->gps_port) - 1);
        else if (strcmp(key, "gps_latency_ms") == 0) meta->gps_latency_ms = atof(val);
    }
    
    fclose(f);
    return 0;
}
