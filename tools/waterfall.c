/*******************************************************************************
 * FROZEN FILE - DO NOT MODIFY (except to integrate modules)
 * See .github/copilot-instructions.md P1 section
 *
 * DSP processing is in waterfall_dsp.c (FROZEN)
 * Audio output is in waterfall_audio.c (can modify)
 ******************************************************************************/

/**
 * @file waterfall.c
 * @brief Simple waterfall display for audio PCM input
 *
 * Reads 16-bit signed mono PCM from stdin OR I/Q samples from TCP.
 * Usage (stdin):  simple_am_receiver.exe -f 10 -i -o | waterfall.exe
 * Usage (TCP):    waterfall.exe --tcp localhost:4535
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include "kiss_fft.h"
#include "version.h"
#include "waterfall_dsp.h"
#include "waterfall_audio.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define SOCKET_ERROR_VAL SOCKET_ERROR
#define socket_close closesocket
#define socket_errno WSAGetLastError()
#define EWOULDBLOCK_VAL WSAEWOULDBLOCK
#define ETIMEDOUT_VAL WSAETIMEDOUT
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define SOCKET_ERROR_VAL (-1)
#define socket_close close
#define socket_errno errno
#define EWOULDBLOCK_VAL EWOULDBLOCK
#define ETIMEDOUT_VAL ETIMEDOUT
#define Sleep(ms) usleep((ms) * 1000)
#endif

/* TCP recv result codes */
typedef enum {
    RECV_OK = 0,        /* Data received successfully */
    RECV_TIMEOUT,       /* No data available (timeout) */
    RECV_ERROR          /* Connection error or closed */
} recv_result_t;

/*============================================================================
 * TCP Configuration and Protocol
 *============================================================================*/

#define DEFAULT_IQ_PORT         4536

/* Binary protocol magic numbers */
#define MAGIC_PHXI  0x50485849  /* "PHXI" - Phoenix IQ header */
#define MAGIC_IQDQ  0x49514451  /* "IQDQ" - I/Q data frame */
#define MAGIC_META  0x4D455441  /* "META" - Metadata update */

/* Sample format codes */
#define IQ_FORMAT_S16   1       /* Interleaved int16 I, int16 Q */
#define IQ_FORMAT_F32   2       /* Interleaved float32 I, float32 Q */
#define IQ_FORMAT_U8    3       /* Interleaved uint8 I, uint8 Q */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;             /* MAGIC_PHXI */
    uint32_t version;           /* Protocol version (1) */
    uint32_t sample_rate;       /* Sample rate in Hz */
    uint32_t sample_format;     /* IQ_FORMAT_xxx */
    uint32_t center_freq_lo;    /* Center frequency low 32 bits */
    uint32_t center_freq_hi;    /* Center frequency high 32 bits */
    uint32_t reserved[2];       /* Future use */
} iq_stream_header_t;           /* 32 bytes */

typedef struct {
    uint32_t magic;             /* MAGIC_IQDQ */
    uint32_t sequence;          /* Frame sequence number */
    uint32_t num_samples;       /* Number of I/Q pairs */
    uint32_t flags;             /* Bit flags */
} iq_data_frame_t;              /* 16 bytes header */

typedef struct {
    uint32_t magic;             /* MAGIC_META */
    uint32_t sample_rate;       /* New sample rate */
    uint32_t sample_format;     /* New format */
    uint32_t center_freq_lo;    /* New freq low */
    uint32_t center_freq_hi;    /* New freq high */
    uint32_t reserved[3];       /* Future use */
} iq_metadata_update_t;         /* 32 bytes */
#pragma pack(pop)

/* TCP state */
static bool g_tcp_mode = true;  /* Default to TCP mode */
static bool g_stdin_mode = false;
static char g_tcp_host[256] = "localhost";
static int g_iq_port = DEFAULT_IQ_PORT;
static socket_t g_iq_sock = SOCKET_INVALID;
static uint32_t g_tcp_sample_rate = 2000000;
static uint32_t g_tcp_sample_format = IQ_FORMAT_S16;
static uint64_t g_tcp_center_freq = 15000000;
static bool g_tcp_streaming = false;

/* Decimation for high sample rate I/Q to display rate */
static int g_decimation_factor = 1;
static int g_decim_counter = 0;

/* DSP path instances - DISPLAY and AUDIO are separate (see P2 in copilot-instructions.md) */
static wf_dsp_path_t g_display_dsp;
static wf_dsp_path_t g_audio_dsp;

#define IQ_FILTER_CUTOFF    3000.0f     /* 3 kHz lowpass on I/Q before magnitude */

/*============================================================================
 * Configuration
 *============================================================================*/

/* Fixed signal processing parameters */
#define FFT_SIZE        1024    /* FFT size (512 usable bins) - FIXED */
#define SAMPLE_RATE     48000   /* Expected input sample rate */

/* Default window dimensions (runtime adjustable) */
#define DEFAULT_WATERFALL_WIDTH 800
#define DEFAULT_BUCKET_WIDTH    200
#define DEFAULT_WINDOW_HEIGHT   600

/* Runtime display dimensions */
static int g_waterfall_width = DEFAULT_WATERFALL_WIDTH;
static int g_bucket_width = DEFAULT_BUCKET_WIDTH;
static int g_window_width = DEFAULT_WATERFALL_WIDTH + DEFAULT_BUCKET_WIDTH;
static int g_window_height = DEFAULT_WINDOW_HEIGHT;

/* Frequency bins to monitor for WWV tick detection
 * Each has a center frequency and bandwidth based on signal characteristics:
 * - Pure tones (440, 500, 600 Hz): narrow bandwidth
 * - Short pulses (1000, 1200 Hz): wide bandwidth (energy spreads due to 5ms pulse)
 * - Subcarrier (100 Hz): tight to avoid DC noise
 * - Longer pulse (1500 Hz): moderate bandwidth (800ms pulse)
 */
#define NUM_TICK_FREQS  7
static const int   TICK_FREQS[NUM_TICK_FREQS] = { 100,   440,  500,  600,  1000, 1200, 1500 };
static const int   TICK_BW[NUM_TICK_FREQS]    = { 10,    5,    5,    5,    100,  100,  20   };  /* ± bandwidth in Hz */
static const char *TICK_NAMES[NUM_TICK_FREQS] = { "100Hz BCD", "440Hz Cal", "500Hz Min", "600Hz ID", "1000Hz Tick", "1200Hz WWVH", "1500Hz Tone" };

/*============================================================================
 * Color Mapping (magnitude to RGB) with auto-gain
 *============================================================================*/

