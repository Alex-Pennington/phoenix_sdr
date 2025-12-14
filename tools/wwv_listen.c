/**
 * @file wwv_listen.c
 * @brief Simple WWV AM receiver - SDR to speakers
 *
 * Minimal tool to verify SDR signal chain by listening to WWV.
 * If you can hear the voice announcements, the SDR is working.
 *
 * Usage: wwv_listen [-f freq_mhz] [-g gain] [-d duration]
 */

#include "phoenix_sdr.h"
#include "decimator.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

#define SAMPLE_RATE_HZ      2000000.0
#define DECIMATED_RATE_HZ   48000.0
#define BANDWIDTH_KHZ       200
#define DEFAULT_FREQ_MHZ    15.0
#define DEFAULT_GAIN        40
#define DEFAULT_DURATION    60

/* Audio output */
#define AUDIO_BUFFERS       4
#define AUDIO_BUFFER_SIZE   4096

/*============================================================================
 * Audio Output (inline Windows implementation)
 *============================================================================*/

#ifdef _WIN32
typedef struct {
    HWAVEOUT    hWaveOut;
    WAVEHDR     headers[AUDIO_BUFFERS];
    int16_t    *buffers[AUDIO_BUFFERS];
    int         currentBuffer;
    bool        running;
    CRITICAL_SECTION cs;
} audio_out_t;

static audio_out_t g_audio;

static bool audio_init(double sample_rate) {
    memset(&g_audio, 0, sizeof(g_audio));
    InitializeCriticalSection(&g_audio.cs);

    /* Allocate buffers */
    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        g_audio.buffers[i] = (int16_t *)malloc(AUDIO_BUFFER_SIZE * sizeof(int16_t));
        if (!g_audio.buffers[i]) {
            fprintf(stderr, "Failed to allocate audio buffer\n");
            return false;
        }
        memset(g_audio.buffers[i], 0, AUDIO_BUFFER_SIZE * sizeof(int16_t));
    }

    /* Open audio device */
    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = (DWORD)sample_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT result = waveOutOpen(&g_audio.hWaveOut, WAVE_MAPPER, &wfx,
                                   0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        fprintf(stderr, "waveOutOpen failed: %d\n", result);
        return false;
    }

    /* Prepare headers */
    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        g_audio.headers[i].lpData = (LPSTR)g_audio.buffers[i];
        g_audio.headers[i].dwBufferLength = AUDIO_BUFFER_SIZE * sizeof(int16_t);
        waveOutPrepareHeader(g_audio.hWaveOut, &g_audio.headers[i], sizeof(WAVEHDR));
    }

    g_audio.running = true;
    printf("Audio output initialized (%.0f Hz mono)\n", sample_rate);
    return true;
}

static void audio_write(const int16_t *samples, uint32_t count) {
    if (!g_audio.running || count == 0) return;

    EnterCriticalSection(&g_audio.cs);

    uint32_t written = 0;
    while (written < count && g_audio.running) {
        WAVEHDR *hdr = &g_audio.headers[g_audio.currentBuffer];

        /* Wait for buffer if in use */
        int wait_count = 0;
        while ((hdr->dwFlags & WHDR_INQUEUE) && wait_count < 100) {
            LeaveCriticalSection(&g_audio.cs);
            Sleep(1);
            EnterCriticalSection(&g_audio.cs);
            wait_count++;
        }

        if (hdr->dwFlags & WHDR_INQUEUE) {
            break;  /* Give up if still busy */
        }

        /* Copy samples */
        uint32_t toCopy = count - written;
        if (toCopy > AUDIO_BUFFER_SIZE) toCopy = AUDIO_BUFFER_SIZE;

        memcpy(g_audio.buffers[g_audio.currentBuffer], samples + written,
               toCopy * sizeof(int16_t));
        hdr->dwBufferLength = toCopy * sizeof(int16_t);

        waveOutWrite(g_audio.hWaveOut, hdr, sizeof(WAVEHDR));

        written += toCopy;
        g_audio.currentBuffer = (g_audio.currentBuffer + 1) % AUDIO_BUFFERS;
    }

    LeaveCriticalSection(&g_audio.cs);
}

