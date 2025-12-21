/**
 * @file simple_am_receiver.c
 * @brief Simple AM Receiver for WWV
 *
 * Tuning Strategy:
 * - Zero-IF mode with +450 Hz offset (e.g., tune 10.000450 MHz for 10 MHz)
 * - This avoids the DC hole (a few Hz notch at exactly 0 Hz)
 * - 450 Hz offset is negligible compared to 3 kHz audio bandwidth
 *
 * DSP Pipeline:
 * 1. Receive IQ samples from SDRplay callback (short *xi, short *xq)
 * 2. Lowpass filter I and Q separately (isolate signal at DC, reject off-center stations)
 * 3. Envelope detection: magnitude = sqrt(I² + Q²)
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
#include "version.h"

#ifdef _WIN32
#include <Windows.h>
#include <mmsystem.h>
#include <io.h>
#include <fcntl.h>
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
#define IQ_FILTER_CUTOFF    3000.0      /* 3 kHz lowpass on I/Q before magnitude */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    float y = x - dc->x_prev + 0.99f * dc->y_prev;  /* 0.99 for voice (was 0.995 for pulse detection) */
    dc->x_prev = x;
    dc->y_prev = y;
    return y;
}

/*============================================================================
 * Audio AGC (Automatic Gain Control)
 *============================================================================*/

typedef struct {
    float level;        /* Running average of signal level */
    float target;       /* Target output level */
    float attack;       /* Attack time constant (fast) */
    float decay;        /* Decay time constant (slow) */
    int warmup;         /* Warmup counter */
} audio_agc_t;

static void audio_agc_init(audio_agc_t *agc, float target) {
    agc->level = 0.0001f;
    agc->target = target;
    agc->attack = 0.01f;   /* Fast attack for loud signals */
    agc->decay = 0.0001f;  /* Slow decay for quiet signals */
    agc->warmup = 0;
}