/* Auto-gain state */
static float g_peak_db = -60.0f;      /* Tracked peak in dB */
static float g_floor_db = -60.0f;     /* Tracked noise floor in dB */
static float g_gain_offset = 0.0f;    /* Manual gain adjustment (+/- keys) */
#define AGC_ATTACK  0.1f              /* Fast attack for peaks */
#define AGC_DECAY   0.001f            /* Slow decay */

/* Tick detection state */
static float g_tick_thresholds[NUM_TICK_FREQS] = { 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f };
static float g_bucket_energy[NUM_TICK_FREQS];  /* Current energy in each bucket */
static int g_selected_param = 0;      /* 0 = gain, 1-6 = tick thresholds */

/* Effective sample rate (may differ in TCP mode) */
static int g_effective_sample_rate = SAMPLE_RATE;

/*============================================================================
 * TCP Helper Functions
 *============================================================================*/

static bool tcp_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

static void tcp_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

static socket_t tcp_connect(const char *host, int port) {
    struct addrinfo hints, *result, *rp;
    char port_str[16];
    socket_t sock = SOCKET_INVALID;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        fprintf(stderr, "Failed to resolve host: %s\n", host);
        return SOCKET_INVALID;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == SOCKET_INVALID) continue;

        if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
            break;  /* Success */
        }

        socket_close(sock);
        sock = SOCKET_INVALID;
    }

    freeaddrinfo(result);
    return sock;
}


/* Read exactly n bytes from socket - returns result code */
static recv_result_t tcp_recv_exact_ex(socket_t sock, void *buf, int n) {
    char *ptr = (char *)buf;
    int remaining = n;

    while (remaining > 0) {
        int received = recv(sock, ptr, remaining, 0);
        if (received > 0) {
            ptr += received;
            remaining -= received;
        } else if (received == 0) {
            /* Connection closed by peer */
            return RECV_ERROR;
        } else {
            /* Error - check if timeout */
            int err = socket_errno;
            if (err == EWOULDBLOCK_VAL || err == ETIMEDOUT_VAL) {
                return RECV_TIMEOUT;
            }
            return RECV_ERROR;
        }
    }
    return RECV_OK;
}

/* Read exactly n bytes from socket (legacy bool version for header) */
static bool tcp_recv_exact(socket_t sock, void *buf, int n) {
    return tcp_recv_exact_ex(sock, buf, n) == RECV_OK;
}

/* Parse --tcp host:port argument */
static bool parse_tcp_arg(const char *arg) {
    /* Format: host:port or just host (use default port) */
    char *colon = strchr(arg, ':');
    if (colon) {
        int host_len = (int)(colon - arg);
        if (host_len >= (int)sizeof(g_tcp_host)) host_len = sizeof(g_tcp_host) - 1;
        strncpy(g_tcp_host, arg, host_len);
        g_tcp_host[host_len] = '\0';
        g_iq_port = atoi(colon + 1);
    } else {
        strncpy(g_tcp_host, arg, sizeof(g_tcp_host) - 1);
        g_tcp_host[sizeof(g_tcp_host) - 1] = '\0';
    }
    return true;
}

/**
 * Attempt to reconnect to the server and re-read the stream header.
 * Returns true if successful, false if user should quit.
 */
static bool tcp_reconnect(void) {
    /* Close existing socket if any */
    if (g_iq_sock != SOCKET_INVALID) {
        socket_close(g_iq_sock);
        g_iq_sock = SOCKET_INVALID;
    }

    /* Reset DSP state for fresh start after reconnection */
    g_display_dsp.initialized = false;
    g_audio_dsp.initialized = false;
    g_decim_counter = 0;

    printf("\n*** CONNECTION LOST - Reconnecting to %s:%d ***\n", g_tcp_host, g_iq_port);

    /* Retry connection until successful */
    int retry_count = 0;
    while (g_iq_sock == SOCKET_INVALID) {
        g_iq_sock = tcp_connect(g_tcp_host, g_iq_port);
        if (g_iq_sock == SOCKET_INVALID) {
            retry_count++;
            if (retry_count == 1) {
                printf("Waiting for server...\n");
            } else if (retry_count % 10 == 0) {
                printf("Still waiting... (%d attempts)\n", retry_count);
            }
            Sleep(1000);  /* Wait 1 second before retry */

            /* Check for SDL quit events during reconnect */
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
                    printf("User quit during reconnect\n");
                    return false;
                }
            }
        }
    }

    printf("*** RECONNECTED to %s:%d ***\n", g_tcp_host, g_iq_port);

    /* Set socket timeout for header read (5s) */
#ifdef _WIN32
    DWORD timeout_ms = 5000;
    setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    /* Read stream header */
    iq_stream_header_t header;
    if (!tcp_recv_exact(g_iq_sock, &header, sizeof(header))) {
        fprintf(stderr, "Failed to read I/Q stream header after reconnect\n");
        socket_close(g_iq_sock);
        g_iq_sock = SOCKET_INVALID;
        return tcp_reconnect();  /* Try again */
    }

    if (header.magic != MAGIC_PHXI) {
        fprintf(stderr, "Invalid header magic after reconnect: 0x%08X\n", header.magic);
        socket_close(g_iq_sock);
        g_iq_sock = SOCKET_INVALID;
        return tcp_reconnect();  /* Try again */
    }

    g_tcp_sample_rate = header.sample_rate;
    g_tcp_sample_format = header.sample_format;
    g_tcp_center_freq = ((uint64_t)header.center_freq_hi << 32) | header.center_freq_lo;

    printf("Stream header: rate=%u Hz, format=%u, freq=%llu Hz\n",
           g_tcp_sample_rate, g_tcp_sample_format, (unsigned long long)g_tcp_center_freq);

    /* Recalculate decimation */
    g_decimation_factor = g_tcp_sample_rate / SAMPLE_RATE;
    if (g_decimation_factor < 1) g_decimation_factor = 1;

    /* Switch to short timeout for data streaming (100ms) */
#ifdef _WIN32
    timeout_ms = 100;
    setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    printf("Ready for I/Q data.\n\n");
    return true;
}

