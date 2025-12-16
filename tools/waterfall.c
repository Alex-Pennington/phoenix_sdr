/*******************************************************************************
 * FROZEN FILE - DO NOT MODIFY
 * See .github/copilot-instructions.md P1 section
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
#include <time.h>

#include <SDL.h>
#include "kiss_fft.h"
#include "version.h"
#include "tick_detector.h"
#include "marker_detector.h"
#include "sync_detector.h"
#include "waterfall_flash.h"

/*============================================================================
 * WWV Subcarrier Tone Schedule (minutes past the hour)
 *============================================================================*/
static const int WWV_500HZ_MINUTES[] = {4,6,12,14,16,20,22,24,26,28,32,34,36,38,40,42,52,54,56,58,-1};
static const int WWV_600HZ_MINUTES[] = {1,3,5,7,11,13,15,17,19,21,23,25,27,31,33,35,37,39,41,53,55,57,-1};

static const char *wwv_expected_tone(int minute) {
    for (int i = 0; WWV_500HZ_MINUTES[i] >= 0; i++)
        if (WWV_500HZ_MINUTES[i] == minute) return "500Hz";
    for (int i = 0; WWV_600HZ_MINUTES[i] >= 0; i++)
        if (WWV_600HZ_MINUTES[i] == minute) return "600Hz";
    return "NONE";
}

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
    RECV_OK = 0,
    RECV_TIMEOUT,
    RECV_ERROR
} recv_result_t;

/*============================================================================
 * TCP Configuration and Protocol
 *============================================================================*/

#define DEFAULT_IQ_PORT         4536

#define MAGIC_PHXI  0x50485849
#define MAGIC_IQDQ  0x49514451
#define MAGIC_META  0x4D455441

#define IQ_FORMAT_S16   1
#define IQ_FORMAT_F32   2
#define IQ_FORMAT_U8    3

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sample_rate;
    uint32_t sample_format;
    uint32_t center_freq_lo;
    uint32_t center_freq_hi;
    uint32_t reserved[2];
} iq_stream_header_t;

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint32_t num_samples;
    uint32_t flags;
} iq_data_frame_t;

typedef struct {
    uint32_t magic;
    uint32_t sample_rate;
    uint32_t sample_format;
    uint32_t center_freq_lo;
    uint32_t center_freq_hi;
    uint32_t gain_reduction;
    uint32_t lna_state;
    uint32_t reserved;
} iq_metadata_update_t;
#pragma pack(pop)

/* TCP state */
static bool g_tcp_mode = true;
static bool g_stdin_mode = false;
static char g_tcp_host[256] = "localhost";
static int g_iq_port = DEFAULT_IQ_PORT;
static socket_t g_iq_sock = SOCKET_INVALID;
static uint32_t g_tcp_sample_rate = 2000000;
static uint32_t g_tcp_sample_format = IQ_FORMAT_S16;
static uint64_t g_tcp_center_freq = 15000000;
static uint32_t g_tcp_gain_reduction = 0;
static uint32_t g_tcp_lna_state = 0;
static bool g_tcp_streaming = false;

/* Decimation factors (computed from TCP sample rate) */
static int g_detector_decimation = 1;   /* 2 MHz → 48 kHz */
static int g_display_decimation = 1;    /* 2 MHz → 12 kHz */

/* Legacy compatibility */
static int g_decimation_factor = 1;
static int g_decim_counter = 0;

/*============================================================================
 * Lowpass Filter
 *============================================================================*/

typedef struct {
    float x1, x2;
    float y1, y2;
    float b0, b1, b2, a1, a2;
} lowpass_t;