static float audio_agc_process(audio_agc_t *agc, float x) {
    float mag = fabsf(x);

    /* Track signal level with asymmetric time constants */
    if (mag > agc->level) {
        agc->level += agc->attack * (mag - agc->level);  /* Fast attack */
    } else {
        agc->level += agc->decay * (mag - agc->level);   /* Slow decay */
    }

    /* Prevent division by zero */
    if (agc->level < 0.0001f) agc->level = 0.0001f;

    /* Calculate gain to reach target level */
    float gain = agc->target / agc->level;

    /* Limit gain to prevent over-amplification during silence */
    if (gain > 100.0f) gain = 100.0f;
    if (gain < 0.1f) gain = 0.1f;

    /* Apply gain */
    return x * gain;
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
static lowpass_t g_lowpass_i;   /* Lowpass for I channel */
static lowpass_t g_lowpass_q;   /* Lowpass for Q channel */
static dc_block_t g_dc_block;
static audio_agc_t g_audio_agc;
static int g_decim_counter = 0;

/* Audio output buffer */
static int16_t g_audio_out[8192];
static int g_audio_out_count = 0;

/* Volume */
static float g_volume = 50.0f;

/* Output modes - can both be enabled */
static bool g_stdout_mode = false;  /* true = also output PCM to stdout (for waterfall) */
static bool g_audio_enabled = true; /* true = output to speakers */

/* Diagnostic output - goes to stderr in stdout mode */
#define LOG(...) fprintf(g_stdout_mode ? stderr : stdout, __VA_ARGS__)

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

        /* Step 2: Lowpass filter I and Q separately
         * This isolates the signal at DC (our tuned frequency)
         * and rejects off-center stations within the bandwidth */
        float I_filt = lowpass_process(&g_lowpass_i, I);
        float Q_filt = lowpass_process(&g_lowpass_q, Q);

        /* Step 3: Envelope detection on filtered signal */
        float magnitude = sqrtf(I_filt * I_filt + Q_filt * Q_filt);

        /* Step 4: DC removal (BEFORE decimation - keeps modulation clean) */
        float audio = dc_block_process(&g_dc_block, magnitude);

        /* Step 5: Audio AGC (automatic gain control for consistent volume) */
        audio = audio_agc_process(&g_audio_agc, audio);

        /* Step 6: Decimation (keep every 42nd sample) */
        g_decim_counter++;
        if (g_decim_counter >= DECIMATION_FACTOR) {
            g_decim_counter = 0;

            /* Scale to audio level */
            audio = audio * g_volume;

            /* Clip */
            if (audio > 32767.0f) audio = 32767.0f;
            if (audio < -32768.0f) audio = -32768.0f;

            /* Store in output buffer */
            g_audio_out[g_audio_out_count++] = (int16_t)audio;

            /* Step 7: Output when buffer full */
            if (g_audio_out_count >= AUDIO_BUFFER_SIZE) {
                if (g_stdout_mode) {
                    fwrite(g_audio_out, sizeof(int16_t), g_audio_out_count, stdout);
                    fflush(stdout);
                }
                if (g_audio_enabled) {
                    audio_write(g_audio_out, g_audio_out_count);
                }
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
            LOG("[OVERLOAD]\n");
        }
    }
}

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    LOG("\nStopping...\n");
    g_running = false;
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    print_version("Phoenix SDR - AM Receiver");

    sdrplay_api_ErrT err;
    float freq_mhz = DEFAULT_FREQ_MHZ;
    int gain_db = DEFAULT_GAIN_DB;
    int lna_state = 0;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            freq_mhz = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            gain_db = atoi(argv[++i]);
            if (gain_db < 20) gain_db = 20;
            if (gain_db > 59) gain_db = 59;
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            lna_state = atoi(argv[++i]);
            if (lna_state < 0) lna_state = 0;
            if (lna_state > 4) lna_state = 4;
        } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            g_volume = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "-o") == 0) {
            g_stdout_mode = true;
        } else if (strcmp(argv[i], "-a") == 0) {
            g_audio_enabled = false;  /* -a = disable audio (mute) */
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("Simple AM Receiver for WWV\n");
            printf("Usage: %s [-f freq_mhz] [-g gain_db] [-l lna_state] [-v volume] [-o] [-a]\n", argv[0]);
            printf("  -f  Frequency in MHz (default: %.1f)\n", DEFAULT_FREQ_MHZ);
            printf("      Tip: Offset by 450 Hz to avoid DC hole (e.g., 10.000450 for 10 MHz)\n");
            printf("  -g  Gain reduction 20-59 dB (default: %d)\n", DEFAULT_GAIN_DB);
            printf("  -l  LNA state 0-4 for Hi-Z port (default: 0)\n");
            printf("  -v  Volume (default: %.1f)\n", g_volume);
            printf("  -o  Output raw PCM to stdout (for waterfall)\n");
            printf("  -a  Mute audio (disable speakers)\n");
            printf("\nRF bandwidth: 6 kHz (fixed)\n");
            return 0;
        }
    }

    LOG("Simple AM Receiver\n");
    LOG("Frequency: %.3f MHz\n", freq_mhz);
    LOG("Gain reduction: %d dB\n", gain_db);
    LOG("LNA state: %d\n", lna_state);
    LOG("RF bandwidth: 6 kHz (fixed)\n");
    LOG("Audio: %s\n", g_audio_enabled ? "speakers" : "muted");
    LOG("Waterfall: %s\n", g_stdout_mode ? "stdout (raw PCM)" : "off");
    LOG("Volume: %.1f\n\n", g_volume);

    signal(SIGINT, signal_handler);

    /* Initialize DSP - lowpass I and Q at 3 kHz (gives 6 kHz RF bandwidth) */
    lowpass_init(&g_lowpass_i, IQ_FILTER_CUTOFF, SDR_SAMPLE_RATE);
    lowpass_init(&g_lowpass_q, IQ_FILTER_CUTOFF, SDR_SAMPLE_RATE);
    dc_block_init(&g_dc_block);
    audio_agc_init(&g_audio_agc, 5000.0f);  /* Target level for 16-bit audio */

    /* Initialize audio if enabled */
    if (g_audio_enabled) {
        if (!audio_init()) {
            fprintf(stderr, "Failed to initialize audio\n");
            return 1;
        }
        LOG("Audio initialized (%.0f Hz)\n", AUDIO_SAMPLE_RATE);
    }

    /* Set up stdout for PCM if waterfall mode */
    if (g_stdout_mode) {
        LOG("PCM output: 48000 Hz, 16-bit signed, mono\n");
#ifdef _WIN32
        /* Set stdout to binary mode on Windows */
        _setmode(_fileno(stdout), _O_BINARY);
#endif
    }

    /* Open SDRplay API */
    err = sdrplay_api_Open();
    if (err != sdrplay_api_Success) {
        fprintf(stderr, "sdrplay_api_Open failed: %s\n", sdrplay_api_GetErrorString(err));
        if (g_audio_enabled) audio_close();
        return 1;
    }

    /* Get devices */
    sdrplay_api_DeviceT devices[4];
    unsigned int numDevs = 0;
    err = sdrplay_api_GetDevices(devices, &numDevs, 4);
    if (err != sdrplay_api_Success || numDevs == 0) {
        fprintf(stderr, "No SDRplay devices found\n");
        sdrplay_api_Close();
        if (g_audio_enabled) audio_close();
        return 1;
    }

    g_device = devices[0];
    LOG("Device: %s\n", g_device.SerNo);

    /* Select device */
    err = sdrplay_api_SelectDevice(&g_device);
    if (err != sdrplay_api_Success) {
        fprintf(stderr, "sdrplay_api_SelectDevice failed: %s\n", sdrplay_api_GetErrorString(err));
        sdrplay_api_Close();
        if (g_audio_enabled) audio_close();
        return 1;
    }

    /* Get device parameters */
    err = sdrplay_api_GetDeviceParams(g_device.dev, &g_params);
    if (err != sdrplay_api_Success || !g_params) {
        fprintf(stderr, "sdrplay_api_GetDeviceParams failed\n");
        sdrplay_api_ReleaseDevice(&g_device);
        sdrplay_api_Close();
        if (g_audio_enabled) audio_close();
        return 1;
    }

    /* Configure device */
    g_params->devParams->fsFreq.fsHz = SDR_SAMPLE_RATE;

    sdrplay_api_RxChannelParamsT *ch = g_params->rxChannelA;
    ch->tunerParams.rfFreq.rfHz = freq_mhz * 1e6;
    ch->tunerParams.bwType = sdrplay_api_BW_0_200;  /* Minimum BW; I/Q filter is tighter */
    ch->tunerParams.ifType = sdrplay_api_IF_Zero;  /* Zero-IF mode */
    ch->tunerParams.gain.gRdB = gain_db;
    ch->tunerParams.gain.LNAstate = (unsigned char)lna_state;

    ch->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    ch->ctrlParams.dcOffset.DCenable = 1;
    ch->ctrlParams.dcOffset.IQenable = 1;

    /* RSP2: Use Hi-Z AM port for HF */
    ch->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_1;
    ch->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A;

    LOG("Configured: %.3f MHz, RF BW=6 kHz, Gain=%d dB, LNA=%d\n",
           freq_mhz, gain_db, lna_state);

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
        if (g_audio_enabled) audio_close();
        return 1;
    }

    LOG("\nListening... (Ctrl+C to stop)\n");

    /* Run until interrupted */
    while (g_running) {
        sleep_ms(100);
    }

    /* Cleanup */
    sdrplay_api_Uninit(g_device.dev);
    sdrplay_api_ReleaseDevice(&g_device);
    sdrplay_api_Close();
    if (g_audio_enabled) audio_close();

    LOG("Done.\n");
    return 0;
}