static void print_usage(const char *progname) {
    printf("Usage: %s [options]\n", progname);
    printf("  --tcp HOST[:PORT]   Connect to SDR server I/Q port (default localhost:%d)\n", DEFAULT_IQ_PORT);
    printf("  --stdin             Read from stdin instead of TCP\n");
    printf("  -h, --help          Show this help\n");
    printf("\nDefault: Connects to localhost:%d and retries until connected.\n", DEFAULT_IQ_PORT);
    printf("Start streaming via control port (4535) first.\n");
    printf("\nStdin mode: simple_am_receiver.exe -f 10 -i -o | %s --stdin\n", progname);
    printf("\nKeyboard Controls:\n");
    printf("  M            Mute/unmute audio\n");
    printf("  Up/Down      Volume up/down\n");
    printf("  +/-          Adjust gain (waterfall display)\n");
    printf("  0-7          Select parameter to adjust\n");
    printf("  D            Toggle tick detection\n");
    printf("  S            Print tick statistics\n");
    printf("  Q/ESC        Quit\n");
}

/*============================================================================
 * Tick Detector State Machine (watches 1000 Hz bucket)
 *============================================================================*/

#define FRAME_DURATION_MS ((float)FFT_SIZE * 1000.0f / SAMPLE_RATE)  /* ~21.3ms */
#define TICK_MIN_DURATION_MS    2
#define TICK_MAX_DURATION_MS    50
#define TICK_COOLDOWN_MS        500
#define TICK_NOISE_ADAPT_RATE   0.001f
#define TICK_WARMUP_ADAPT_RATE  0.05f
#define TICK_HYSTERESIS_RATIO   0.7f
#define TICK_WARMUP_FRAMES      50
#define TICK_FLASH_FRAMES       3       /* How long to show purple flash */
#define TICK_HISTORY_SIZE       30      /* Store last N tick timestamps for averaging */
#define TICK_AVG_WINDOW_MS      15000.0f /* 15 second averaging window */

#define MS_TO_FRAMES(ms) ((int)((ms) / FRAME_DURATION_MS + 0.5f))

typedef enum { TICK_IDLE, TICK_IN_TICK, TICK_COOLDOWN } tick_state_t;

typedef struct {
    tick_state_t state;
    float noise_floor;
    float threshold_high;
    float threshold_low;
    uint64_t tick_start_frame;
    float tick_peak_energy;
    int tick_duration_frames;
    int ticks_detected;
    int ticks_rejected;
    uint64_t last_tick_frame;
    uint64_t start_frame;
    int cooldown_frames;
    bool warmup_complete;
    bool detection_enabled;
    int flash_frames_remaining;  /* For purple flash */
    FILE *csv_file;
    /* Interval history for averaging */
    float tick_timestamps_ms[TICK_HISTORY_SIZE];  /* Circular buffer of tick times */
    int tick_history_idx;                          /* Next write position */
    int tick_history_count;                        /* Number of valid entries */
} tick_detector_t;

static tick_detector_t g_tick_detector;

static void tick_detector_init(tick_detector_t *td) {
    memset(td, 0, sizeof(*td));
    td->state = TICK_IDLE;
    td->noise_floor = 0.001f;
    td->threshold_high = td->noise_floor * 2.0f;
    td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;
    td->detection_enabled = true;
    td->flash_frames_remaining = 0;
    td->tick_history_idx = 0;
    td->tick_history_count = 0;
    td->csv_file = fopen("wwv_ticks.csv", "w");
    if (td->csv_file) {
        fprintf(td->csv_file, "timestamp_ms,tick_num,energy_peak,duration_ms,interval_ms,avg_interval_ms,noise_floor\n");
        fflush(td->csv_file);
    }
}

static void tick_detector_close(tick_detector_t *td) {
    if (td->csv_file) { fclose(td->csv_file); td->csv_file = NULL; }
}

/* Calculate average interval from ticks within the last 15 seconds */
static float tick_detector_avg_interval(tick_detector_t *td, float current_time_ms) {
    if (td->tick_history_count < 2) return 0.0f;

    float cutoff = current_time_ms - TICK_AVG_WINDOW_MS;
    float sum = 0.0f;
    int count = 0;
    float prev_time = -1.0f;

    /* Scan through history to find ticks within window */
    for (int i = 0; i < td->tick_history_count; i++) {
        int idx = (td->tick_history_idx - td->tick_history_count + i + TICK_HISTORY_SIZE) % TICK_HISTORY_SIZE;
        float t = td->tick_timestamps_ms[idx];
        if (t >= cutoff) {
            if (prev_time >= 0.0f) {
                sum += (t - prev_time);
                count++;
            }
            prev_time = t;
        }
    }

    return (count > 0) ? (sum / count) : 0.0f;
}

static bool tick_detector_update(tick_detector_t *td, float energy, uint64_t frame_num) {
    if (!td->detection_enabled) return false;
    bool tick_detected = false;

    /* Warmup */
    if (!td->warmup_complete) {
        td->noise_floor += TICK_WARMUP_ADAPT_RATE * (energy - td->noise_floor);
        if (td->noise_floor < 0.0001f) td->noise_floor = 0.0001f;
        td->threshold_high = td->noise_floor * 2.0f;
        td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;
        if (frame_num >= td->start_frame + TICK_WARMUP_FRAMES) {
            td->warmup_complete = true;
            printf("[WARMUP] Complete. Noise=%.6f, Thresh=%.6f\n", td->noise_floor, td->threshold_high);
        }
        return false;
    }

    /* Adapt noise floor during idle */
    if (td->state == TICK_IDLE && energy < td->threshold_high) {
        td->noise_floor += TICK_NOISE_ADAPT_RATE * (energy - td->noise_floor);
        if (td->noise_floor < 0.0001f) td->noise_floor = 0.0001f;
        td->threshold_high = td->noise_floor * 2.0f;
        td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;
    }

    switch (td->state) {
        case TICK_IDLE:
            if (energy > td->threshold_high) {
                td->state = TICK_IN_TICK;
                td->tick_start_frame = frame_num;
                td->tick_peak_energy = energy;
                td->tick_duration_frames = 1;
            }
            break;
        case TICK_IN_TICK:
            td->tick_duration_frames++;
            if (energy > td->tick_peak_energy) td->tick_peak_energy = energy;
            if (energy < td->threshold_low) {
                float duration_ms = td->tick_duration_frames * FRAME_DURATION_MS;
                if (duration_ms >= TICK_MIN_DURATION_MS && duration_ms <= TICK_MAX_DURATION_MS) {
                    td->ticks_detected++;
                    tick_detected = true;
                    td->flash_frames_remaining = TICK_FLASH_FRAMES;
                    float timestamp_ms = frame_num * FRAME_DURATION_MS;
                    float interval_ms = (td->last_tick_frame > 0) ?
                        (td->tick_start_frame - td->last_tick_frame) * FRAME_DURATION_MS : 0.0f;

                    /* Store timestamp in history buffer */
                    td->tick_timestamps_ms[td->tick_history_idx] = timestamp_ms;
                    td->tick_history_idx = (td->tick_history_idx + 1) % TICK_HISTORY_SIZE;
                    if (td->tick_history_count < TICK_HISTORY_SIZE) td->tick_history_count++;

                    /* Calculate average interval over last 15 seconds */
                    float avg_interval_ms = tick_detector_avg_interval(td, timestamp_ms);

                    char ind = (interval_ms > 950.0f && interval_ms < 1050.0f) ? ' ' : '!';
                    printf("[%7.1fs] TICK #%-4d  int=%6.0fms  avg=%6.0fms %c\n",
                           timestamp_ms/1000.0f, td->ticks_detected, interval_ms, avg_interval_ms, ind);
                    if (td->csv_file) {
                        fprintf(td->csv_file, "%.1f,%d,%.6f,%.1f,%.0f,%.0f,%.6f\n",
                                timestamp_ms, td->ticks_detected, td->tick_peak_energy, duration_ms, interval_ms, avg_interval_ms, td->noise_floor);
                        fflush(td->csv_file);
                    }
                    td->last_tick_frame = td->tick_start_frame;
                } else { td->ticks_rejected++; }
                td->state = TICK_COOLDOWN;
                td->cooldown_frames = MS_TO_FRAMES(TICK_COOLDOWN_MS);
            } else if (td->tick_duration_frames * FRAME_DURATION_MS > TICK_MAX_DURATION_MS) {
                td->ticks_rejected++;
                td->state = TICK_COOLDOWN;
                td->cooldown_frames = MS_TO_FRAMES(TICK_COOLDOWN_MS);
            }
            break;
        case TICK_COOLDOWN:
            if (--td->cooldown_frames <= 0) td->state = TICK_IDLE;
            break;
    }
    return tick_detected;
}

