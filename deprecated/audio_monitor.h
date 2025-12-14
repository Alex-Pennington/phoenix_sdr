/**
 * @file audio_monitor.h
 * @brief Real-time audio monitoring for I/Q capture
 * 
 * Outputs decimated I/Q as audio to speakers for monitoring during capture.
 * Uses Windows waveOut API - no external dependencies.
 */

#ifndef AUDIO_MONITOR_H
#define AUDIO_MONITOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Error Codes
 *============================================================================*/

typedef enum {
    AUDIO_OK = 0,
    AUDIO_ERR_INIT,
    AUDIO_ERR_NO_DEVICE,
    AUDIO_ERR_BUFFER,
    AUDIO_ERR_WRITE,
    AUDIO_ERR_NOT_RUNNING
} audio_error_t;

/*============================================================================
 * Opaque Handle
 *============================================================================*/

typedef struct audio_monitor audio_monitor_t;

/*============================================================================
 * API Functions
 *============================================================================*/

/**
 * @brief Get error string
 */
const char* audio_strerror(audio_error_t err);

/**
 * @brief Create audio monitor
 * 
 * @param mon           Receives allocated monitor
 * @param sample_rate   Sample rate in Hz (e.g., 48000)
 * @return Error code
 */
audio_error_t audio_create(audio_monitor_t **mon, double sample_rate);

/**
 * @brief Start audio output
 * 
 * @param mon  Audio monitor
 * @return Error code
 */
audio_error_t audio_start(audio_monitor_t *mon);

/**
 * @brief Write I/Q samples to audio output
 * 
 * Converts complex I/Q to mono audio (uses I channel).
 * Non-blocking - buffers internally.
 * 
 * @param mon    Audio monitor
 * @param xi     I samples (int16)
 * @param xq     Q samples (int16) - currently unused, mono output
 * @param count  Number of samples
 * @return Error code
 */
audio_error_t audio_write(audio_monitor_t *mon, 
                          const int16_t *xi, const int16_t *xq,
                          uint32_t count);

/**
 * @brief Write float I/Q samples to audio output
 * 
 * @param mon    Audio monitor
 * @param fi     I samples (float, -1.0 to 1.0)
 * @param fq     Q samples (float) - currently unused
 * @param count  Number of samples
 * @return Error code
 */
audio_error_t audio_write_float(audio_monitor_t *mon,
                                const float *fi, const float *fq,
                                uint32_t count);

/**
 * @brief Stop audio output
 * 
 * @param mon  Audio monitor
 */
void audio_stop(audio_monitor_t *mon);

/**
 * @brief Destroy audio monitor
 * 
 * @param mon  Audio monitor (will be freed)
 */
void audio_destroy(audio_monitor_t *mon);

/**
 * @brief Check if audio is running
 * 
 * @param mon  Audio monitor
 * @return True if running
 */
bool audio_is_running(const audio_monitor_t *mon);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_MONITOR_H */
