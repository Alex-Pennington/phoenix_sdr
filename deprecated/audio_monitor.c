/**
 * @file audio_monitor.c
 * @brief Real-time audio monitoring using Windows waveOut API
 */

#include "audio_monitor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

/*============================================================================
 * Constants
 *============================================================================*/

#define NUM_BUFFERS     4
#define BUFFER_SAMPLES  4096    /* Samples per buffer */

/*============================================================================
 * Internal Structure
 *============================================================================*/

struct audio_monitor {
    HWAVEOUT        hWaveOut;
    WAVEHDR         waveHeaders[NUM_BUFFERS];
    int16_t        *buffers[NUM_BUFFERS];
    int             currentBuffer;
    int             bufferSamples;
    double          sampleRate;
    bool            running;
    CRITICAL_SECTION cs;
};

/*============================================================================
 * Error Strings
 *============================================================================*/

const char* audio_strerror(audio_error_t err) {
    switch (err) {
        case AUDIO_OK:           return "OK";
        case AUDIO_ERR_INIT:     return "Initialization failed";
        case AUDIO_ERR_NO_DEVICE: return "No audio device found";
        case AUDIO_ERR_BUFFER:   return "Buffer allocation failed";
        case AUDIO_ERR_WRITE:    return "Write failed";
        case AUDIO_ERR_NOT_RUNNING: return "Audio not running";
        default:                 return "Unknown error";
    }
}

/*============================================================================
 * Callback
 *============================================================================*/

static void CALLBACK waveOutCallback(
    HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance,
    DWORD_PTR dwParam1, DWORD_PTR dwParam2
) {
    (void)hwo;
    (void)dwParam1;
    (void)dwParam2;
    
    if (uMsg == WOM_DONE) {
        audio_monitor_t *mon = (audio_monitor_t *)dwInstance;
        (void)mon;  /* Could signal buffer available here */
    }
}

/*============================================================================
 * API Implementation
 *============================================================================*/

audio_error_t audio_create(audio_monitor_t **mon, double sample_rate) {
    if (!mon) return AUDIO_ERR_INIT;
    
    audio_monitor_t *m = (audio_monitor_t *)calloc(1, sizeof(audio_monitor_t));
    if (!m) return AUDIO_ERR_BUFFER;
    
    m->sampleRate = sample_rate;
    m->bufferSamples = BUFFER_SAMPLES;
    m->currentBuffer = 0;
    m->running = false;
    
    InitializeCriticalSection(&m->cs);
    
    /* Allocate audio buffers */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        m->buffers[i] = (int16_t *)malloc(BUFFER_SAMPLES * sizeof(int16_t));
        if (!m->buffers[i]) {
            /* Cleanup on failure */
            for (int j = 0; j < i; j++) {
                free(m->buffers[j]);
            }
            DeleteCriticalSection(&m->cs);
            free(m);
            return AUDIO_ERR_BUFFER;
        }
        memset(m->buffers[i], 0, BUFFER_SAMPLES * sizeof(int16_t));
    }
    
    *mon = m;
    return AUDIO_OK;
}

audio_error_t audio_start(audio_monitor_t *mon) {
    if (!mon) return AUDIO_ERR_INIT;
    
    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;  /* Mono */
    wfx.nSamplesPerSec = (DWORD)mon->sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    
    MMRESULT result = waveOutOpen(
        &mon->hWaveOut,
        WAVE_MAPPER,
        &wfx,
        (DWORD_PTR)waveOutCallback,
        (DWORD_PTR)mon,
        CALLBACK_FUNCTION
    );
    
    if (result != MMSYSERR_NOERROR) {
        fprintf(stderr, "waveOutOpen failed: %d\n", result);
        return AUDIO_ERR_NO_DEVICE;
    }
    
    /* Prepare headers */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        memset(&mon->waveHeaders[i], 0, sizeof(WAVEHDR));
        mon->waveHeaders[i].lpData = (LPSTR)mon->buffers[i];
        mon->waveHeaders[i].dwBufferLength = BUFFER_SAMPLES * sizeof(int16_t);
        
        result = waveOutPrepareHeader(mon->hWaveOut, &mon->waveHeaders[i], sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            fprintf(stderr, "waveOutPrepareHeader failed: %d\n", result);
            waveOutClose(mon->hWaveOut);
            return AUDIO_ERR_INIT;
        }
    }
    
    mon->running = true;
    printf("Audio monitor started (%.0f Hz mono)\n", mon->sampleRate);
    
    return AUDIO_OK;
}