static void lowpass_init(lowpass_t *lp, float cutoff_hz, float sample_rate) {
    float w0 = 2.0f * 3.14159265f * cutoff_hz / sample_rate;
    float alpha = sinf(w0) / (2.0f * 0.7071f);
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
 * Window Functions
 *============================================================================*/

/**
 * Generate Blackman-Harris window coefficients
 * 4-term Blackman-Harris has excellent sidelobe suppression (-92 dB)
 * NENBW = 2.0 (noise equivalent bandwidth)
 */
static void generate_blackman_harris(float *window, int size) {
    const float a0 = 0.35875f;
    const float a1 = 0.48829f;
    const float a2 = 0.14128f;
    const float a3 = 0.01168f;
    const float pi = 3.14159265358979323846f;

    for (int i = 0; i < size; i++) {
        float n = (float)i / (float)(size - 1);
        window[i] = a0
                  - a1 * cosf(2.0f * pi * n)
                  + a2 * cosf(4.0f * pi * n)
                  - a3 * cosf(6.0f * pi * n);
    }
}

/**
 * Generate Hann window coefficients (for detector path, unchanged)
 */
static void generate_hann(float *window, int size) {
    const float pi = 3.14159265358979323846f;
    for (int i = 0; i < size; i++) {
        window[i] = 0.5f * (1.0f - cosf(2.0f * pi * i / (size - 1)));
    }
}

/* DSP filter instances - DETECTOR PATH */
static lowpass_t g_detector_lowpass_i;
static lowpass_t g_detector_lowpass_q;
static bool g_detector_dsp_initialized = false;
static int g_detector_decim_counter = 0;

/* DSP filter instances - DISPLAY PATH */
static lowpass_t g_display_lowpass_i;
static lowpass_t g_display_lowpass_q;
static bool g_display_dsp_initialized = false;
static int g_display_decim_counter = 0;

/* Buffer for decimated I/Q samples */
typedef struct {
    float i;
    float q;
} iq_sample_t;

/* Detector path buffer (48 kHz, feeds tick detector) */
static iq_sample_t *g_detector_buffer = NULL;
static int g_detector_buffer_idx = 0;

/* Display path buffer (12 kHz, 2048-pt FFT with 50% overlap) */
static iq_sample_t *g_display_buffer = NULL;
static int g_display_buffer_idx = 0;
static int g_display_new_samples = 0;  /* Count new samples since last FFT */

/* Legacy compatibility */
static iq_sample_t *g_iq_buffer = NULL;
static int g_iq_buffer_idx = 0;

/*============================================================================
 * Configuration - DUAL PATH ARCHITECTURE
 *============================================================================*/

/* Window dimensions */
#define WATERFALL_WIDTH 1024
#define BUCKET_WIDTH    200
#define WINDOW_WIDTH    (WATERFALL_WIDTH + BUCKET_WIDTH)
#define WINDOW_HEIGHT   600

/*----------------------------------------------------------------------------
 * DETECTOR PATH (unchanged from original)
 * Purpose: Feed tick detector with samples optimized for pulse detection
 *----------------------------------------------------------------------------*/
#define DETECTOR_SAMPLE_RATE    48000       /* 48 kHz for detector */
#define DETECTOR_FILTER_CUTOFF  5000.0f     /* 5 kHz lowpass */

/*----------------------------------------------------------------------------
 * DISPLAY PATH (new high-resolution waterfall)
 * Purpose: Visual feedback matching SDRuno quality
 *
 * Design rationale (from SDRuno analysis):
 *   - 12 kHz sample rate gives ±6 kHz bandwidth (WWV is within ±2 kHz)
 *   - 2048-pt FFT at 12 kHz = 5.86 Hz/bin resolution
 *   - Blackman-Harris window for clean spectral lines (NENBW ≈ 1.9)
 *   - 50% overlap recovers time resolution: 170ms frame → 85ms effective
 *   - Effective RBW ≈ 11 Hz (can resolve 10 Hz BCD sidebands)
 *----------------------------------------------------------------------------*/
#define DISPLAY_SAMPLE_RATE     12000       /* 12 kHz for display */
#define DISPLAY_FILTER_CUTOFF   6000.0f     /* 6 kHz lowpass (Nyquist guard) */
#define DISPLAY_FFT_SIZE        2048        /* 2048-pt FFT */
#define DISPLAY_OVERLAP         1024        /* 50% overlap (half of FFT_SIZE) */
#define DISPLAY_HZ_PER_BIN      ((float)DISPLAY_SAMPLE_RATE / DISPLAY_FFT_SIZE)  /* 5.86 Hz */
#define DISPLAY_FRAME_MS        ((float)DISPLAY_FFT_SIZE * 1000.0f / DISPLAY_SAMPLE_RATE)  /* 170.7 ms */
#define DISPLAY_EFFECTIVE_MS    ((float)DISPLAY_OVERLAP * 1000.0f / DISPLAY_SAMPLE_RATE)  /* 85.3 ms */

/* Legacy defines for compatibility */
#define FFT_SIZE        DISPLAY_FFT_SIZE
#define SAMPLE_RATE     DETECTOR_SAMPLE_RATE  /* Detector path rate */
#define ZOOM_MAX_HZ     5000.0f               /* Display ±5000 Hz around DC */

/* Frequency buckets for WWV detection */
#define NUM_TICK_FREQS  7
static const int   TICK_FREQS[NUM_TICK_FREQS] = { 100,   440,  500,  600,  1000, 1200, 1500 };
static const int   TICK_BW[NUM_TICK_FREQS]    = { 10,    5,    5,    5,    100,  100,  20   };
static const char *TICK_NAMES[NUM_TICK_FREQS] = { "100Hz", "440Hz", "500Hz", "600Hz", "1000Hz", "1200Hz", "1500Hz" };

/*============================================================================
 * Display State
 *============================================================================*/

static float g_peak_db = -40.0f;
static float g_floor_db = -80.0f;
static float g_gain_offset = 0.0f;
#define AGC_ATTACK  0.05f
#define AGC_DECAY   0.002f

/* Bucket energy and thresholds - initialized to reasonable values */
static float g_bucket_energy[NUM_TICK_FREQS];
static int g_selected_param = 0;

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
            break;
        }

        socket_close(sock);
        sock = SOCKET_INVALID;
    }

    freeaddrinfo(result);
    return sock;
}

static recv_result_t tcp_recv_exact_ex(socket_t sock, void *buf, int n) {
    char *ptr = (char *)buf;
    int remaining = n;

    while (remaining > 0) {
        int received = recv(sock, ptr, remaining, 0);
        if (received > 0) {
            ptr += received;
            remaining -= received;
        } else if (received == 0) {
            return RECV_ERROR;
        } else {
            int err = socket_errno;
            if (err == EWOULDBLOCK_VAL || err == ETIMEDOUT_VAL) {
                return RECV_TIMEOUT;
            }
            return RECV_ERROR;
        }
    }
    return RECV_OK;
}

static bool tcp_recv_exact(socket_t sock, void *buf, int n) {
    return tcp_recv_exact_ex(sock, buf, n) == RECV_OK;
}

