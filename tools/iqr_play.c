/**
 * @file iqr_play.c
 * @brief IQR file info utility
 * 
 * Displays info about .iqr files and optionally dumps samples.
 * 
 * Usage: iqr_play <file.iqr> [dump_count]
 */

#include "iq_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("IQR File Info Utility\n");
        printf("Usage: %s <file.iqr> [dump_count]\n", argv[0]);
        printf("\nOptions:\n");
        printf("  dump_count  Number of samples to dump (default: 0)\n");
        return 1;
    }
    
    const char *filename = argv[1];
    int dump_count = (argc > 2) ? atoi(argv[2]) : 0;
    
    iqr_reader_t *reader = NULL;
    iqr_error_t err;
    
    /* Open IQR file */
    err = iqr_open(&reader, filename);
    if (err != IQR_OK) {
        fprintf(stderr, "Failed to open file: %s\n", iqr_strerror(err));
        return 1;
    }
    
    /* Get file info */
    const iqr_header_t *hdr = iqr_get_header(reader);
    double duration = (double)hdr->sample_count / hdr->sample_rate_hz;
    
    printf("IQR File: %s\n", filename);
    printf("=========================================\n");
    printf("  Sample Rate:  %.0f Hz\n", hdr->sample_rate_hz);
    printf("  Center Freq:  %.6f MHz (%.0f Hz)\n", 
           hdr->center_freq_hz / 1e6, hdr->center_freq_hz);
    printf("  Bandwidth:    %u kHz\n", hdr->bandwidth_khz);
    printf("  Gain Reduc:   %u dB\n", hdr->gain_reduction);
    printf("  Samples:      %llu\n", (unsigned long long)hdr->sample_count);
    printf("  Duration:     %.2f seconds\n", duration);
    printf("=========================================\n");
    
    /* Dump samples if requested */
    if (dump_count > 0) {
        printf("\nFirst %d samples (I, Q, magnitude):\n", dump_count);
        
        int16_t xi[256], xq[256];
        uint32_t num_read;
        int dumped = 0;
        
        while (dumped < dump_count) {
            int to_read = (dump_count - dumped) < 256 ? (dump_count - dumped) : 256;
            err = iqr_read(reader, xi, xq, to_read, &num_read);
            if (err != IQR_OK || num_read == 0) break;
            
            for (uint32_t i = 0; i < num_read && dumped < dump_count; i++, dumped++) {
                float mag = sqrtf((float)xi[i]*xi[i] + (float)xq[i]*xq[i]);
                printf("  [%6d] I=%6d Q=%6d mag=%.1f\n", dumped, xi[i], xq[i], mag);
            }
        }
    }
    
    iqr_close(reader);
    return 0;
}