static void tick_detector_print_stats(tick_detector_t *td, uint64_t frame) {
    float elapsed = frame * FRAME_DURATION_MS / 1000.0f;
    float current_time_ms = frame * FRAME_DURATION_MS;
    float detecting = td->warmup_complete ? (elapsed - TICK_WARMUP_FRAMES * FRAME_DURATION_MS / 1000.0f) : 0.0f;
    int expected = (int)detecting;
    float rate = (expected > 0) ? (100.0f * td->ticks_detected / expected) : 0.0f;
    float avg_interval = tick_detector_avg_interval(td, current_time_ms);
    printf("\n=== TICK STATS ===\n");
    printf("Elapsed: %.1fs  Detected: %d  Expected: %d  Rate: %.1f%%\n", elapsed, td->ticks_detected, expected, rate);
    printf("Avg interval (15s): %.0fms  Rejected: %d  Noise: %.6f\n", avg_interval, td->ticks_rejected, td->noise_floor);
    printf("==================\n\n");
}

static void magnitude_to_rgb(float mag, float peak_db, float floor_db, uint8_t *r, uint8_t *g, uint8_t *b) {
    /* Log scale for better visibility */
    float db = 20.0f * log10f(mag + 1e-10f);

    /* Apply manual gain offset */
    db += g_gain_offset;

    /* Map dB to 0-1 range using tracked peak and floor */
    float range = peak_db - floor_db;
    if (range < 20.0f) range = 20.0f;  /* Minimum 20 dB range */

    float norm = (db - floor_db) / range;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;

    /* Blue -> Cyan -> Green -> Yellow -> Red */
    if (norm < 0.25f) {
        /* Black to Blue */
        *r = 0;
        *g = 0;
        *b = (uint8_t)(norm * 4.0f * 255.0f);
    } else if (norm < 0.5f) {
        /* Blue to Cyan */
        *r = 0;
        *g = (uint8_t)((norm - 0.25f) * 4.0f * 255.0f);
        *b = 255;
    } else if (norm < 0.75f) {
        /* Cyan to Yellow */
        *r = (uint8_t)((norm - 0.5f) * 4.0f * 255.0f);
        *g = 255;
        *b = (uint8_t)((0.75f - norm) * 4.0f * 255.0f);
    } else {
        /* Yellow to Red */
        *r = 255;
        *g = (uint8_t)((1.0f - norm) * 4.0f * 255.0f);
        *b = 0;
    }
}

/*============================================================================
 * FFT to Display Scaling
 *============================================================================*/

/**
 * Scale FFT magnitudes (FFT_SIZE bins, DC-centered) to display width.
 * Uses linear interpolation when display is smaller or larger than FFT.
 *
 * @param fft_mags      Input: FFT_SIZE magnitude values (DC at center)
 * @param display_mags  Output: g_waterfall_width magnitude values
 */
