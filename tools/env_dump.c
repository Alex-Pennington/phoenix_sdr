/* Quick envelope dumper */
#include "iq_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char *argv[]) {
    if (argc < 4) { printf("Usage: env_dump <file.iqr> <start_ms> <count_ms>\n"); return 1; }
    
    iqr_reader_t *reader = NULL;
    if (iqr_open(&reader, argv[1]) != IQR_OK) { printf("Failed to open\n"); return 1; }
    
    const iqr_header_t *hdr = iqr_get_header(reader);
    float fs = (float)hdr->sample_rate_hz;
    int start_ms = atoi(argv[2]);
    int count_ms = atoi(argv[3]);
    
    uint64_t start_sample = (uint64_t)(start_ms * fs / 1000.0);
    iqr_seek(reader, start_sample);
    
    int samples_per_ms = (int)(fs / 1000.0);
    int16_t *xi = malloc(samples_per_ms * sizeof(int16_t));
    int16_t *xq = malloc(samples_per_ms * sizeof(int16_t));
    
    for (int ms = 0; ms < count_ms; ms++) {
        uint32_t num_read;
        iqr_read(reader, xi, xq, samples_per_ms, &num_read);
        
        float sum = 0;
        for (int i = 0; i < num_read; i++) {
            float mag = sqrtf((float)(xi[i]*xi[i] + xq[i]*xq[i])) / 32768.0f;
            sum += mag;
        }
        printf("%d %.6f\n", start_ms + ms, sum / num_read);
    }
    
    free(xi); free(xq);
    iqr_close(reader);
    return 0;
}
