/**
 * @file simple_am_receiver.c
 * @brief Simple AM Receiver for WWV
 *
 * DSP Pipeline:
 * 1. Receive IQ samples from SDRplay callback (short *xi, short *xq)
 * 2. Envelope detection: magnitude = sqrt(I² + Q²)
 * 3. Lowpass anti-aliasing filter (2.5 kHz cutoff)
 * 4. Decimation: 2 MHz → 48 kHz (factor 42)
 * 5. DC removal: highpass IIR y[n] = x[n] - x[n-1] + 0.995*y[n-1]
 * 6. Output to speakers
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <signal.h>

#include "sdrplay_api.h"

#ifdef _WIN32
#include <Windows.h>
#include <mmsystem.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

#define SDR_SAMPLE_RATE     2000000.0   /* 2 MHz from SDRplay */
#define AUDIO_SAMPLE_RATE   48000.0     /* 48 kHz audio output */
#define DECIMATION_FACTOR   42          /* 2M / 48k ≈ 42 */
#define AUDIO_BANDWIDTH     2500.0      /* 2.5 kHz for WWV */

#define DEFAULT_FREQ_MHZ    15.0
#define DEFAULT_GAIN_DB     40

/* Audio output */
#define AUDIO_BUFFERS       4
#define AUDIO_BUFFER_SIZE   4096

/*============================================================================
 * Lowpass Filter (simple 2nd order Butterworth, 2.5 kHz @ 2 MHz)
 *============================================================================*/

typedef struct {
    float x1, x2;   /* Input history */
    float y1, y2;   /* Output history */
    float b0, b1, b2, a1, a2;  /* Coefficients */
} lowpass_t;

static void lowpass_init(lowpass_t *lp, float cutoff_hz, float sample_rate) {
    /* 2nd order Butterworth lowpass */
    float w0 = 2.0f * 3.14159265f * cutoff_hz / sample_rate;
    float alpha = sinf(w0) / (2.0f * 0.7071f);  /* Q = 0.7071 for Butterworth */
    float cos_w0 = cosf(w0);

    float a0 = 1.0f + alpha;
    lp->b0 = (1.0f - cos_w0) / 2.0f / a0;
    lp->b1 = (1.0f - cos_w0) / a0;
    lp->b2 = (1.0f - cos_w0) / 2.0f / a0;
    lp->a1 = -2.0f * cos_w0 / a0;
    lp->a2 = (1.0f - alpha) / a0;

    lp->x1 = lp->x2 = 0.0f;
    lp->y1 = lp->y2 = 0.0f;
}

static float lowpass_process(lowpass_t *lp, float x) {
    float y = lp->b0 * x + lp->b1 * lp->x1 + lp->b2 * lp->x2
            - lp->a1 * lp->y1 - lp->a2 * lp->y2;
    lp->x2 = lp->x1;
    lp->x1 = x;
    lp->y2 = lp->y1;
    lp->y1 = y;
    return y;
}

/*============================================================================
 * DC Removal (highpass IIR: y[n] = x[n] - x[n-1] + 0.995*y[n-1])
 *============================================================================*/

typedef struct {
    float x_prev;
    float y_prev;
} dc_block_t;

static void dc_block_init(dc_block_t *dc) {
    dc->x_prev = 0.0f;
    dc->y_prev = 0.0f;
}

static float dc_block_process(dc_block_t *dc, float x) {
    float y = x - dc->x_prev + 0.995f * dc->y_prev;
    dc->x_prev = x;
    dc->y_prev = y;
    return y;
}

/*============================================================================
 * Audio Output (Windows waveOut)
 *============================================================================*/

#ifdef _WIN32
static HWAVEOUT g_waveOut;
static WAVEHDR g_headers[AUDIO_BUFFERS];
static int16_t *g_audio_buffers[AUDIO_BUFFERS];
static int g_current_buffer = 0;
static CRITICAL_SECTION g_audio_cs;
static bool g_audio_running = false;

static bool audio_init(void) {
    InitializeCriticalSection(&g_audio_cs);

    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        g_audio_buffers[i] = (int16_t *)malloc(AUDIO_BUFFER_SIZE * sizeof(int16_t));
        if (!g_audio_buffers[i]) return false;
        memset(g_audio_buffers[i], 0, AUDIO_BUFFER_SIZE * sizeof(int16_t));
    }

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = (DWORD)AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * 2;

    if (waveOutOpen(&g_waveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        return false;
    }

    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        g_headers[i].lpData = (LPSTR)g_audio_buffers[i];
        g_headers[i].dwBufferLength = AUDIO_BUFFER_SIZE * sizeof(int16_t);
        waveOutPrepareHeader(g_waveOut, &g_headers[i], sizeof(WAVEHDR));
    }

    g_audio_running = true;
    return true;
}