static void audio_close(void) {
    if (!g_audio.running) return;
    g_audio.running = false;

    waveOutReset(g_audio.hWaveOut);
    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        waveOutUnprepareHeader(g_audio.hWaveOut, &g_audio.headers[i], sizeof(WAVEHDR));
        free(g_audio.buffers[i]);
    }
    waveOutClose(g_audio.hWaveOut);
    DeleteCriticalSection(&g_audio.cs);
    printf("Audio output closed\n");
}
#else
/* Stub for non-Windows */
static bool audio_init(double sr) { (void)sr; fprintf(stderr, "Audio not supported\n"); return false; }
static void audio_write(const int16_t *s, uint32_t c) { (void)s; (void)c; }
static void audio_close(void) {}
#endif

/*============================================================================
 * Global State
 *============================================================================*/

static volatile bool g_running = true;
static decim_state_t *g_decimator = NULL;
static decim_complex_t g_decim_buffer[8192];

/* Audio conversion buffer */
static int16_t g_audio_buffer[8192];

/* Volume/gain control */
static float g_volume = 10.0f;  /* Amplification factor */

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    printf("\nStopping...\n");
    g_running = false;
}

/*============================================================================
 * SDR Callback - AM demodulation to audio
 *============================================================================*/

static void on_samples(
    const int16_t *xi,
    const int16_t *xq,
    uint32_t count,
    bool reset,
    void *user_ctx
) {
    (void)user_ctx;

    if (reset && g_decimator) {
        decim_reset(g_decimator);
    }

    if (!g_running) return;

    /* Decimate to 48kHz */
    size_t out_count = 0;
    decim_error_t err = decim_process_int16(
        g_decimator, xi, xq, count,
        g_decim_buffer, sizeof(g_decim_buffer)/sizeof(g_decim_buffer[0]),
        &out_count
    );

    if (err != DECIM_OK || out_count == 0) return;

    /* AM demodulation: envelope = sqrt(I² + Q²) */
    for (size_t i = 0; i < out_count; i++) {
        float I = g_decim_buffer[i].i;
        float Q = g_decim_buffer[i].q;
        float envelope = sqrtf(I*I + Q*Q);

        /* Scale to audio level with volume control */
        float audio = envelope * g_volume * 32767.0f;

        /* Clip to prevent distortion */
        if (audio > 32767.0f) audio = 32767.0f;
        if (audio < -32768.0f) audio = -32768.0f;

        g_audio_buffer[i] = (int16_t)audio;
    }

    /* Send to speakers */
    audio_write(g_audio_buffer, (uint32_t)out_count);
}

static void on_gain_change(double gain_db, int lna_db, void *user_ctx) {
    (void)user_ctx;
    printf("  Gain: %.1f dB (LNA: %d dB)\n", gain_db, lna_db);
}

static void on_overload(bool overloaded, void *user_ctx) {
    (void)user_ctx;
    if (overloaded) {
        printf("  [OVERLOAD]\n");
    }
}

/*============================================================================
 * Usage
 *============================================================================*/