static bool parse_tcp_arg(const char *arg) {
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

static bool tcp_reconnect(void) {
    if (g_iq_sock != SOCKET_INVALID) {
        socket_close(g_iq_sock);
        g_iq_sock = SOCKET_INVALID;
    }

    g_detector_dsp_initialized = false;
    g_display_dsp_initialized = false;
    g_detector_decim_counter = 0;
    g_display_decim_counter = 0;

    printf("\n*** CONNECTION LOST - Reconnecting to %s:%d ***\n", g_tcp_host, g_iq_port);

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
            Sleep(1000);

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

#ifdef _WIN32
    DWORD timeout_ms = 5000;
    setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    iq_stream_header_t header;
    if (!tcp_recv_exact(g_iq_sock, &header, sizeof(header))) {
        fprintf(stderr, "Failed to read I/Q stream header after reconnect\n");
        socket_close(g_iq_sock);
        g_iq_sock = SOCKET_INVALID;
        return tcp_reconnect();
    }

    if (header.magic != MAGIC_PHXI) {
        fprintf(stderr, "Invalid header magic after reconnect: 0x%08X\n", header.magic);
        socket_close(g_iq_sock);
        g_iq_sock = SOCKET_INVALID;
        return tcp_reconnect();
    }

    g_tcp_sample_rate = header.sample_rate;
    g_tcp_sample_format = header.sample_format;
    g_tcp_center_freq = ((uint64_t)header.center_freq_hi << 32) | header.center_freq_lo;

    printf("Stream header: rate=%u Hz, format=%u, freq=%llu Hz\n",
           g_tcp_sample_rate, g_tcp_sample_format, (unsigned long long)g_tcp_center_freq);

    /* Recalculate decimation factors */
    g_detector_decimation = g_tcp_sample_rate / DETECTOR_SAMPLE_RATE;
    if (g_detector_decimation < 1) g_detector_decimation = 1;
    g_display_decimation = g_tcp_sample_rate / DISPLAY_SAMPLE_RATE;
    if (g_display_decimation < 1) g_display_decimation = 1;
    g_decimation_factor = g_detector_decimation;

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
}

/*============================================================================
 * Tick Detector Module
 *============================================================================*/

static tick_detector_t *g_tick_detector = NULL;
static marker_detector_t *g_marker_detector = NULL;
static sync_detector_t *g_sync_detector = NULL;
static FILE *g_channel_csv = NULL;
static uint64_t g_channel_log_interval = 0;  /* Log every N frames */
static FILE *g_subcarrier_csv = NULL;

/*============================================================================
 * Sync Detector Callback Wrappers
 *============================================================================*/

static void on_tick_marker(const tick_marker_event_t *event, void *user_data) {
    (void)user_data;
    if (g_sync_detector) {
        sync_detector_tick_marker(g_sync_detector, event->timestamp_ms,
                                   event->duration_ms, event->corr_ratio);
    }
}

static void on_marker_event(const marker_event_t *event, void *user_data) {
    (void)user_data;
    if (g_sync_detector) {
        sync_detector_marker_event(g_sync_detector, event->timestamp_ms,
                                    event->accumulated_energy, event->duration_ms);
    }
}

/*============================================================================
 * Color Mapping
 *============================================================================*/

static void magnitude_to_rgb(float mag, float peak_db, float floor_db, uint8_t *r, uint8_t *g, uint8_t *b) {
    float db = 20.0f * log10f(mag + 1e-10f);
    db += g_gain_offset;

    float range = peak_db - floor_db;
    if (range < 20.0f) range = 20.0f;

    float norm = (db - floor_db) / range;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;

    if (norm < 0.25f) {
        *r = 0;
        *g = 0;
        *b = (uint8_t)(norm * 4.0f * 255.0f);
    } else if (norm < 0.5f) {
        *r = 0;
        *g = (uint8_t)((norm - 0.25f) * 4.0f * 255.0f);
        *b = 255;
    } else if (norm < 0.75f) {
        *r = (uint8_t)((norm - 0.5f) * 4.0f * 255.0f);
        *g = 255;
        *b = (uint8_t)((0.75f - norm) * 4.0f * 255.0f);
    } else {
        *r = 255;
        *g = (uint8_t)((1.0f - norm) * 4.0f * 255.0f);
        *b = 0;
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
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

    if (g_tcp_mode) {
        printf("TCP Mode: Connecting to %s:%d\n", g_tcp_host, g_iq_port);

        if (!tcp_init()) {
            fprintf(stderr, "Failed to initialize networking\n");
            return 1;
        }

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
                Sleep(1000);
            }
        }
        printf("\n*** CONNECTED to %s:%d ***\n\n", g_tcp_host, g_iq_port);

#ifdef _WIN32
        DWORD timeout_ms = 5000;
        setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        printf("Waiting for stream header...\n");
        iq_stream_header_t header;
        if (!tcp_recv_exact(g_iq_sock, &header, sizeof(header))) {
            fprintf(stderr, "Failed to read I/Q stream header\n");
            socket_close(g_iq_sock);
            tcp_cleanup();
            return 1;
        }

        if (header.magic != MAGIC_PHXI) {
            fprintf(stderr, "Invalid header magic: 0x%08X\n", header.magic);
            socket_close(g_iq_sock);
            tcp_cleanup();
            return 1;
        }

        g_tcp_sample_rate = header.sample_rate;
        g_tcp_sample_format = header.sample_format;
        g_tcp_center_freq = ((uint64_t)header.center_freq_hi << 32) | header.center_freq_lo;

        printf("Stream: rate=%u Hz, format=%u, freq=%llu Hz\n",
               g_tcp_sample_rate, g_tcp_sample_format, (unsigned long long)g_tcp_center_freq);

#ifdef _WIN32
        timeout_ms = 100;
        setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        setsockopt(g_iq_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        /* Calculate decimation factors for both paths */
        g_detector_decimation = g_tcp_sample_rate / DETECTOR_SAMPLE_RATE;
        if (g_detector_decimation < 1) g_detector_decimation = 1;

        g_display_decimation = g_tcp_sample_rate / DISPLAY_SAMPLE_RATE;
        if (g_display_decimation < 1) g_display_decimation = 1;

        /* Legacy compatibility */
        g_decimation_factor = g_detector_decimation;
        g_effective_sample_rate = g_tcp_sample_rate / g_detector_decimation;

        printf("Detector path: %d:1 -> %d Hz\n", g_detector_decimation, DETECTOR_SAMPLE_RATE);
        printf("Display path:  %d:1 -> %d Hz (%.1f Hz/bin, %.1f ms effective)\n",
               g_display_decimation, DISPLAY_SAMPLE_RATE, DISPLAY_HZ_PER_BIN, DISPLAY_EFFECTIVE_MS);

        g_tcp_streaming = true;
    }

    if (g_stdin_mode) {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        printf("Stdin mode: Waiting for PCM data...\n");
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        g_tcp_mode ? "Waterfall (TCP)" : "Waterfall",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_SHOWN
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
        WINDOW_WIDTH, WINDOW_HEIGHT
    );
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    kiss_fft_cfg fft_cfg = kiss_fft_alloc(DISPLAY_FFT_SIZE, 0, NULL, NULL);
    if (!fft_cfg) {
        fprintf(stderr, "kiss_fft_alloc failed\n");
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Allocate buffers */
    kiss_fft_cpx *fft_in = (kiss_fft_cpx *)malloc(DISPLAY_FFT_SIZE * sizeof(kiss_fft_cpx));
    kiss_fft_cpx *fft_out = (kiss_fft_cpx *)malloc(DISPLAY_FFT_SIZE * sizeof(kiss_fft_cpx));
    uint8_t *pixels = (uint8_t *)malloc(WINDOW_WIDTH * WINDOW_HEIGHT * 3);
    float *magnitudes = (float *)malloc(WATERFALL_WIDTH * sizeof(float));

    /* Display path buffer (12 kHz, 2048 samples for FFT) */
    g_display_buffer = (iq_sample_t *)malloc(DISPLAY_FFT_SIZE * sizeof(iq_sample_t));

    /* Legacy buffer (not used but kept for compatibility) */
    g_iq_buffer = (iq_sample_t *)malloc(DISPLAY_FFT_SIZE * sizeof(iq_sample_t));

    if (!fft_in || !fft_out || !pixels || !magnitudes || !g_display_buffer || !g_iq_buffer) {
        fprintf(stderr, "Memory allocation failed\n");
        return 1;
    }

    memset(g_display_buffer, 0, DISPLAY_FFT_SIZE * sizeof(iq_sample_t));
    g_display_buffer_idx = 0;
    g_display_new_samples = 0;
    memset(g_iq_buffer, 0, DISPLAY_FFT_SIZE * sizeof(iq_sample_t));
    g_iq_buffer_idx = 0;
    memset(pixels, 0, WINDOW_WIDTH * WINDOW_HEIGHT * 3);

    /* Blackman-Harris window for display FFT (better sidelobe suppression) */
    float *window_func = (float *)malloc(DISPLAY_FFT_SIZE * sizeof(float));
    generate_blackman_harris(window_func, DISPLAY_FFT_SIZE);

    printf("\nWaterfall ready. Window: %dx%d\n", WINDOW_WIDTH, WINDOW_HEIGHT);
    printf("Display: %d-pt FFT, Blackman-Harris, 50%% overlap\n", DISPLAY_FFT_SIZE);
    printf("Resolution: %.1f Hz/bin, %.1f ms effective update\n", DISPLAY_HZ_PER_BIN, DISPLAY_EFFECTIVE_MS);
    printf("Keys: +/- gain, D=detect toggle, S=stats, Q/Esc quit\n\n");

    g_tick_detector = tick_detector_create("wwv_ticks.csv");
    if (!g_tick_detector) {
        fprintf(stderr, "Failed to create tick detector\n");
        return 1;
    }

    g_marker_detector = marker_detector_create("wwv_markers.csv");
    if (!g_marker_detector) {
        fprintf(stderr, "Failed to create marker detector\n");
        return 1;
    }

    /* Create sync detector and wire up callbacks */
    g_sync_detector = sync_detector_create("wwv_sync.csv");
    if (!g_sync_detector) {
        fprintf(stderr, "Failed to create sync detector\n");
        return 1;
    }
    tick_detector_set_marker_callback(g_tick_detector, on_tick_marker, NULL);
    marker_detector_set_callback(g_marker_detector, on_marker_event, NULL);

    g_channel_csv = fopen("wwv_channel.csv", "w");
    if (g_channel_csv) {
        fprintf(g_channel_csv, "# Phoenix SDR WWV Channel Log v%s\n", PHOENIX_VERSION_FULL);
        fprintf(g_channel_csv, "time,timestamp_ms,carrier_db,snr_db,sub500_db,sub600_db,tone1000_db,noise_db,quality\n");
    }

    g_subcarrier_csv = fopen("wwv_subcarrier.csv", "w");
    if (g_subcarrier_csv) {
        time_t now = time(NULL);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(g_subcarrier_csv, "# Phoenix SDR WWV Subcarrier Log v%s\n", PHOENIX_VERSION_FULL);
        fprintf(g_subcarrier_csv, "# Started: %s\n", time_str);
        fprintf(g_subcarrier_csv, "time,timestamp_ms,minute,expected,sub500_db,sub600_db,delta_db,detected,match\n");
        fflush(g_subcarrier_csv);
    }

    /* Initialize flash system and register detectors */
    flash_init();
    flash_register(&(flash_source_t){
        .name = "tick",
        .get_flash_frames = (int (*)(void*))tick_detector_get_flash_frames,
        .decrement_flash = (void (*)(void*))tick_detector_decrement_flash,
        .ctx = g_tick_detector,
        .freq_hz = 1000,
        .band_half_width = 3,
        .band_r = 180, .band_g = 0, .band_b = 255,
        .bar_index = 4,
        .bar_r = 180, .bar_g = 0, .bar_b = 255
    });
    flash_register(&(flash_source_t){
        .name = "marker",
        .get_flash_frames = (int (*)(void*))marker_detector_get_flash_frames,
        .decrement_flash = (void (*)(void*))marker_detector_decrement_flash,
        .ctx = g_marker_detector,
        .freq_hz = 1000,
        .band_half_width = 8,
        .band_r = 255, .band_g = 50, .band_b = 50,
        .bar_index = 4,
        .bar_r = 255, .bar_g = 50, .bar_b = 50
    });

    uint64_t frame_num = 0;

    bool running = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                    running = false;
                } else if (event.key.keysym.sym == SDLK_PLUS || event.key.keysym.sym == SDLK_EQUALS || event.key.keysym.sym == SDLK_KP_PLUS) {
                    g_gain_offset += 3.0f;
                    printf("Gain: %+.0f dB\n", g_gain_offset);
                    tick_detector_log_display_gain(g_tick_detector, g_gain_offset);
                    marker_detector_log_display_gain(g_marker_detector, g_gain_offset);
                } else if (event.key.keysym.sym == SDLK_MINUS || event.key.keysym.sym == SDLK_KP_MINUS) {
                    g_gain_offset -= 3.0f;
                    printf("Gain: %+.0f dB\n", g_gain_offset);
                    tick_detector_log_display_gain(g_tick_detector, g_gain_offset);
                    marker_detector_log_display_gain(g_marker_detector, g_gain_offset);
                } else if (event.key.keysym.sym == SDLK_d) {
                    bool enabled = !tick_detector_get_enabled(g_tick_detector);
                    tick_detector_set_enabled(g_tick_detector, enabled);
                    printf("Tick detection: %s\n", enabled ? "ENABLED" : "DISABLED");
                } else if (event.key.keysym.sym == SDLK_s) {
                    tick_detector_print_stats(g_tick_detector);
                }
            }
        }

        bool got_samples = false;
        int samples_collected = 0;

        if (g_tcp_mode) {
            while (samples_collected < DISPLAY_OVERLAP && running) {
                uint32_t magic;
                recv_result_t result = tcp_recv_exact_ex(g_iq_sock, &magic, 4);
                if (result == RECV_TIMEOUT) {
                    break;
                }
                if (result == RECV_ERROR) {
                    if (!tcp_reconnect()) {
                        running = false;
                    }
                    break;
                }

                if (magic == MAGIC_META) {
                    iq_metadata_update_t meta;
                    meta.magic = magic;
                    if (tcp_recv_exact_ex(g_iq_sock, ((char*)&meta) + 4, sizeof(meta) - 4) != RECV_OK) {
                        if (!tcp_reconnect()) {
                            running = false;
                        }
                        break;
                    }
                    g_tcp_sample_rate = meta.sample_rate;
                    g_tcp_center_freq = ((uint64_t)meta.center_freq_hi << 32) | meta.center_freq_lo;
                    g_tcp_gain_reduction = meta.gain_reduction;
                    g_tcp_lna_state = meta.lna_state;

                    /* Recalculate decimation factors */
                    g_detector_decimation = g_tcp_sample_rate / DETECTOR_SAMPLE_RATE;
                    if (g_detector_decimation < 1) g_detector_decimation = 1;
                    g_display_decimation = g_tcp_sample_rate / DISPLAY_SAMPLE_RATE;
                    if (g_display_decimation < 1) g_display_decimation = 1;
                    g_decimation_factor = g_detector_decimation;
                    printf("Metadata update: rate=%u, freq=%llu, GR=%u, LNA=%u\n",
                           g_tcp_sample_rate, (unsigned long long)g_tcp_center_freq,
                           g_tcp_gain_reduction, g_tcp_lna_state);

                    /* Log metadata change to tick CSV */
                    if (g_tick_detector) {
                        tick_detector_log_metadata(g_tick_detector,
                            g_tcp_center_freq, g_tcp_sample_rate,
                            g_tcp_gain_reduction, g_tcp_lna_state);
                    }
                    /* Log metadata change to marker CSV */
                    if (g_marker_detector) {
                        marker_detector_log_metadata(g_marker_detector,
                            g_tcp_center_freq, g_tcp_sample_rate,
                            g_tcp_gain_reduction, g_tcp_lna_state);
                    }
                    continue;
                }

                if (magic != MAGIC_IQDQ) {
                    fprintf(stderr, "Unknown frame magic: 0x%08X\n", magic);
                    continue;
                }

                iq_data_frame_t frame;
                frame.magic = magic;
                if (tcp_recv_exact_ex(g_iq_sock, ((char*)&frame) + 4, sizeof(frame) - 4) != RECV_OK) {
                    if (!tcp_reconnect()) {
                        running = false;
                    }
                    break;
                }

                int bytes_per_sample = (g_tcp_sample_format == IQ_FORMAT_S16) ? 4 :
                                       (g_tcp_sample_format == IQ_FORMAT_F32) ? 8 : 2;
                int data_bytes = frame.num_samples * bytes_per_sample;

                static uint8_t *iq_buffer = NULL;
                static int iq_buffer_size = 0;
                if (data_bytes > iq_buffer_size) {
                    iq_buffer = (uint8_t *)realloc(iq_buffer, data_bytes);
                    iq_buffer_size = data_bytes;
                }

                if (tcp_recv_exact_ex(g_iq_sock, iq_buffer, data_bytes) != RECV_OK) {
                    if (!tcp_reconnect()) {
                        running = false;
                    }
                    break;
                }

                /* Initialize DSP paths on first data */
                if (!g_detector_dsp_initialized) {
                    lowpass_init(&g_detector_lowpass_i, DETECTOR_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                    lowpass_init(&g_detector_lowpass_q, DETECTOR_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                    g_detector_dsp_initialized = true;
                    printf("Detector DSP: lowpass @ %.0f Hz\n", DETECTOR_FILTER_CUTOFF);
                }
                if (!g_display_dsp_initialized) {
                    lowpass_init(&g_display_lowpass_i, DISPLAY_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                    lowpass_init(&g_display_lowpass_q, DISPLAY_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                    g_display_dsp_initialized = true;
                    printf("Display DSP: lowpass @ %.0f Hz\n", DISPLAY_FILTER_CUTOFF);
                }

                for (uint32_t s = 0; s < frame.num_samples; s++) {
                    float i_raw, q_raw;

                    if (g_tcp_sample_format == IQ_FORMAT_S16) {
                        int16_t *samples = (int16_t *)iq_buffer;
                        i_raw = (float)samples[s * 2];
                        q_raw = (float)samples[s * 2 + 1];
                    } else if (g_tcp_sample_format == IQ_FORMAT_F32) {
                        float *samples = (float *)iq_buffer;
                        i_raw = samples[s * 2];
                        q_raw = samples[s * 2 + 1];
                    } else {
                        i_raw = (float)(iq_buffer[s * 2] - 128);
                        q_raw = (float)(iq_buffer[s * 2 + 1] - 128);
                    }

                    /*========================================================
                     * DETECTOR PATH (48 kHz)
                     * Feeds tick detector with samples optimized for pulses
                     *========================================================*/
                    float det_i = lowpass_process(&g_detector_lowpass_i, i_raw);
                    float det_q = lowpass_process(&g_detector_lowpass_q, q_raw);

                    g_detector_decim_counter++;
                    if (g_detector_decim_counter >= g_detector_decimation) {
                        g_detector_decim_counter = 0;
                        tick_detector_process_sample(g_tick_detector, det_i, det_q);
                        marker_detector_process_sample(g_marker_detector, det_i, det_q);
                    }

                    /*========================================================
                     * DISPLAY PATH (12 kHz)
                     * High-resolution waterfall with Blackman-Harris window
                     *========================================================*/
                    float disp_i = lowpass_process(&g_display_lowpass_i, i_raw);
                    float disp_q = lowpass_process(&g_display_lowpass_q, q_raw);

                    g_display_decim_counter++;
                    if (g_display_decim_counter >= g_display_decimation) {
                        g_display_decim_counter = 0;

                        /* Store in circular buffer */
                        g_display_buffer[g_display_buffer_idx].i = disp_i;
                        g_display_buffer[g_display_buffer_idx].q = disp_q;
                        g_display_buffer_idx = (g_display_buffer_idx + 1) % DISPLAY_FFT_SIZE;
                        g_display_new_samples++;

                        /* With 50% overlap, run FFT every DISPLAY_OVERLAP new samples */
                        if (g_display_new_samples >= DISPLAY_OVERLAP) {
                            samples_collected = DISPLAY_OVERLAP;  /* Signal ready for FFT */
                        }
                    }
                }
            }
            got_samples = (samples_collected >= DISPLAY_OVERLAP);
        } else {
            /* Stdin mode - not used in TCP mode */
            SDL_Delay(10);
            continue;
        }

        if (!got_samples) {
            SDL_Delay(10);
            continue;
        }

        /* Reset overlap counter */
        g_display_new_samples = 0;

        /* Complex FFT of I/Q data - shows RF spectrum centered on DC */
        for (int i = 0; i < DISPLAY_FFT_SIZE; i++) {
            int buf_idx = (g_display_buffer_idx + i) % DISPLAY_FFT_SIZE;
            fft_in[i].r = g_display_buffer[buf_idx].i * window_func[i];
            fft_in[i].i = g_display_buffer[buf_idx].q * window_func[i];
        }
        kiss_fft(fft_cfg, fft_in, fft_out);

        /* Calculate magnitudes with FFT shift (DC in center) */
        float bin_hz = DISPLAY_HZ_PER_BIN;
        for (int i = 0; i < WATERFALL_WIDTH; i++) {
            /* Map pixel to frequency: left = -ZOOM_MAX_HZ, center = 0 (DC), right = +ZOOM_MAX_HZ */
            float freq = ((float)i / WATERFALL_WIDTH - 0.5f) * 2.0f * ZOOM_MAX_HZ;

            int bin;
            if (freq >= 0) {
                bin = (int)(freq / bin_hz + 0.5f);
            } else {
                bin = DISPLAY_FFT_SIZE + (int)(freq / bin_hz - 0.5f);
            }

            if (bin < 0) bin = 0;
            if (bin >= DISPLAY_FFT_SIZE) bin = DISPLAY_FFT_SIZE - 1;

            float re = fft_out[bin].r;
            float im = fft_out[bin].i;
            magnitudes[i] = sqrtf(re * re + im * im) / DISPLAY_FFT_SIZE;
        }

        /* Auto-gain tracking */
        float frame_max = -200.0f;
        float frame_min = 200.0f;
        for (int i = 0; i < WATERFALL_WIDTH; i++) {
            float db = 20.0f * log10f(magnitudes[i] + 1e-10f);
            if (db > frame_max) frame_max = db;
            if (db < frame_min) frame_min = db;
        }

        if (frame_max > g_peak_db) {
            g_peak_db += AGC_ATTACK * (frame_max - g_peak_db);
        } else {
            g_peak_db += AGC_DECAY * (frame_max - g_peak_db);
        }
        if (frame_min < g_floor_db) {
            g_floor_db += AGC_ATTACK * (frame_min - g_floor_db);
        } else {
            g_floor_db += AGC_DECAY * (frame_min - g_floor_db);
        }

        /* Scroll waterfall down by 1 row */
        memmove(pixels + WINDOW_WIDTH * 3,
                pixels,
                WINDOW_WIDTH * (WINDOW_HEIGHT - 1) * 3);

        /* Draw new row at top */
        for (int x = 0; x < WATERFALL_WIDTH; x++) {
            uint8_t r, g, b;
            magnitude_to_rgb(magnitudes[x], g_peak_db, g_floor_db, &r, &g, &b);
            pixels[x * 3 + 0] = r;
            pixels[x * 3 + 1] = g;
            pixels[x * 3 + 2] = b;
        }

        /* Calculate bucket energies (using display FFT) */
        for (int f = 0; f < NUM_TICK_FREQS; f++) {
            int freq = TICK_FREQS[f];
            int bandwidth = TICK_BW[f];

            int center_bin = (int)(freq / DISPLAY_HZ_PER_BIN + 0.5f);
            int bin_span = (int)(bandwidth / DISPLAY_HZ_PER_BIN + 0.5f);
            if (bin_span < 1) bin_span = 1;

            float pos_energy = 0.0f, neg_energy = 0.0f;
            for (int b = -bin_span; b <= bin_span; b++) {
                int pos_bin = center_bin + b;
                int neg_bin = DISPLAY_FFT_SIZE - center_bin + b;

                if (pos_bin >= 0 && pos_bin < DISPLAY_FFT_SIZE) {
                    float re = fft_out[pos_bin].r;
                    float im = fft_out[pos_bin].i;
                    pos_energy += sqrtf(re * re + im * im) / DISPLAY_FFT_SIZE;
                }
                if (neg_bin >= 0 && neg_bin < DISPLAY_FFT_SIZE) {
                    float re = fft_out[neg_bin].r;
                    float im = fft_out[neg_bin].i;
                    neg_energy += sqrtf(re * re + im * im) / DISPLAY_FFT_SIZE;
                }
            }

            g_bucket_energy[f] = pos_energy + neg_energy;
        }

        /* Log channel conditions every ~1 second (12 frames at 85ms effective) */
        if (g_channel_csv && (frame_num % 12) == 0) {
            float timestamp_ms = frame_num * DISPLAY_EFFECTIVE_MS;
            float carrier_db = 20.0f * log10f(magnitudes[WATERFALL_WIDTH/2] + 1e-10f);
            float sub500_db = 20.0f * log10f(g_bucket_energy[2] + 1e-10f);  /* 500 Hz */
            float sub600_db = 20.0f * log10f(g_bucket_energy[3] + 1e-10f);  /* 600 Hz */
            float tone1000_db = 20.0f * log10f(g_bucket_energy[4] + 1e-10f); /* 1000 Hz */
            float noise_db = g_floor_db;
            float snr_db = tone1000_db - noise_db;
            const char *quality = (snr_db > 15) ? "GOOD" : (snr_db > 8) ? "FAIR" : (snr_db > 3) ? "POOR" : "NONE";

            fprintf(g_channel_csv, "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%s\n",
                    timestamp_ms, carrier_db, snr_db, sub500_db, sub600_db, tone1000_db, noise_db, quality);
        }

        /* Log subcarrier conditions every ~1 second (12 frames) */
        if (g_subcarrier_csv && (frame_num % 12) == 0) {
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
            
            int minute = tm_info->tm_min;
            const char *expected = wwv_expected_tone(minute);
            
            float sub500_db = 20.0f * log10f(g_bucket_energy[2] + 1e-10f);  /* 500 Hz bucket */
            float sub600_db = 20.0f * log10f(g_bucket_energy[3] + 1e-10f);  /* 600 Hz bucket */
            float delta_db = sub500_db - sub600_db;
            
            const char *detected = (delta_db > 3.0f) ? "500Hz" : (delta_db < -3.0f) ? "600Hz" : "NONE";
            const char *match = (strcmp(expected, detected) == 0) ? "YES" : 
                                (strcmp(expected, "NONE") == 0) ? "-" : "NO";
            
            fprintf(g_subcarrier_csv, "%s,%.1f,%d,%s,%.1f,%.1f,%.1f,%s,%s\n",
                    time_str, frame_num * DISPLAY_EFFECTIVE_MS, minute,
                    expected, sub500_db, sub600_db, delta_db, detected, match);
            fflush(g_subcarrier_csv);
        }

        /* Draw flash bands on waterfall for all registered detectors */
        flash_draw_waterfall_bands(pixels, 0, WATERFALL_WIDTH, WINDOW_WIDTH, ZOOM_MAX_HZ);

        /* === RIGHT PANEL: Bucket energy bars === */
        int bar_width = BUCKET_WIDTH / NUM_TICK_FREQS;
        int bar_gap = 2;

        /* Clear right panel */
        for (int y = 0; y < WINDOW_HEIGHT; y++) {
            for (int x = WATERFALL_WIDTH; x < WINDOW_WIDTH; x++) {
                int idx = (y * WINDOW_WIDTH + x) * 3;
                pixels[idx + 0] = 20;  /* Dark gray background */
                pixels[idx + 1] = 20;
                pixels[idx + 2] = 20;
            }
        }

        /* Draw each bucket bar */
        for (int f = 0; f < NUM_TICK_FREQS; f++) {
            int bar_x = WATERFALL_WIDTH + f * bar_width + bar_gap;
            int bar_w = bar_width - bar_gap * 2;

            /* Use waterfall's tracked dB scale */
            float db = 20.0f * log10f(g_bucket_energy[f] + 1e-10f);
            float norm = (db - g_floor_db) / (g_peak_db - g_floor_db);
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;

            int bar_height = (int)(norm * WINDOW_HEIGHT);

            uint8_t r, g, b;

            /* Check if any detector wants to flash this bar */
            if (flash_get_bar_override(f, &r, &g, &b)) {
                bar_height = WINDOW_HEIGHT;
            } else {
                /* Color based on energy level */
                magnitude_to_rgb(g_bucket_energy[f], -20.0f, -80.0f, &r, &g, &b);
            }

            /* Draw bar from bottom up */
            for (int y = WINDOW_HEIGHT - bar_height; y < WINDOW_HEIGHT; y++) {
                for (int x = bar_x; x < bar_x + bar_w && x < WINDOW_WIDTH; x++) {
                    int idx = (y * WINDOW_WIDTH + x) * 3;
                    pixels[idx + 0] = r;
                    pixels[idx + 1] = g;
                    pixels[idx + 2] = b;
                }
            }

            /* For 1000 Hz bar: show adaptive threshold (cyan) and noise floor (green) */
            if (f == 4) {
                /* Cyan = threshold */
                float thresh_db = 20.0f * log10f(tick_detector_get_threshold(g_tick_detector) + 1e-10f);
                float thresh_norm = (thresh_db - (-80.0f)) / 60.0f;
                if (thresh_norm < 0.0f) thresh_norm = 0.0f;
                if (thresh_norm > 1.0f) thresh_norm = 1.0f;
                int thresh_y = WINDOW_HEIGHT - (int)(thresh_norm * WINDOW_HEIGHT);
                for (int dy = -1; dy <= 1; dy++) {
                    int y = thresh_y + dy;
                    if (y >= 0 && y < WINDOW_HEIGHT) {
                        for (int x = bar_x; x < bar_x + bar_w && x < WINDOW_WIDTH; x++) {
                            int idx = (y * WINDOW_WIDTH + x) * 3;
                            pixels[idx + 0] = 0;
                            pixels[idx + 1] = 255;
                            pixels[idx + 2] = 255;
                        }
                    }
                }

                /* Green = noise floor */
                float noise_db = 20.0f * log10f(tick_detector_get_noise_floor(g_tick_detector) + 1e-10f);
                float noise_norm = (noise_db - (-80.0f)) / 60.0f;
                if (noise_norm < 0.0f) noise_norm = 0.0f;
                if (noise_norm > 1.0f) noise_norm = 1.0f;
                int noise_y = WINDOW_HEIGHT - (int)(noise_norm * WINDOW_HEIGHT);
                for (int dy = -1; dy <= 1; dy++) {
                    int y = noise_y + dy;
                    if (y >= 0 && y < WINDOW_HEIGHT) {
                        for (int x = bar_x; x < bar_x + bar_w && x < WINDOW_WIDTH; x++) {
                            int idx = (y * WINDOW_WIDTH + x) * 3;
                            pixels[idx + 0] = 0;
                            pixels[idx + 1] = 255;
                            pixels[idx + 2] = 0;
                        }
                    }
                }
            }
        }

        /* Decrement flash counters for all registered sources */
        flash_decrement_all();

        SDL_UpdateTexture(texture, NULL, pixels, WINDOW_WIDTH * 3);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        frame_num++;
    }

    printf("\n");
    tick_detector_print_stats(g_tick_detector);
    marker_detector_print_stats(g_marker_detector);
    tick_detector_destroy(g_tick_detector);
    marker_detector_destroy(g_marker_detector);
    sync_detector_destroy(g_sync_detector);
    if (g_channel_csv) fclose(g_channel_csv);
    if (g_subcarrier_csv) fclose(g_subcarrier_csv);

    if (g_tcp_mode) {
        if (g_iq_sock != SOCKET_INVALID) {
            socket_close(g_iq_sock);
        }
        tcp_cleanup();
    }

    free(window_func);
    free(magnitudes);
    free(pixels);
    free(fft_out);
    free(fft_in);
    free(g_display_buffer);
    free(g_iq_buffer);
    kiss_fft_free(fft_cfg);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("Done.\n");
    return 0;
}
