/**
 * @file iq_recorder.c
 * @brief I/Q sample recording implementation
 */

#include "iq_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/time.h>
#endif

/*============================================================================
 * Constants
 *============================================================================*/

#define DEFAULT_BUFFER_SAMPLES  (64 * 1024)  /* 64K sample pairs */

/*============================================================================
 * Internal Structures
 *============================================================================*/

struct iqr_recorder {
    FILE           *file;
    iqr_header_t    header;
    int16_t        *buffer;         /* Interleaved I/Q buffer */
    size_t          buffer_size;    /* Buffer capacity (sample pairs) */
    size_t          buffer_used;    /* Samples in buffer */
    uint64_t        total_samples;
    bool            recording;
};

struct iqr_reader {
    FILE           *file;
    iqr_header_t    header;
    uint64_t        position;       /* Current sample position */
};

/*============================================================================
 * Error Strings
 *============================================================================*/

static const char *error_strings[] = {
    [IQR_OK]                  = "Success",
    [IQR_ERR_INVALID_ARG]     = "Invalid argument",
    [IQR_ERR_FILE_OPEN]       = "Failed to open file",
    [IQR_ERR_FILE_WRITE]      = "Failed to write file",
    [IQR_ERR_FILE_READ]       = "Failed to read file",
    [IQR_ERR_FILE_SEEK]       = "Failed to seek in file",
    [IQR_ERR_NOT_RECORDING]   = "Not currently recording",
    [IQR_ERR_ALREADY_RECORDING] = "Already recording",
    [IQR_ERR_INVALID_FORMAT]  = "Invalid file format",
    [IQR_ERR_VERSION_MISMATCH] = "File version mismatch",
    [IQR_ERR_ALLOC]           = "Memory allocation failed"
};

const char* iqr_strerror(iqr_error_t err) {
    if (err < 0 || err > IQR_ERR_ALLOC) {
        return "Unknown error";
    }
    return error_strings[err];
}

/*============================================================================
 * Helpers
 *============================================================================*/

static int64_t get_timestamp_us(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    /* Convert Windows FILETIME to Unix timestamp in microseconds */
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;  /* Jan 1, 1601 -> Jan 1, 1970 */
    return (int64_t)(t / 10);    /* 100ns -> microseconds */
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
#endif
}

static iqr_error_t flush_buffer(iqr_recorder_t *rec) {
    if (!rec->buffer_used) return IQR_OK;
    
    size_t bytes = rec->buffer_used * 2 * sizeof(int16_t);
    size_t written = fwrite(rec->buffer, 1, bytes, rec->file);
    
    if (written != bytes) {
        return IQR_ERR_FILE_WRITE;
    }
    
    rec->total_samples += rec->buffer_used;
    rec->buffer_used = 0;
    
    return IQR_OK;
}

/*============================================================================
 * Recorder Implementation
 *============================================================================*/

iqr_error_t iqr_create(iqr_recorder_t **rec, size_t buffer_size) {
    if (!rec) return IQR_ERR_INVALID_ARG;
    
    iqr_recorder_t *r = calloc(1, sizeof(iqr_recorder_t));
    if (!r) return IQR_ERR_ALLOC;
    
    r->buffer_size = buffer_size ? buffer_size : DEFAULT_BUFFER_SAMPLES;
    
    /* Allocate interleaved buffer: I0, Q0, I1, Q1, ... */
    r->buffer = malloc(r->buffer_size * 2 * sizeof(int16_t));
    if (!r->buffer) {
        free(r);
        return IQR_ERR_ALLOC;
    }
    
    *rec = r;
    return IQR_OK;
}

void iqr_destroy(iqr_recorder_t *rec) {
    if (!rec) return;
    
    if (rec->recording) {
        iqr_stop(rec);
    }
    
    free(rec->buffer);
    free(rec);
}