static void scale_fft_to_display(const float *fft_mags, float *display_mags) {
    float scale = (float)FFT_SIZE / (float)g_waterfall_width;

    for (int x = 0; x < g_waterfall_width; x++) {
        /* Map display pixel to FFT bin (floating point) */
        float fft_pos = x * scale;
        int bin_low = (int)fft_pos;
        int bin_high = bin_low + 1;
        float frac = fft_pos - bin_low;

        /* Clamp to valid range */
        if (bin_low < 0) bin_low = 0;
        if (bin_high >= FFT_SIZE) bin_high = FFT_SIZE - 1;
        if (bin_low >= FFT_SIZE) bin_low = FFT_SIZE - 1;

        /* Linear interpolation between adjacent bins */
        display_mags[x] = fft_mags[bin_low] * (1.0f - frac) + fft_mags[bin_high] * frac;
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    /* Parse command line */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tcp") == 0 && i + 1 < argc) {
            g_tcp_mode = true;
            g_stdin_mode = false;
            parse_tcp_arg(argv[++i]);
        } else if (strcmp(argv[i], "--stdin") == 0) {
            g_stdin_mode = true;
            g_tcp_mode = false;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    print_version("Phoenix SDR - Waterfall");

    /* TCP mode initialization */
    if (g_tcp_mode) {
        printf("TCP Mode: Connecting to %s:%d (I/Q data)\n",
               g_tcp_host, g_iq_port);

        if (!tcp_init()) {
            fprintf(stderr, "Failed to initialize networking\n");
            return 1;
        }

        /* Retry connection until successful */
        int retry_count = 0;
        while (g_iq_sock == SOCKET_INVALID) {
            g_iq_sock = tcp_connect(g_tcp_host, g_iq_port);
            if (g_iq_sock == SOCKET_INVALID) {
                retry_count++;
                if (retry_count == 1) {
                    printf("Waiting for server on %s:%d...\n", g_tcp_host, g_iq_port);
                } else if (retry_count % 10 == 0) {
                    printf("Still waiting... (%d attempts)\n", retry_count);
                }
                Sleep(1000);  /* Wait 1 second before retry */
            }
        }
        printf("\n*** CONNECTED to %s:%d ***\n\n", g_tcp_host, g_iq_port);

        /* Set socket timeout for header read (5s) */
#ifdef _WIN32
        DWORD timeout_ms = 5000;
        setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        /* Read stream header */
        printf("Waiting for stream header (5s timeout)...\n");
        iq_stream_header_t header;
        if (!tcp_recv_exact(g_iq_sock, &header, sizeof(header))) {
            fprintf(stderr, "Failed to read I/Q stream header\n");
            socket_close(g_iq_sock);
            tcp_cleanup();
            return 1;
        }

        if (header.magic != MAGIC_PHXI) {
            fprintf(stderr, "Invalid header magic: 0x%08X (expected PHXI)\n", header.magic);
            socket_close(g_iq_sock);
            tcp_cleanup();
            return 1;
        }

        g_tcp_sample_rate = header.sample_rate;
        g_tcp_sample_format = header.sample_format;
        g_tcp_center_freq = ((uint64_t)header.center_freq_hi << 32) | header.center_freq_lo;

        printf("Stream header: rate=%u Hz, format=%u, freq=%llu Hz\n",
               g_tcp_sample_rate, g_tcp_sample_format, (unsigned long long)g_tcp_center_freq);

        /* Switch to short timeout for data streaming (100ms) */
        /* This allows the main loop to stay responsive when no data is flowing */
#ifdef _WIN32
        timeout_ms = 100;
        setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        printf("Waiting for I/Q data (use control port %d to START streaming)...\n", g_iq_port - 1);

        /* Calculate decimation factor to get to display rate (~48kHz) */
        g_decimation_factor = g_tcp_sample_rate / SAMPLE_RATE;
        if (g_decimation_factor < 1) g_decimation_factor = 1;
        g_effective_sample_rate = g_tcp_sample_rate / g_decimation_factor;
        printf("Decimation: %d:1 -> %d Hz effective\n", g_decimation_factor, g_effective_sample_rate);

        /* Note: Streaming should be started externally via control port */
        /* Waterfall is a passive I/Q data consumer only */
        g_tcp_streaming = true;
    }

    if (g_stdin_mode) {
#ifdef _WIN32
        /* Set stdin to binary mode */
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        printf("Stdin mode: Waiting for PCM data...\n");
    }

    /* Initialize audio output */
    if (wf_audio_is_enabled()) {
        if (!wf_audio_init()) {
            fprintf(stderr, "Warning: Audio output unavailable\n");
        }
    }

    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        g_tcp_mode ? "Waterfall (TCP)" : "Waterfall",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_window_width, g_window_height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Texture *texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        g_window_width, g_window_height
    );
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Initialize KissFFT */
    kiss_fft_cfg fft_cfg = kiss_fft_alloc(FFT_SIZE, 0, NULL, NULL);
    if (!fft_cfg) {
        fprintf(stderr, "kiss_fft_alloc failed\n");
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Allocate buffers */
    int16_t *pcm_buffer = (int16_t *)malloc(FFT_SIZE * sizeof(int16_t));
    kiss_fft_cpx *fft_in = (kiss_fft_cpx *)malloc(FFT_SIZE * sizeof(kiss_fft_cpx));
    kiss_fft_cpx *fft_out = (kiss_fft_cpx *)malloc(FFT_SIZE * sizeof(kiss_fft_cpx));
    uint8_t *pixels = (uint8_t *)malloc(g_window_width * g_window_height * 3);
    float *fft_magnitudes = (float *)malloc(FFT_SIZE * sizeof(float));        /* Full FFT resolution */
    float *display_magnitudes = (float *)malloc(g_waterfall_width * sizeof(float)); /* Scaled for display */

    if (!pcm_buffer || !fft_in || !fft_out || !pixels || !fft_magnitudes || !display_magnitudes) {
        fprintf(stderr, "Memory allocation failed\n");
        return 1;
    }

    /* Clear pixel buffer */
    memset(pixels, 0, g_window_width * g_window_height * 3);

    /* Hanning window */
    float *window_func = (float *)malloc(FFT_SIZE * sizeof(float));
    for (int i = 0; i < FFT_SIZE; i++) {
        window_func[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (FFT_SIZE - 1)));
    }

    printf("Waterfall display ready.\n");
    if (g_tcp_mode) {
        printf("Mode: TCP I/Q (rate=%u Hz, decim=%d:1 -> %d Hz)\n",
               g_tcp_sample_rate, g_decimation_factor, g_effective_sample_rate);
    } else {
        printf("Mode: Stdin PCM (rate=%d Hz)\n", SAMPLE_RATE);
    }
    printf("Window: %dx%d, FFT: %d bins (%.1f Hz/bin)\n",
           g_window_width, g_window_height, FFT_SIZE / 2, (float)g_effective_sample_rate / FFT_SIZE);
    printf("Keys: 0=gain, 1-7=tick thresholds, +/- adjust, D=detect, S=stats, Q/Esc quit\n");
    printf("1:100Hz(±10) 2:440Hz(±5) 3:500Hz(±5) 4:600Hz(±5) 5:1000Hz(±100) 6:1200Hz(±100) 7:1500Hz(±20)\n");

    /* Initialize tick detector */
    tick_detector_init(&g_tick_detector);
    uint64_t frame_num = 0;
    printf("\nTick detection ENABLED - watching 1000 Hz bucket\n");
    printf("Logging to wwv_ticks.csv\n\n");

    bool running = true;

    while (running) {
        /* Handle SDL events */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    int new_width = event.window.data1;
                    int new_height = event.window.data2;

                    /* Update runtime dimensions */
                    g_window_width = new_width;
                    g_window_height = new_height;
                    g_waterfall_width = new_width - g_bucket_width;
                    if (g_waterfall_width < 100) g_waterfall_width = 100;  /* Minimum waterfall width */

                    /* Recreate texture */
                    SDL_DestroyTexture(texture);
                    texture = SDL_CreateTexture(
                        renderer,
                        SDL_PIXELFORMAT_RGB24,
                        SDL_TEXTUREACCESS_STREAMING,
                        g_window_width, g_window_height
                    );

                    /* Reallocate pixel buffer */
                    free(pixels);
                    pixels = (uint8_t *)malloc(g_window_width * g_window_height * 3);
                    memset(pixels, 0, g_window_width * g_window_height * 3);

                    /* Reallocate display magnitudes buffer */
                    free(display_magnitudes);
                    display_magnitudes = (float *)malloc(g_waterfall_width * sizeof(float));

                    printf("Window resized to %dx%d (waterfall: %d)\n",
                           g_window_width, g_window_height, g_waterfall_width);
                }
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    running = false;
                } else if (event.key.keysym.sym == SDLK_PLUS ||
                           event.key.keysym.sym == SDLK_EQUALS ||
                           event.key.keysym.sym == SDLK_KP_PLUS) {
                    if (g_selected_param == 0) {
                        g_gain_offset += 3.0f;
                        printf("Gain: %+.0f dB\n", g_gain_offset);
                    } else {
                        int idx = g_selected_param - 1;
                        g_tick_thresholds[idx] *= 1.5f;
                        printf("%s threshold: %.4f\n", TICK_NAMES[idx], g_tick_thresholds[idx]);
                    }
                } else if (event.key.keysym.sym == SDLK_MINUS ||
                           event.key.keysym.sym == SDLK_KP_MINUS) {
                    if (g_selected_param == 0) {
                        g_gain_offset -= 3.0f;
                        printf("Gain: %+.0f dB\n", g_gain_offset);
                    } else {
                        int idx = g_selected_param - 1;
                        g_tick_thresholds[idx] /= 1.5f;
                        if (g_tick_thresholds[idx] < 0.0001f) g_tick_thresholds[idx] = 0.0001f;
                        printf("%s threshold: %.4f\n", TICK_NAMES[idx], g_tick_thresholds[idx]);
                    }
                } else if (event.key.keysym.sym == SDLK_0 || event.key.keysym.sym == SDLK_KP_0) {
                    g_selected_param = 0;
                    printf("Selected: Gain (%+.0f dB)\n", g_gain_offset);
                } else if (event.key.keysym.sym == SDLK_1 || event.key.keysym.sym == SDLK_KP_1) {
                    g_selected_param = 1;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[0], g_tick_thresholds[0]);
                } else if (event.key.keysym.sym == SDLK_2 || event.key.keysym.sym == SDLK_KP_2) {
                    g_selected_param = 2;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[1], g_tick_thresholds[1]);
                } else if (event.key.keysym.sym == SDLK_3 || event.key.keysym.sym == SDLK_KP_3) {
                    g_selected_param = 3;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[2], g_tick_thresholds[2]);
                } else if (event.key.keysym.sym == SDLK_4 || event.key.keysym.sym == SDLK_KP_4) {
                    g_selected_param = 4;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[3], g_tick_thresholds[3]);
                } else if (event.key.keysym.sym == SDLK_5 || event.key.keysym.sym == SDLK_KP_5) {
                    g_selected_param = 5;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[4], g_tick_thresholds[4]);
                } else if (event.key.keysym.sym == SDLK_6 || event.key.keysym.sym == SDLK_KP_6) {
                    g_selected_param = 6;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[5], g_tick_thresholds[5]);
                } else if (event.key.keysym.sym == SDLK_7 || event.key.keysym.sym == SDLK_KP_7) {
                    g_selected_param = 7;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[6], g_tick_thresholds[6]);
                } else if (event.key.keysym.sym == SDLK_d) {
                    g_tick_detector.detection_enabled = !g_tick_detector.detection_enabled;
                    printf("Tick detection: %s\n", g_tick_detector.detection_enabled ? "ENABLED" : "DISABLED");
                } else if (event.key.keysym.sym == SDLK_s) {
                    tick_detector_print_stats(&g_tick_detector, frame_num);
                } else if (event.key.keysym.sym == SDLK_m) {
                    /* Mute/unmute audio */
                    wf_audio_toggle_mute();
                    printf("Audio: %s\n", wf_audio_is_enabled() ? "ON" : "MUTED");
                } else if (event.key.keysym.sym == SDLK_UP) {
                    /* Volume up */
                    wf_audio_volume_up();
                } else if (event.key.keysym.sym == SDLK_DOWN) {
                    /* Volume down */
                    wf_audio_volume_down();
                }
            }
        }

        /* Read samples - either from stdin (PCM) or TCP (I/Q) */
        bool got_samples = false;

        if (g_tcp_mode) {
            /* TCP mode: read I/Q data frames */
            int samples_needed = FFT_SIZE;
            int pcm_idx = 0;

            while (pcm_idx < samples_needed && running) {
                /* Check for metadata or data frame */
                uint32_t magic;
                recv_result_t result = tcp_recv_exact_ex(g_iq_sock, &magic, 4);
                if (result == RECV_TIMEOUT) {
                    /* No data yet - break out to update display, will retry next frame */
                    break;
                }
                if (result == RECV_ERROR) {
                    /* Connection lost - attempt to reconnect */
                    if (!tcp_reconnect()) {
                        running = false;  /* User quit during reconnect */
                    }
                    break;  /* Break out to main loop, will retry with new connection */
                }

                if (magic == MAGIC_META) {
                    /* Metadata update - read rest of struct */
                    iq_metadata_update_t meta;
                    meta.magic = magic;
                    if (tcp_recv_exact_ex(g_iq_sock, ((char*)&meta) + 4, sizeof(meta) - 4) != RECV_OK) {
                        /* Connection error reading metadata - reconnect */
                        if (!tcp_reconnect()) {
                            running = false;
                        }
                        break;
                    }
                    g_tcp_sample_rate = meta.sample_rate;
                    g_tcp_center_freq = ((uint64_t)meta.center_freq_hi << 32) | meta.center_freq_lo;
                    g_decimation_factor = g_tcp_sample_rate / SAMPLE_RATE;
                    if (g_decimation_factor < 1) g_decimation_factor = 1;
                    printf("Metadata update: rate=%u, freq=%llu\n",
                           g_tcp_sample_rate, (unsigned long long)g_tcp_center_freq);
                    continue;
                }

                if (magic != MAGIC_IQDQ) {
                    fprintf(stderr, "Unknown frame magic: 0x%08X\n", magic);
                    continue;
                }

                /* Read data frame header */
                iq_data_frame_t frame;
                frame.magic = magic;
                if (tcp_recv_exact_ex(g_iq_sock, ((char*)&frame) + 4, sizeof(frame) - 4) != RECV_OK) {
                    /* Connection error reading frame header - reconnect */
                    if (!tcp_reconnect()) {
                        running = false;
                    }
                    break;
                }

                /* Read I/Q samples based on format */
                int bytes_per_sample = (g_tcp_sample_format == IQ_FORMAT_S16) ? 4 :
                                       (g_tcp_sample_format == IQ_FORMAT_F32) ? 8 : 2;
                int data_bytes = frame.num_samples * bytes_per_sample;

                /* Allocate temp buffer for raw I/Q data */
                static uint8_t *iq_buffer = NULL;
                static int iq_buffer_size = 0;
                if (data_bytes > iq_buffer_size) {
                    iq_buffer = (uint8_t *)realloc(iq_buffer, data_bytes);
                    iq_buffer_size = data_bytes;
                }

                if (tcp_recv_exact_ex(g_iq_sock, iq_buffer, data_bytes) != RECV_OK) {
                    /* Connection error reading I/Q data - reconnect */
                    if (!tcp_reconnect()) {
                        running = false;
                    }
                    break;
                }

                /* Convert I/Q to magnitude and decimate - verbatim from simple_am_receiver.c */

                /* Initialize DSP filters on first sample (after we know sample rate) */
                if (!g_display_dsp.initialized) {
                    wf_dsp_path_init(&g_display_dsp, IQ_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                    printf("Display DSP initialized: lowpass @ %.0f Hz, sample rate = %u\n",
                           IQ_FILTER_CUTOFF, g_tcp_sample_rate);
                }
                if (!g_audio_dsp.initialized) {
                    wf_dsp_path_init(&g_audio_dsp, IQ_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                    printf("Audio DSP initialized: lowpass @ %.0f Hz, sample rate = %u\n",
                           IQ_FILTER_CUTOFF, g_tcp_sample_rate);
                }

                for (uint32_t s = 0; s < frame.num_samples && pcm_idx < samples_needed; s++) {
                    float i_raw, q_raw;

                    if (g_tcp_sample_format == IQ_FORMAT_S16) {
                        int16_t *samples = (int16_t *)iq_buffer;
                        /* NO normalization - keep raw int16 values as float */
                        i_raw = (float)samples[s * 2];
                        q_raw = (float)samples[s * 2 + 1];
                    } else if (g_tcp_sample_format == IQ_FORMAT_F32) {
                        float *samples = (float *)iq_buffer;
                        i_raw = samples[s * 2];
                        q_raw = samples[s * 2 + 1];
                    } else {
                        /* U8 format */
                        i_raw = (float)(iq_buffer[s * 2] - 128);
                        q_raw = (float)(iq_buffer[s * 2 + 1] - 128);
                    }

                    /* Simple decimation: keep every Nth sample */
                    g_decim_counter++;
                    if (g_decim_counter >= g_decimation_factor) {

                        /* ===== DISPLAY PATH (frozen logic - see P2) ===== */
                        float display_ac = wf_dsp_path_process(&g_display_dsp, i_raw, q_raw);
                        float display_sample = display_ac * 50.0f;
                        if (display_sample > 32767.0f) display_sample = 32767.0f;
                        if (display_sample < -32767.0f) display_sample = -32767.0f;
                        pcm_buffer[pcm_idx++] = (int16_t)display_sample;

                        /* ===== AUDIO PATH (can be modified - see P2) ===== */
                        float audio_ac = wf_dsp_path_process(&g_audio_dsp, i_raw, q_raw);
                        wf_audio_process_sample(audio_ac);

                        g_decim_counter = 0;
                    }
                }
            }
            got_samples = (pcm_idx >= samples_needed);
        } else {
            /* Stdin mode: Read PCM samples */
            size_t read_count = fread(pcm_buffer, sizeof(int16_t), FFT_SIZE, stdin);
            if (read_count < (size_t)FFT_SIZE) {
                if (feof(stdin)) {
                    printf("End of input\n");
                    /* Keep window open but stop reading */
                    SDL_Delay(100);
                    continue;
                }
                /* Pad with zeros if partial read */
                memset(pcm_buffer + read_count, 0, (FFT_SIZE - read_count) * sizeof(int16_t));
            }
            got_samples = true;
        }

        if (!got_samples) {
            SDL_Delay(10);
            continue;
        }

        /* Convert to complex and apply window */
        for (int i = 0; i < FFT_SIZE; i++) {
            fft_in[i].r = (pcm_buffer[i] / 32768.0f) * window_func[i];
            fft_in[i].i = 0.0f;
        }

        /* Run FFT */
        kiss_fft(fft_cfg, fft_in, fft_out);

        /* Calculate magnitudes with FFT shift (DC in center) - full FFT resolution */
        for (int i = 0; i < FFT_SIZE; i++) {
            int bin;
            if (i < FFT_SIZE / 2) {
                /* Left half: negative frequencies */
                bin = FFT_SIZE / 2 + i;
            } else {
                /* Right half: positive frequencies */
                bin = i - FFT_SIZE / 2;
            }
            /* Wrap around */
            if (bin < 0) bin += FFT_SIZE;
            if (bin >= FFT_SIZE) bin -= FFT_SIZE;

            /* Bounds check */
            if (bin >= 0 && bin < FFT_SIZE) {
                float re = fft_out[bin].r;
                float im = fft_out[bin].i;
                fft_magnitudes[i] = sqrtf(re * re + im * im) / FFT_SIZE;
            } else {
                fft_magnitudes[i] = 0.0f;
            }
        }

        /* Scale FFT magnitudes to display width */
        scale_fft_to_display(fft_magnitudes, display_magnitudes);

        /* Auto-gain: track peak and floor */
        float frame_max = -200.0f;
        float frame_min = 200.0f;
        for (int i = 0; i < g_waterfall_width; i++) {
            float db = 20.0f * log10f(display_magnitudes[i] + 1e-10f);
            if (db > frame_max) frame_max = db;
            if (db < frame_min) frame_min = db;
        }
        /* Update tracked values with attack/decay */
        if (frame_max > g_peak_db) {
            g_peak_db = g_peak_db + AGC_ATTACK * (frame_max - g_peak_db);
        } else {
            g_peak_db = g_peak_db + AGC_DECAY * (frame_max - g_peak_db);
        }
        if (frame_min < g_floor_db) {
            g_floor_db = g_floor_db + AGC_ATTACK * (frame_min - g_floor_db);
        } else {
            g_floor_db = g_floor_db + AGC_DECAY * (frame_min - g_floor_db);
        }

        /* Scroll pixels down by 1 row */
        memmove(pixels + g_window_width * 3,  /* dest: row 1 */
                pixels,                        /* src: row 0 */
                g_window_width * (g_window_height - 1) * 3);

        /* Draw new row at top (row 0) - WATERFALL ONLY */
        for (int x = 0; x < g_waterfall_width; x++) {
            uint8_t r, g, b;
            magnitude_to_rgb(display_magnitudes[x], g_peak_db, g_floor_db, &r, &g, &b);
            pixels[x * 3 + 0] = r;
            pixels[x * 3 + 1] = g;
            pixels[x * 3 + 2] = b;
        }

        /* Tick detection: check each frequency and mark with colored dot if above threshold */
        float hz_per_bin = (float)SAMPLE_RATE / FFT_SIZE;
        for (int f = 0; f < NUM_TICK_FREQS; f++) {
            int freq = TICK_FREQS[f];
            int bandwidth = TICK_BW[f];

            /* Calculate center bin and how many bins to sum based on bandwidth */
            int center_bin = (int)(freq / hz_per_bin + 0.5f);
            int bin_span = (int)(bandwidth / hz_per_bin + 0.5f);
            if (bin_span < 1) bin_span = 1;

            /* Sum energy across bandwidth for both sidebands */
            float pos_energy = 0.0f, neg_energy = 0.0f;
            for (int b = -bin_span; b <= bin_span; b++) {
                int pos_bin = center_bin + b;
                int neg_bin = FFT_SIZE - center_bin + b;

                if (pos_bin >= 0 && pos_bin < FFT_SIZE) {
                    float re = fft_out[pos_bin].r;
                    float im = fft_out[pos_bin].i;
                    pos_energy += sqrtf(re * re + im * im) / FFT_SIZE;
                }
                if (neg_bin >= 0 && neg_bin < FFT_SIZE) {
                    float re = fft_out[neg_bin].r;
                    float im = fft_out[neg_bin].i;
                    neg_energy += sqrtf(re * re + im * im) / FFT_SIZE;
                }
            }

            float combined_energy = pos_energy + neg_energy;
            g_bucket_energy[f] = combined_energy;  /* Store for right panel display */

            /* Feed 1000 Hz bucket (index 4) to tick detector */
            if (f == 4) {
                tick_detector_update(&g_tick_detector, combined_energy, frame_num);
            }

            /* If above threshold, draw marker dot at the frequency position */
            if (combined_energy > g_tick_thresholds[f]) {
                /* Calculate x position in FFT-shifted display, scaled to display width */
                float scale = (float)g_waterfall_width / (float)FFT_SIZE;
                int x_pos = (int)((FFT_SIZE / 2 + center_bin) * scale);
                int x_neg = (int)((FFT_SIZE / 2 - center_bin) * scale);

                /* Draw red dot at positive frequency */
                if (x_pos >= 0 && x_pos < g_waterfall_width) {
                    pixels[x_pos * 3 + 0] = 255;  /* R */
                    pixels[x_pos * 3 + 1] = 0;    /* G */
                    pixels[x_pos * 3 + 2] = 0;    /* B */
                }
                /* Draw red dot at negative frequency */
                if (x_neg >= 0 && x_neg < g_waterfall_width) {
                    pixels[x_neg * 3 + 0] = 255;  /* R */
                    pixels[x_neg * 3 + 1] = 0;    /* G */
                    pixels[x_neg * 3 + 2] = 0;    /* B */
                }
            }
        }

        /* Draw selection indicator at bottom of first row */
        /* Small colored tick at the selected parameter position */
        {
            int indicator_x = 10 + g_selected_param * 20;
            if (indicator_x < g_waterfall_width) {
                pixels[indicator_x * 3 + 0] = 255;  /* Cyan indicator */
                pixels[indicator_x * 3 + 1] = 255;
                pixels[indicator_x * 3 + 2] = 0;
            }
        }

        /* === RIGHT PANEL: Bucket energy bars === */
        {
            int bar_width = g_bucket_width / NUM_TICK_FREQS;  /* ~28 pixels per bar */
            int bar_gap = 2;  /* Gap between bars */

            /* Clear right panel (black background) */
            for (int y = 0; y < g_window_height; y++) {
                for (int x = g_waterfall_width; x < g_window_width; x++) {
                    int idx = (y * g_window_width + x) * 3;
                    pixels[idx + 0] = 0;
                    pixels[idx + 1] = 0;
                    pixels[idx + 2] = 0;
                }
            }

            /* Draw each bucket bar */
            for (int f = 0; f < NUM_TICK_FREQS; f++) {
                int bar_x = g_waterfall_width + f * bar_width + bar_gap;
                int bar_w = bar_width - bar_gap * 2;

                /* Convert energy to height using log scale */
                float db = 20.0f * log10f(g_bucket_energy[f] + 1e-10f);
                float norm = (db - g_floor_db) / (g_peak_db - g_floor_db + 0.1f);
                if (norm < 0.0f) norm = 0.0f;
                if (norm > 1.0f) norm = 1.0f;

                int bar_height = (int)(norm * g_window_height);

                /* Get color based on magnitude */
                uint8_t r, g, b;

                /* Check if this is the 1000Hz bar (index 4) and tick was just detected */
                if (f == 4 && g_tick_detector.flash_frames_remaining > 0) {
                    /* Purple flash for tick detection - full height bar */
                    r = 180;
                    g = 0;
                    b = 255;
                    bar_height = g_window_height;  /* Full height when detected */
                } else {
                    magnitude_to_rgb(g_bucket_energy[f], g_peak_db, g_floor_db, &r, &g, &b);
                }

                /* Draw bar from bottom up */
                for (int y = g_window_height - bar_height; y < g_window_height; y++) {
                    for (int x = bar_x; x < bar_x + bar_w && x < g_window_width; x++) {
                        int idx = (y * g_window_width + x) * 3;
                        pixels[idx + 0] = r;
                        pixels[idx + 1] = g;
                        pixels[idx + 2] = b;
                    }
                }
            }
        }

        /* Decrement flash counter */
        if (g_tick_detector.flash_frames_remaining > 0) {
            g_tick_detector.flash_frames_remaining--;
        }

        /* Update texture */
        SDL_UpdateTexture(texture, NULL, pixels, g_window_width * 3);

        /* Render */
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);

        SDL_RenderPresent(renderer);

        frame_num++;
    }

    /* Print final tick stats and cleanup */
    printf("\n");
    tick_detector_print_stats(&g_tick_detector, frame_num);
    tick_detector_close(&g_tick_detector);

    /* Audio cleanup */
    wf_audio_close();

    /* TCP cleanup */
    if (g_tcp_mode) {
        if (g_iq_sock != SOCKET_INVALID) {
            socket_close(g_iq_sock);
        }
        tcp_cleanup();
    }

    /* Cleanup */
    free(window_func);
    free(display_magnitudes);
    free(fft_magnitudes);
    free(pixels);
    free(fft_out);
    free(fft_in);
    free(pcm_buffer);
    kiss_fft_free(fft_cfg);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("Done.\n");
    return 0;
}
