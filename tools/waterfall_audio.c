/**
 * @file waterfall_audio.c
 * @brief Audio output module for waterfall application
 *
 * Handles Windows waveOut audio output with volume and mute control.
 * This file CAN be modified - it's part of the audio path, not the frozen DSP.
 */

#include "waterfall_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#define AUDIO_BUFFERS 8

static HWAVEOUT g_waveOut = NULL;
static WAVEHDR g_waveHeaders[AUDIO_BUFFERS];
static int16_t *g_audio_buffers[AUDIO_BUFFERS];
static int g_current_audio_buffer = 0;
static CRITICAL_SECTION g_audio_cs;
static bool g_audio_running = false;
static bool g_audio_enabled = true;
static float g_volume = 50.0f;

/* Output buffer for accumulating samples */
static int16_t g_audio_out[WF_AUDIO_BUFFER_SIZE];
static int g_audio_out_count = 0;

bool wf_audio_init(void) {
    InitializeCriticalSection(&g_audio_cs);

    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        g_audio_buffers[i] = (int16_t *)malloc(WF_AUDIO_BUFFER_SIZE * sizeof(int16_t));
        if (!g_audio_buffers[i]) return false;
        memset(g_audio_buffers[i], 0, WF_AUDIO_BUFFER_SIZE * sizeof(int16_t));
    }

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = (DWORD)WF_AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * 2;

    if (waveOutOpen(&g_waveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "Failed to open audio output\n");
        return false;
    }

    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        g_waveHeaders[i].lpData = (LPSTR)g_audio_buffers[i];
        g_waveHeaders[i].dwBufferLength = WF_AUDIO_BUFFER_SIZE * sizeof(int16_t);
        waveOutPrepareHeader(g_waveOut, &g_waveHeaders[i], sizeof(WAVEHDR));
    }

    g_audio_running = true;
    g_audio_out_count = 0;
    printf("Audio output initialized at %d Hz\n", WF_AUDIO_SAMPLE_RATE);
    return true;
}

void wf_audio_close(void) {
    if (!g_audio_running) return;
    g_audio_running = false;

    if (g_waveOut) {
        waveOutReset(g_waveOut);
        for (int i = 0; i < AUDIO_BUFFERS; i++) {
            waveOutUnprepareHeader(g_waveOut, &g_waveHeaders[i], sizeof(WAVEHDR));
            free(g_audio_buffers[i]);
        }
        waveOutClose(g_waveOut);
        g_waveOut = NULL;
    }
    DeleteCriticalSection(&g_audio_cs);
    printf("Audio output closed\n");
}

void wf_audio_write(const int16_t *samples, uint32_t count) {
    if (!g_audio_running || !g_audio_enabled || count == 0) return;

    EnterCriticalSection(&g_audio_cs);

    WAVEHDR *hdr = &g_waveHeaders[g_current_audio_buffer];

    /* Wait if buffer busy */
    while (hdr->dwFlags & WHDR_INQUEUE) {
        LeaveCriticalSection(&g_audio_cs);
        Sleep(1);
        EnterCriticalSection(&g_audio_cs);
    }

    uint32_t to_copy = (count > WF_AUDIO_BUFFER_SIZE) ? WF_AUDIO_BUFFER_SIZE : count;
    memcpy(g_audio_buffers[g_current_audio_buffer], samples, to_copy * sizeof(int16_t));
    hdr->dwBufferLength = to_copy * sizeof(int16_t);

    waveOutWrite(g_waveOut, hdr, sizeof(WAVEHDR));
    g_current_audio_buffer = (g_current_audio_buffer + 1) % AUDIO_BUFFERS;

    LeaveCriticalSection(&g_audio_cs);
}

void wf_audio_process_sample(float sample) {
    /* Apply volume scaling */
    float scaled = sample * g_volume;

    /* Hard clamp to int16 range */
    if (scaled > 32767.0f) scaled = 32767.0f;
    if (scaled < -32767.0f) scaled = -32767.0f;

    /* Accumulate in output buffer */
    g_audio_out[g_audio_out_count++] = (int16_t)scaled;

    /* Write when buffer is full */
    if (g_audio_out_count >= WF_AUDIO_BUFFER_SIZE) {
        wf_audio_write(g_audio_out, g_audio_out_count);
        g_audio_out_count = 0;
    }
}

void wf_audio_flush(void) {
    if (g_audio_out_count > 0) {
        wf_audio_write(g_audio_out, g_audio_out_count);
        g_audio_out_count = 0;
    }
}

float wf_audio_get_volume(void) {
    return g_volume;
}

void wf_audio_set_volume(float volume) {
    if (volume < 1.0f) volume = 1.0f;
    if (volume > 500.0f) volume = 500.0f;
    g_volume = volume;
    printf("Volume: %.0f\n", g_volume);
}

void wf_audio_volume_up(void) {
    wf_audio_set_volume(g_volume * 1.5f);
}

void wf_audio_volume_down(void) {
    wf_audio_set_volume(g_volume / 1.5f);
}

bool wf_audio_is_enabled(void) {
    return g_audio_enabled;
}

void wf_audio_set_enabled(bool enabled) {
    g_audio_enabled = enabled;
    printf("Audio: %s\n", g_audio_enabled ? "ON" : "MUTED");
}

void wf_audio_toggle_mute(void) {
    wf_audio_set_enabled(!g_audio_enabled);
}

#else
/* Non-Windows stub */
static bool g_audio_enabled = false;
static float g_volume = 50.0f;

bool wf_audio_init(void) { return false; }
void wf_audio_close(void) { }
void wf_audio_write(const int16_t *samples, uint32_t count) { (void)samples; (void)count; }
void wf_audio_process_sample(float sample) { (void)sample; }
void wf_audio_flush(void) { }
float wf_audio_get_volume(void) { return g_volume; }
void wf_audio_set_volume(float volume) { g_volume = volume; }
void wf_audio_volume_up(void) { }
void wf_audio_volume_down(void) { }
bool wf_audio_is_enabled(void) { return g_audio_enabled; }
void wf_audio_set_enabled(bool enabled) { g_audio_enabled = enabled; }
void wf_audio_toggle_mute(void) { }
#endif