static void print_usage(const char *prog) {
    printf("WWV AM Receiver - Listen to WWV through speakers\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -f <MHz>      Frequency (default: %.1f)\n", DEFAULT_FREQ_MHZ);
    printf("  -g <dB>       Gain reduction 20-59 (default: %d)\n", DEFAULT_GAIN);
    printf("  -v <factor>   Volume multiplier (default: %.1f)\n", g_volume);
    printf("  -d <sec>      Duration in seconds (default: %d, 0=forever)\n", DEFAULT_DURATION);
    printf("  -h            Show this help\n\n");
    printf("WWV Frequencies: 2.5, 5.0, 10.0, 15.0, 20.0, 25.0 MHz\n");
    printf("15 MHz is usually strongest during daytime.\n");
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    double freq_mhz = DEFAULT_FREQ_MHZ;
    int gain = DEFAULT_GAIN;
    int duration = DEFAULT_DURATION;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            freq_mhz = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            gain = atoi(argv[++i]);
            if (gain < 20) gain = 20;
            if (gain > 59) gain = 59;
        }
        else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            g_volume = (float)atof(argv[++i]);
        }
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration = atoi(argv[++i]);
        }
    }

    printf("========================================\n");
    printf("WWV AM Receiver\n");
    printf("========================================\n");
    printf("Frequency: %.3f MHz\n", freq_mhz);
    printf("Gain reduction: %d dB\n", gain);
    printf("Volume: %.1fx\n", g_volume);
    printf("Duration: %s\n", duration > 0 ? "limited" : "unlimited");
    printf("========================================\n\n");

    /* Set up signal handler */
    signal(SIGINT, signal_handler);

    /* Initialize audio output */
    if (!audio_init(DECIMATED_RATE_HZ)) {
        fprintf(stderr, "Failed to initialize audio\n");
        return 1;
    }

    /* Initialize decimator */
    decim_error_t derr = decim_create(&g_decimator, SAMPLE_RATE_HZ, DECIMATED_RATE_HZ);
    if (derr != DECIM_OK) {
        fprintf(stderr, "Failed to create decimator\n");
        audio_close();
        return 1;
    }
    printf("Decimator: %.0f Hz -> %.0f Hz\n", SAMPLE_RATE_HZ, DECIMATED_RATE_HZ);

    /* Open SDR */
    psdr_context_t *sdr = NULL;
    psdr_error_t perr = psdr_open(&sdr, 0);  /* Device index 0 */
    if (perr != PSDR_OK) {
        fprintf(stderr, "Failed to open SDR: %d\n", perr);
        decim_destroy(g_decimator);
        audio_close();
        return 1;
    }

    /* Configure SDR */
    psdr_config_t config;
    psdr_config_defaults(&config);
    config.freq_hz = freq_mhz * 1e6;
    config.sample_rate_hz = SAMPLE_RATE_HZ;
    config.bandwidth = BANDWIDTH_KHZ;
    config.agc_mode = PSDR_AGC_DISABLED;
    config.gain_reduction = gain;
    config.lna_state = 0;
    config.antenna = PSDR_ANT_HIZ;  /* Hi-Z for HF */

    perr = psdr_configure(sdr, &config);
    if (perr != PSDR_OK) {
        fprintf(stderr, "Failed to configure SDR: %d\n", perr);
        psdr_close(sdr);
        decim_destroy(g_decimator);
        audio_close();
        return 1;
    }
    printf("SDR configured: %.3f MHz, %d kHz BW\n\n", freq_mhz, BANDWIDTH_KHZ);

    /* Start streaming */
    psdr_callbacks_t callbacks = {
        .on_samples = on_samples,
        .on_gain_change = on_gain_change,
        .on_overload = on_overload,
        .user_ctx = NULL
    };

    perr = psdr_start(sdr, &callbacks);
    if (perr != PSDR_OK) {
        fprintf(stderr, "Failed to start streaming: %d\n", perr);
        psdr_close(sdr);
        decim_destroy(g_decimator);
        audio_close();
        return 1;
    }

    printf("Listening... (Ctrl+C to stop)\n");
    printf("You should hear WWV time announcements and tick pulses.\n\n");

    /* Run for duration or until interrupted */
    int elapsed = 0;
    while (g_running) {
        sleep_ms(1000);
        elapsed++;

        if (duration > 0 && elapsed >= duration) {
            printf("\nDuration reached.\n");
            break;
        }

        /* Print status every 10 seconds */
        if (elapsed % 10 == 0) {
            printf("  [%d sec]\n", elapsed);
        }
    }

    /* Cleanup */
    printf("\nShutting down...\n");
    psdr_stop(sdr);
    psdr_close(sdr);
    decim_destroy(g_decimator);
    audio_close();

    printf("Done.\n");
    return 0;
}