iqr_error_t iqr_start(
    iqr_recorder_t *rec,
    const char *filename,
    double sample_rate_hz,
    double center_freq_hz,
    uint32_t bandwidth_khz,
    int32_t gain_reduction,
    uint32_t lna_state
) {
    if (!rec || !filename) return IQR_ERR_INVALID_ARG;
    if (rec->recording) return IQR_ERR_ALREADY_RECORDING;
    
    /* Open file */
    rec->file = fopen(filename, "wb");
    if (!rec->file) {
        return IQR_ERR_FILE_OPEN;
    }
    
    /* Initialize header */
    memset(&rec->header, 0, sizeof(rec->header));
    memcpy(rec->header.magic, IQR_MAGIC, 4);
    rec->header.version = IQR_VERSION;
    rec->header.sample_rate_hz = sample_rate_hz;
    rec->header.center_freq_hz = center_freq_hz;
    rec->header.bandwidth_khz = bandwidth_khz;
    rec->header.gain_reduction = gain_reduction;
    rec->header.lna_state = lna_state;
    rec->header.start_time_us = get_timestamp_us();
    rec->header.sample_count = 0;
    rec->header.flags = 0;
    
    /* Write initial header (will update sample_count on close) */
    if (fwrite(&rec->header, sizeof(rec->header), 1, rec->file) != 1) {
        fclose(rec->file);
        rec->file = NULL;
        return IQR_ERR_FILE_WRITE;
    }
    
    rec->buffer_used = 0;
    rec->total_samples = 0;
    rec->recording = true;
    
    printf("iqr_start: Recording to %s\n", filename);
    printf("  Sample rate: %.0f Hz\n", sample_rate_hz);
    printf("  Center freq: %.0f Hz\n", center_freq_hz);
    printf("  Bandwidth:   %u kHz\n", bandwidth_khz);
    
    return IQR_OK;
}

iqr_error_t iqr_write(
    iqr_recorder_t *rec,
    const int16_t *xi,
    const int16_t *xq,
    uint32_t count
) {
    if (!rec || !xi || !xq) return IQR_ERR_INVALID_ARG;
    if (!rec->recording) return IQR_ERR_NOT_RECORDING;
    
    const int16_t *pi = xi;
    const int16_t *pq = xq;
    uint32_t remaining = count;
    
    while (remaining > 0) {
        /* Calculate how many samples fit in buffer */
        size_t space = rec->buffer_size - rec->buffer_used;
        size_t to_copy = (remaining < space) ? remaining : space;
        
        /* Interleave into buffer */
        int16_t *dst = rec->buffer + (rec->buffer_used * 2);
        for (size_t i = 0; i < to_copy; i++) {
            *dst++ = *pi++;
            *dst++ = *pq++;
        }
        rec->buffer_used += to_copy;
        remaining -= (uint32_t)to_copy;
        
        /* Flush if buffer full */
        if (rec->buffer_used >= rec->buffer_size) {
            iqr_error_t err = flush_buffer(rec);
            if (err != IQR_OK) return err;
        }
    }
    
    return IQR_OK;
}

iqr_error_t iqr_stop(iqr_recorder_t *rec) {
    if (!rec) return IQR_ERR_INVALID_ARG;
    if (!rec->recording) return IQR_ERR_NOT_RECORDING;
    
    /* Flush remaining samples */
    iqr_error_t err = flush_buffer(rec);
    if (err != IQR_OK) {
        fclose(rec->file);
        rec->file = NULL;
        rec->recording = false;
        return err;
    }
    
    /* Update header with final sample count */
    rec->header.sample_count = rec->total_samples;
    
    if (fseek(rec->file, 0, SEEK_SET) != 0) {
        fclose(rec->file);
        rec->file = NULL;
        rec->recording = false;
        return IQR_ERR_FILE_SEEK;
    }
    
    if (fwrite(&rec->header, sizeof(rec->header), 1, rec->file) != 1) {
        fclose(rec->file);
        rec->file = NULL;
        rec->recording = false;
        return IQR_ERR_FILE_WRITE;
    }
    
    fclose(rec->file);
    rec->file = NULL;
    rec->recording = false;
    
    double duration = (double)rec->total_samples / rec->header.sample_rate_hz;
    printf("iqr_stop: Recording complete\n");
    printf("  Samples: %llu\n", (unsigned long long)rec->total_samples);
    printf("  Duration: %.2f seconds\n", duration);
    
    return IQR_OK;
}

bool iqr_is_recording(const iqr_recorder_t *rec) {
    return rec && rec->recording;
}

uint64_t iqr_get_sample_count(const iqr_recorder_t *rec) {
    if (!rec) return 0;
    return rec->total_samples + rec->buffer_used;
}