static void audio_write(const int16_t *samples, uint32_t count) {
    if (!g_audio_running || count == 0) return;

    EnterCriticalSection(&g_audio_cs);

    WAVEHDR *hdr = &g_headers[g_current_buffer];

    /* Wait if buffer busy */
    while (hdr->dwFlags & WHDR_INQUEUE) {
        LeaveCriticalSection(&g_audio_cs);
        Sleep(1);
        EnterCriticalSection(&g_audio_cs);
    }

    uint32_t to_copy = (count > AUDIO_BUFFER_SIZE) ? AUDIO_BUFFER_SIZE : count;
    memcpy(g_audio_buffers[g_current_buffer], samples, to_copy * sizeof(int16_t));
    hdr->dwBufferLength = to_copy * sizeof(int16_t);

    waveOutWrite(g_waveOut, hdr, sizeof(WAVEHDR));
    g_current_buffer = (g_current_buffer + 1) % AUDIO_BUFFERS;

    LeaveCriticalSection(&g_audio_cs);
}

static void audio_close(void) {
    if (!g_audio_running) return;
    g_audio_running = false;

    waveOutReset(g_waveOut);
    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        waveOutUnprepareHeader(g_waveOut, &g_headers[i], sizeof(WAVEHDR));
        free(g_audio_buffers[i]);
    }
    waveOutClose(g_waveOut);
    DeleteCriticalSection(&g_audio_cs);
}
#endif

/*============================================================================
 * Global State
 *============================================================================*/

static volatile bool g_running = true;
static sdrplay_api_DeviceT g_device;
static sdrplay_api_DeviceParamsT *g_params = NULL;

/* DSP state */
static lowpass_t g_lowpass;
static dc_block_t g_dc_block;
static int g_decim_counter = 0;

/* Audio output buffer */
static int16_t g_audio_out[8192];
static int g_audio_out_count = 0;

/* Volume */
static float g_volume = 50.0f;

/*============================================================================
 * SDRplay Stream Callback
 *============================================================================*/

static void stream_callback(
    short *xi,
    short *xq,
    sdrplay_api_StreamCbParamsT *params,
    unsigned int numSamples,
    unsigned int reset,
    void *cbContext
) {
    (void)params;
    (void)reset;
    (void)cbContext;

    if (!g_running) return;

    for (unsigned int i = 0; i < numSamples; i++) {
        /* Step 1: Get IQ sample */
        float I = (float)xi[i];
        float Q = (float)xq[i];

        /* Step 2: Envelope detection - magnitude = sqrt(I² + Q²) */
        float magnitude = sqrtf(I * I + Q * Q);

        /* Step 3: Anti-aliasing lowpass filter (2.5 kHz) */
        float filtered = lowpass_process(&g_lowpass, magnitude);

        /* Step 4: Decimation (keep every 42nd sample) */
        g_decim_counter++;
        if (g_decim_counter >= DECIMATION_FACTOR) {
            g_decim_counter = 0;

            /* Step 5: DC removal */
            float audio = dc_block_process(&g_dc_block, filtered);

            /* Scale to audio level */
            audio = audio * g_volume;

            /* Clip */
            if (audio > 32767.0f) audio = 32767.0f;
            if (audio < -32768.0f) audio = -32768.0f;

            /* Store in output buffer */
            g_audio_out[g_audio_out_count++] = (int16_t)audio;

            /* Step 6: Output to speakers when buffer full */
            if (g_audio_out_count >= AUDIO_BUFFER_SIZE) {
                audio_write(g_audio_out, g_audio_out_count);
                g_audio_out_count = 0;
            }
        }
    }
}

static void stream_callback_b(
    short *xi, short *xq,
    sdrplay_api_StreamCbParamsT *params,
    unsigned int numSamples, unsigned int reset, void *cbContext
) {
    (void)xi; (void)xq; (void)params;
    (void)numSamples; (void)reset; (void)cbContext;
}

