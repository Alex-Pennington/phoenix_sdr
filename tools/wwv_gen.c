/**
 * @file wwv_gen.c
 * @brief WWV test signal generator - standalone tool
 *
 * Generates WWV/WWVH time signals as IQ recording files for testing.
 * Outputs fixed 2-minute reference signals with exact timing per NIST spec.
 */

#include "wwv_signal.h"
#include "../include/iq_recorder.h"
#include "../include/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SAMPLE_RATE 2000000
#define SAMPLES_PER_MINUTE 120000000ULL
#define BUFFER_SIZE 65536  // 64K samples = 32K I/Q pairs

static void print_usage(const char *prog) {
    printf("WWV Test Signal Generator v%s\n", PHOENIX_VERSION_FULL);
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -t HH:MM        Start time (default: 00:00)\n");
    printf("  -d DDD          Day of year (default: 001)\n");
    printf("  -y YY           Year, last 2 digits (default: 25)\n");
    printf("  -s wwv|wwvh     Station type (default: wwv)\n");
    printf("  -o FILE         Output .iqr file (default: wwv_test.iqr)\n");
    printf("  -h              Show this help\n");
    printf("\nGenerates fixed 2-minute reference signal at 2 Msps.\n");
}

static bool parse_time(const char *str, int *hour, int *minute) {
    if (sscanf(str, "%d:%d", hour, minute) != 2) {
        return false;
    }
    if (*hour < 0 || *hour > 23 || *minute < 0 || *minute > 59) {
        return false;
    }
    return true;
}

int main(int argc, char *argv[]) {
    // Default parameters
    int start_hour = 0;
    int start_minute = 0;
    int day_of_year = 1;
    int year = 25;
    wwv_station_t station = WWV_STATION_WWV;
    const char *output_file = "wwv_test.iqr";

    // Parse command line
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            if (!parse_time(argv[++i], &start_hour, &start_minute)) {
                fprintf(stderr, "Error: Invalid time format '%s'\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            day_of_year = atoi(argv[++i]);
            if (day_of_year < 1 || day_of_year > 366) {
                fprintf(stderr, "Error: Day of year must be 1-366\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-y") == 0 && i + 1 < argc) {
            year = atoi(argv[++i]);
            if (year < 0 || year > 99) {
                fprintf(stderr, "Error: Year must be 0-99\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "wwv") == 0) {
                station = WWV_STATION_WWV;
            } else if (strcmp(argv[i], "wwvh") == 0) {
                station = WWV_STATION_WWVH;
            } else {
                fprintf(stderr, "Error: Station must be 'wwv' or 'wwvh'\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    // Print configuration
    printf("Generating WWV test signal...\n");
    printf("  Station:    %s\n", station == WWV_STATION_WWV ? "WWV" : "WWVH");
    printf("  Start time: %02d:%02d (day %03d, year 20%02d)\n",
           start_hour, start_minute, day_of_year, year);
    printf("  Output:     %s\n", output_file);
    printf("  Duration:   2 minutes (120M samples)\n");

    // Create signal generator
    wwv_signal_t *sig = wwv_signal_create(start_minute, start_hour, day_of_year, year, station);
    if (!sig) {
        fprintf(stderr, "Error: Failed to create signal generator\n");
        return 1;
    }

    // Create IQ recorder
    iqr_recorder_t *rec;
    if (iqr_create(&rec, BUFFER_SIZE) != 0) {
        fprintf(stderr, "Error: Failed to create IQ recorder\n");
        wwv_signal_destroy(sig);
        return 1;
    }

    // Start recording
    // Use 5 MHz as nominal center frequency (matches common WWV tuning)
    if (iqr_start(rec, output_file, SAMPLE_RATE, 5000000.0, 2000, 59, 0) != 0) {
        fprintf(stderr, "Error: Failed to start recording\n");
        iqr_destroy(rec);
        wwv_signal_destroy(sig);
        return 1;
    }

    // Allocate sample buffers
    short *i_buffer = (short *)malloc(BUFFER_SIZE * sizeof(short));
    short *q_buffer = (short *)malloc(BUFFER_SIZE * sizeof(short));
    if (!i_buffer || !q_buffer) {
        fprintf(stderr, "Error: Failed to allocate buffers\n");
        free(i_buffer);
        free(q_buffer);
        iqr_stop(rec);
        iqr_destroy(rec);
        wwv_signal_destroy(sig);
        return 1;
    }

    // Generate 2 minutes of samples
    uint64_t total_samples = 2 * SAMPLES_PER_MINUTE;
    uint64_t samples_written = 0;
    int last_progress = -1;

    while (samples_written < total_samples) {
        // Fill buffer
        int samples_to_write = BUFFER_SIZE;
        if (samples_written + samples_to_write > total_samples) {
            samples_to_write = (int)(total_samples - samples_written);
        }

        for (int i = 0; i < samples_to_write; i++) {
            int16_t i_sample, q_sample;
            wwv_signal_get_sample_int16(sig, &i_sample, &q_sample);
            i_buffer[i] = i_sample;
            q_buffer[i] = q_sample;
        }

        // Write to file
        if (iqr_write(rec, i_buffer, q_buffer, samples_to_write) != 0) {
            fprintf(stderr, "Error: Failed to write samples\n");
            break;
        }

        samples_written += samples_to_write;

        // Update progress every 10 seconds
        int progress = (int)((samples_written * 100) / total_samples);
        if (progress / 10 != last_progress / 10) {
            int seconds = (int)(samples_written / SAMPLE_RATE);
            printf("\r  Progress: %d%% (%d seconds)...", progress, seconds);
            fflush(stdout);
            last_progress = progress;
        }
    }

    printf("\r  Progress: 100%% (120 seconds)... Done!\n");

    // Cleanup
    free(i_buffer);
    free(q_buffer);
    iqr_stop(rec);
    iqr_destroy(rec);
    wwv_signal_destroy(sig);

    printf("Generated %llu samples (%.1f MB)\n",
           (unsigned long long)samples_written,
           (samples_written * 4.0) / (1024 * 1024));

    return 0;
}