double iqr_get_duration(const iqr_recorder_t *rec) {
    if (!rec || rec->header.sample_rate_hz <= 0) return 0.0;
    return (double)iqr_get_sample_count(rec) / rec->header.sample_rate_hz;
}

/*============================================================================
 * Reader Implementation
 *============================================================================*/

iqr_error_t iqr_open(iqr_reader_t **reader, const char *filename) {
    if (!reader || !filename) return IQR_ERR_INVALID_ARG;
    
    iqr_reader_t *r = calloc(1, sizeof(iqr_reader_t));
    if (!r) return IQR_ERR_ALLOC;
    
    r->file = fopen(filename, "rb");
    if (!r->file) {
        free(r);
        return IQR_ERR_FILE_OPEN;
    }
    
    /* Read header */
    if (fread(&r->header, sizeof(r->header), 1, r->file) != 1) {
        fclose(r->file);
        free(r);
        return IQR_ERR_FILE_READ;
    }
    
    /* Validate magic */
    if (memcmp(r->header.magic, IQR_MAGIC, 4) != 0) {
        fclose(r->file);
        free(r);
        return IQR_ERR_INVALID_FORMAT;
    }
    
    /* Check version */
    if (r->header.version != IQR_VERSION) {
        fclose(r->file);
        free(r);
        return IQR_ERR_VERSION_MISMATCH;
    }
    
    r->position = 0;
    *reader = r;
    
    printf("iqr_open: Opened %s\n", filename);
    printf("  Sample rate: %.0f Hz\n", r->header.sample_rate_hz);
    printf("  Center freq: %.0f Hz\n", r->header.center_freq_hz);
    printf("  Samples: %llu\n", (unsigned long long)r->header.sample_count);
    printf("  Duration: %.2f seconds\n", 
           (double)r->header.sample_count / r->header.sample_rate_hz);
    
    return IQR_OK;
}

void iqr_close(iqr_reader_t *reader) {
    if (!reader) return;
    
    if (reader->file) {
        fclose(reader->file);
    }
    free(reader);
}

const iqr_header_t* iqr_get_header(const iqr_reader_t *reader) {
    if (!reader) return NULL;
    return &reader->header;
}

iqr_error_t iqr_read(
    iqr_reader_t *reader,
    int16_t *xi,
    int16_t *xq,
    uint32_t max_samples,
    uint32_t *num_read
) {
    if (!reader || !xi || !xq || !num_read) return IQR_ERR_INVALID_ARG;
    
    *num_read = 0;
    
    /* Check if at end */
    if (reader->position >= reader->header.sample_count) {
        return IQR_OK;
    }
    
    /* Calculate how many samples to read */
    uint64_t remaining = reader->header.sample_count - reader->position;
    uint32_t to_read = (max_samples < remaining) ? max_samples : (uint32_t)remaining;
    
    /* Read interleaved data */
    /* Using a temporary buffer to de-interleave */
    int16_t *temp = malloc(to_read * 2 * sizeof(int16_t));
    if (!temp) return IQR_ERR_ALLOC;
    
    size_t read_count = fread(temp, 2 * sizeof(int16_t), to_read, reader->file);
    
    if (read_count == 0 && ferror(reader->file)) {
        free(temp);
        return IQR_ERR_FILE_READ;
    }
    
    /* De-interleave: I0, Q0, I1, Q1, ... -> separate I and Q arrays */
    for (size_t i = 0; i < read_count; i++) {
        xi[i] = temp[i * 2];
        xq[i] = temp[i * 2 + 1];
    }
    
    free(temp);
    
    reader->position += read_count;
    *num_read = (uint32_t)read_count;
    
    return IQR_OK;
}

iqr_error_t iqr_seek(iqr_reader_t *reader, uint64_t sample) {
    if (!reader) return IQR_ERR_INVALID_ARG;
    
    if (sample > reader->header.sample_count) {
        sample = reader->header.sample_count;
    }
    
    /* Calculate file position: header + (sample * 4 bytes per sample pair) */
    long offset = IQR_HEADER_SIZE + (long)(sample * 2 * sizeof(int16_t));
    
    if (fseek(reader->file, offset, SEEK_SET) != 0) {
        return IQR_ERR_FILE_SEEK;
    }
    
    reader->position = sample;
    return IQR_OK;
}

iqr_error_t iqr_rewind(iqr_reader_t *reader) {
    return iqr_seek(reader, 0);
}