static void event_callback(
    sdrplay_api_EventT eventId,
    sdrplay_api_TunerSelectT tuner,
    sdrplay_api_EventParamsT *params,
    void *cbContext
) {
    (void)cbContext;

    if (eventId == sdrplay_api_PowerOverloadChange) {
        /* Must acknowledge overload */
        sdrplay_api_Update(g_device.dev, tuner,
                           sdrplay_api_Update_Ctrl_OverloadMsgAck,
                           sdrplay_api_Update_Ext1_None);
        if (params->powerOverloadParams.powerOverloadChangeType == sdrplay_api_Overload_Detected) {
            printf("[OVERLOAD]\n");
        }
    }
}

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    printf("\nStopping...\n");
    g_running = false;
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    sdrplay_api_ErrT err;
    float freq_mhz = DEFAULT_FREQ_MHZ;
    int gain_db = DEFAULT_GAIN_DB;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            freq_mhz = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            gain_db = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            g_volume = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("Simple AM Receiver for WWV\n");
            printf("Usage: %s [-f freq_mhz] [-g gain_db] [-v volume]\n", argv[0]);
            printf("  -f  Frequency in MHz (default: %.1f)\n", DEFAULT_FREQ_MHZ);
            printf("  -g  Gain reduction 20-59 dB (default: %d)\n", DEFAULT_GAIN_DB);
            printf("  -v  Volume (default: %.1f)\n", g_volume);
            return 0;
        }
    }

    printf("Simple AM Receiver\n");
    printf("Frequency: %.3f MHz\n", freq_mhz);
    printf("Gain reduction: %d dB\n", gain_db);
    printf("Volume: %.1f\n\n", g_volume);

    signal(SIGINT, signal_handler);

    /* Initialize DSP */
    lowpass_init(&g_lowpass, AUDIO_BANDWIDTH, SDR_SAMPLE_RATE);
    dc_block_init(&g_dc_block);

    /* Initialize audio */
    if (!audio_init()) {
        fprintf(stderr, "Failed to initialize audio\n");
        return 1;
    }
    printf("Audio initialized (%.0f Hz)\n", AUDIO_SAMPLE_RATE);

    /* Open SDRplay API */
    err = sdrplay_api_Open();
    if (err != sdrplay_api_Success) {
        fprintf(stderr, "sdrplay_api_Open failed: %s\n", sdrplay_api_GetErrorString(err));
        audio_close();
        return 1;
    }

    /* Get devices */
    sdrplay_api_DeviceT devices[4];
    unsigned int numDevs = 0;
    err = sdrplay_api_GetDevices(devices, &numDevs, 4);
    if (err != sdrplay_api_Success || numDevs == 0) {
        fprintf(stderr, "No SDRplay devices found\n");
        sdrplay_api_Close();
        audio_close();
        return 1;
    }

    g_device = devices[0];
    printf("Device: %s\n", g_device.SerNo);

    /* Select device */
    err = sdrplay_api_SelectDevice(&g_device);
    if (err != sdrplay_api_Success) {
        fprintf(stderr, "sdrplay_api_SelectDevice failed: %s\n", sdrplay_api_GetErrorString(err));
        sdrplay_api_Close();
        audio_close();
        return 1;
    }

    /* Get device parameters */
    err = sdrplay_api_GetDeviceParams(g_device.dev, &g_params);
    if (err != sdrplay_api_Success || !g_params) {
        fprintf(stderr, "sdrplay_api_GetDeviceParams failed\n");
        sdrplay_api_ReleaseDevice(&g_device);
        sdrplay_api_Close();
        audio_close();
        return 1;
    }

    /* Configure device */
    g_params->devParams->fsFreq.fsHz = SDR_SAMPLE_RATE;

    sdrplay_api_RxChannelParamsT *ch = g_params->rxChannelA;
    ch->tunerParams.rfFreq.rfHz = freq_mhz * 1e6;
    ch->tunerParams.bwType = sdrplay_api_BW_0_600;  /* 600 kHz BW */
    ch->tunerParams.ifType = sdrplay_api_IF_Zero;   /* Zero-IF */
    ch->tunerParams.gain.gRdB = gain_db;
    ch->tunerParams.gain.LNAstate = 0;

    ch->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    ch->ctrlParams.dcOffset.DCenable = 1;
    ch->ctrlParams.dcOffset.IQenable = 1;

    /* RSP2: Use Hi-Z AM port for HF */
    ch->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_1;
    ch->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A;

    printf("Configured: %.3f MHz, BW=600 kHz, IF=Zero, Gain=%d dB\n",
           freq_mhz, gain_db);

    /* Set up callbacks */
    sdrplay_api_CallbackFnsT callbacks;
    callbacks.StreamACbFn = stream_callback;
    callbacks.StreamBCbFn = stream_callback_b;
    callbacks.EventCbFn = event_callback;

    /* Start streaming */
    err = sdrplay_api_Init(g_device.dev, &callbacks, NULL);
    if (err != sdrplay_api_Success) {
        fprintf(stderr, "sdrplay_api_Init failed: %s\n", sdrplay_api_GetErrorString(err));
        sdrplay_api_ReleaseDevice(&g_device);
        sdrplay_api_Close();
        audio_close();
        return 1;
    }

    printf("\nListening... (Ctrl+C to stop)\n");

    /* Run until interrupted */
    while (g_running) {
        sleep_ms(100);
    }

    /* Cleanup */
    sdrplay_api_Uninit(g_device.dev);
    sdrplay_api_ReleaseDevice(&g_device);
    sdrplay_api_Close();
    audio_close();

    printf("Done.\n");
    return 0;
}