audio_error_t audio_write(audio_monitor_t *mon,
                          const int16_t *xi, const int16_t *xq,
                          uint32_t count) {
    (void)xq;  /* Mono output - only use I channel */
    
    if (!mon || !mon->running) return AUDIO_ERR_NOT_RUNNING;
    if (!xi || count == 0) return AUDIO_OK;
    
    EnterCriticalSection(&mon->cs);
    
    uint32_t written = 0;
    while (written < count) {
        WAVEHDR *hdr = &mon->waveHeaders[mon->currentBuffer];
        
        /* Wait for buffer to be available */
        while (hdr->dwFlags & WHDR_INQUEUE) {
            LeaveCriticalSection(&mon->cs);
            Sleep(1);
            EnterCriticalSection(&mon->cs);
        }
        
        /* Calculate how much to copy */
        uint32_t toCopy = count - written;
        if (toCopy > BUFFER_SAMPLES) {
            toCopy = BUFFER_SAMPLES;
        }
        
        /* Copy samples */
        memcpy(mon->buffers[mon->currentBuffer], xi + written, toCopy * sizeof(int16_t));
        hdr->dwBufferLength = toCopy * sizeof(int16_t);
        
        /* Queue buffer */
        MMRESULT result = waveOutWrite(mon->hWaveOut, hdr, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            LeaveCriticalSection(&mon->cs);
            return AUDIO_ERR_WRITE;
        }
        
        written += toCopy;
        mon->currentBuffer = (mon->currentBuffer + 1) % NUM_BUFFERS;
    }
    
    LeaveCriticalSection(&mon->cs);
    return AUDIO_OK;
}

audio_error_t audio_write_float(audio_monitor_t *mon,
                                const float *fi, const float *fq,
                                uint32_t count) {
    (void)fq;  /* Mono output */
    
    if (!mon || !mon->running) return AUDIO_ERR_NOT_RUNNING;
    if (!fi || count == 0) return AUDIO_OK;
    
    /* Convert float to int16 and write */
    int16_t *temp = (int16_t *)malloc(count * sizeof(int16_t));
    if (!temp) return AUDIO_ERR_BUFFER;
    
    for (uint32_t i = 0; i < count; i++) {
        float sample = fi[i] * 32767.0f;
        if (sample > 32767.0f) sample = 32767.0f;
        if (sample < -32768.0f) sample = -32768.0f;
        temp[i] = (int16_t)sample;
    }
    
    audio_error_t err = audio_write(mon, temp, NULL, count);
    free(temp);
    return err;
}

void audio_stop(audio_monitor_t *mon) {
    if (!mon || !mon->running) return;
    
    mon->running = false;
    
    waveOutReset(mon->hWaveOut);
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        waveOutUnprepareHeader(mon->hWaveOut, &mon->waveHeaders[i], sizeof(WAVEHDR));
    }
    
    waveOutClose(mon->hWaveOut);
    printf("Audio monitor stopped\n");
}

void audio_destroy(audio_monitor_t *mon) {
    if (!mon) return;
    
    if (mon->running) {
        audio_stop(mon);
    }
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        free(mon->buffers[i]);
    }
    
    DeleteCriticalSection(&mon->cs);
    free(mon);
}

bool audio_is_running(const audio_monitor_t *mon) {
    return mon && mon->running;
}

#else
/* Non-Windows stub implementation */

struct audio_monitor {
    bool running;
};

const char* audio_strerror(audio_error_t err) {
    (void)err;
    return "Audio monitoring not supported on this platform";
}

audio_error_t audio_create(audio_monitor_t **mon, double sample_rate) {
    (void)sample_rate;
    *mon = NULL;
    fprintf(stderr, "Audio monitoring only supported on Windows\n");
    return AUDIO_ERR_INIT;
}

audio_error_t audio_start(audio_monitor_t *mon) {
    (void)mon;
    return AUDIO_ERR_INIT;
}

audio_error_t audio_write(audio_monitor_t *mon,
                          const int16_t *xi, const int16_t *xq,
                          uint32_t count) {
    (void)mon; (void)xi; (void)xq; (void)count;
    return AUDIO_ERR_NOT_RUNNING;
}

audio_error_t audio_write_float(audio_monitor_t *mon,
                                const float *fi, const float *fq,
                                uint32_t count) {
    (void)mon; (void)fi; (void)fq; (void)count;
    return AUDIO_ERR_NOT_RUNNING;
}

void audio_stop(audio_monitor_t *mon) { (void)mon; }
void audio_destroy(audio_monitor_t *mon) { (void)mon; }
bool audio_is_running(const audio_monitor_t *mon) { (void)mon; return false; }

#endif /* _WIN32 */
