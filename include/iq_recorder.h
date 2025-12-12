/**
 * @file iq_recorder.h
 * @brief I/Q sample recording to file
 * 
 * Records raw I/Q samples to disk for offline analysis and regression testing.
 * File format: Binary header followed by interleaved 16-bit I/Q samples.
 */

#ifndef IQ_RECORDER_H
#define IQ_RECORDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * File Format
 *============================================================================*/

/**
 * I/Q Recording File Format (.iqr)
 * 
 * Header (64 bytes, fixed):
 *   - Magic:        4 bytes  "IQR1"
 *   - Version:      4 bytes  uint32_t (1)
 *   - Sample Rate:  8 bytes  double (Hz)
 *   - Center Freq:  8 bytes  double (Hz)
 *   - Bandwidth:    4 bytes  uint32_t (kHz)
 *   - Gain Reduc:   4 bytes  int32_t (dB)
 *   - LNA State:    4 bytes  uint32_t
 *   - Start Time:   8 bytes  int64_t (Unix timestamp, microseconds)
 *   - Sample Count: 8 bytes  uint64_t (updated on close)
 *   - Flags:        4 bytes  uint32_t (reserved)
 *   - Reserved:     8 bytes  (padding to 64 bytes)
 * 
 * Data (after header):
 *   - Interleaved I/Q samples: I0, Q0, I1, Q1, ...
 *   - Each sample is int16_t (little-endian)
 *   - 4 bytes per sample pair
 */

#define IQR_MAGIC       "IQR1"
#define IQR_VERSION     1
#define IQR_HEADER_SIZE 64

#pragma pack(push, 1)
typedef struct {
    char        magic[4];       /* "IQR1" */
    uint32_t    version;        /* Format version */
    double      sample_rate_hz; /* Sample rate */
    double      center_freq_hz; /* Center frequency */
    uint32_t    bandwidth_khz;  /* IF bandwidth */
    int32_t     gain_reduction; /* Gain reduction dB */
    uint32_t    lna_state;      /* LNA state */
    int64_t     start_time_us;  /* Recording start (Unix time, microseconds) */
    uint64_t    sample_count;   /* Total samples recorded */
    uint32_t    flags;          /* Reserved flags */
    uint8_t     reserved[8];    /* Padding to 64 bytes */
} iqr_header_t;
#pragma pack(pop)

/* Verify header size at compile time */
_Static_assert(sizeof(iqr_header_t) == IQR_HEADER_SIZE, 
               "IQR header must be exactly 64 bytes");

/*============================================================================
 * Error Codes
 *============================================================================*/

typedef enum {
    IQR_OK = 0,
    IQR_ERR_INVALID_ARG,
    IQR_ERR_FILE_OPEN,
    IQR_ERR_FILE_WRITE,
    IQR_ERR_FILE_READ,
    IQR_ERR_FILE_SEEK,
    IQR_ERR_NOT_RECORDING,
    IQR_ERR_ALREADY_RECORDING,
    IQR_ERR_INVALID_FORMAT,
    IQR_ERR_VERSION_MISMATCH,
    IQR_ERR_ALLOC
} iqr_error_t;

/*============================================================================
 * Recording Context
 *============================================================================*/

typedef struct iqr_recorder iqr_recorder_t;

/*============================================================================
 * Recording API
 *============================================================================*/

/**
 * @brief Get human-readable error string
 */
const char* iqr_strerror(iqr_error_t err);

/**
 * @brief Create a new recorder instance
 * 
 * @param rec           Receives allocated recorder
 * @param buffer_size   Internal buffer size in sample pairs (0 for default 64K)
 * @return Error code
 */
iqr_error_t iqr_create(iqr_recorder_t **rec, size_t buffer_size);

/**
 * @brief Destroy recorder and free resources
 * 
 * @param rec  Recorder to destroy (NULL safe)
 */
void iqr_destroy(iqr_recorder_t *rec);

/**
 * @brief Start recording to file
 * 
 * @param rec             Recorder instance
 * @param filename        Output filename (.iqr extension recommended)
 * @param sample_rate_hz  Sample rate in Hz
 * @param center_freq_hz  Center frequency in Hz
 * @param bandwidth_khz   IF bandwidth in kHz
 * @param gain_reduction  Gain reduction in dB
 * @param lna_state       LNA state (0-8)
 * @return Error code
 */
iqr_error_t iqr_start(
    iqr_recorder_t *rec,
    const char *filename,
    double sample_rate_hz,
    double center_freq_hz,
    uint32_t bandwidth_khz,
    int32_t gain_reduction,
    uint32_t lna_state
);

/**
 * @brief Write I/Q samples to recording
 * 
 * Called from streaming callback. Thread-safe with internal buffering.
 * 
 * @param rec    Recorder instance
 * @param xi     I (real) samples
 * @param xq     Q (imaginary) samples
 * @param count  Number of samples
 * @return Error code
 */
iqr_error_t iqr_write(
    iqr_recorder_t *rec,
    const int16_t *xi,
    const int16_t *xq,
    uint32_t count
);

/**
 * @brief Stop recording and finalize file
 * 
 * Flushes buffers and updates header with final sample count.
 * 
 * @param rec  Recorder instance
 * @return Error code
 */
iqr_error_t iqr_stop(iqr_recorder_t *rec);

/**
 * @brief Check if currently recording
 * 
 * @param rec  Recorder instance
 * @return True if recording is active
 */
bool iqr_is_recording(const iqr_recorder_t *rec);

/**
 * @brief Get current sample count
 * 
 * @param rec  Recorder instance
 * @return Number of samples recorded so far
 */
uint64_t iqr_get_sample_count(const iqr_recorder_t *rec);

/**
 * @brief Get recording duration in seconds
 * 
 * @param rec  Recorder instance
 * @return Duration in seconds (based on sample count and rate)
 */
double iqr_get_duration(const iqr_recorder_t *rec);

/*============================================================================
 * Playback/Reader API (for offline analysis)
 *============================================================================*/

typedef struct iqr_reader iqr_reader_t;

/**
 * @brief Open an I/Q recording file for reading
 * 
 * @param reader    Receives allocated reader
 * @param filename  File to open
 * @return Error code
 */
iqr_error_t iqr_open(iqr_reader_t **reader, const char *filename);

/**
 * @brief Close reader and free resources
 * 
 * @param reader  Reader to close (NULL safe)
 */
void iqr_close(iqr_reader_t *reader);

/**
 * @brief Get file header/metadata
 * 
 * @param reader  Reader instance
 * @return Pointer to header (valid until reader closed)
 */
const iqr_header_t* iqr_get_header(const iqr_reader_t *reader);

/**
 * @brief Read samples from file
 * 
 * @param reader     Reader instance
 * @param xi         Buffer for I samples (must hold max_samples)
 * @param xq         Buffer for Q samples (must hold max_samples)
 * @param max_samples  Maximum samples to read
 * @param num_read   Receives actual number read (0 at EOF)
 * @return Error code
 */
iqr_error_t iqr_read(
    iqr_reader_t *reader,
    int16_t *xi,
    int16_t *xq,
    uint32_t max_samples,
    uint32_t *num_read
);

/**
 * @brief Seek to sample position
 * 
 * @param reader   Reader instance
 * @param sample   Sample index (0-based)
 * @return Error code
 */
iqr_error_t iqr_seek(iqr_reader_t *reader, uint64_t sample);

/**
 * @brief Reset to beginning of file
 * 
 * @param reader  Reader instance
 * @return Error code
 */
iqr_error_t iqr_rewind(iqr_reader_t *reader);

#ifdef __cplusplus
}
#endif

#endif /* IQ_RECORDER_H */
