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

#include <SDL.h>
#include "kiss_fft.h"
#include "version.h"
#include "tick_detector.h"

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
    uint32_t reserved[3];
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
static bool g_tcp_streaming = false;

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

/* DSP filter instances */
static lowpass_t g_display_lowpass_i;
static lowpass_t g_display_lowpass_q;
static bool g_display_dsp_initialized = false;

#define IQ_FILTER_CUTOFF    5000.0f     /* 5 kHz lowpass on I/Q */

/* Buffer for decimated I/Q samples */
typedef struct {
    float i;
    float q;
} iq_sample_t;

static iq_sample_t *g_iq_buffer = NULL;
static int g_iq_buffer_idx = 0;

/*============================================================================
 * Configuration - SIMPLIFIED SINGLE WATERFALL
 *============================================================================*/

#define WATERFALL_WIDTH 1024
#define BUCKET_WIDTH    200
#define WINDOW_WIDTH    (WATERFALL_WIDTH + BUCKET_WIDTH)
#define WINDOW_HEIGHT   600     /* Single waterfall, not split */
#define FFT_SIZE        1024    /* Display FFT - good frequency resolution */
#define SAMPLE_RATE     48000
#define ZOOM_MAX_HZ     5000.0f /* Display ±5000 Hz around DC */

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

    g_display_dsp_initialized = false;
    g_decim_counter = 0;

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

    g_decimation_factor = g_tcp_sample_rate / SAMPLE_RATE;
    if (g_decimation_factor < 1) g_decimation_factor = 1;

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

        g_decimation_factor = g_tcp_sample_rate / SAMPLE_RATE;
        if (g_decimation_factor < 1) g_decimation_factor = 1;
        g_effective_sample_rate = g_tcp_sample_rate / g_decimation_factor;
        printf("Decimation: %d:1 -> %d Hz effective\n", g_decimation_factor, g_effective_sample_rate);

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
    kiss_fft_cpx *fft_in = (kiss_fft_cpx *)malloc(FFT_SIZE * sizeof(kiss_fft_cpx));
    kiss_fft_cpx *fft_out = (kiss_fft_cpx *)malloc(FFT_SIZE * sizeof(kiss_fft_cpx));
    uint8_t *pixels = (uint8_t *)malloc(WINDOW_WIDTH * WINDOW_HEIGHT * 3);
    float *magnitudes = (float *)malloc(WATERFALL_WIDTH * sizeof(float));
    g_iq_buffer = (iq_sample_t *)malloc(FFT_SIZE * sizeof(iq_sample_t));

    if (!fft_in || !fft_out || !pixels || !magnitudes || !g_iq_buffer) {
        fprintf(stderr, "Memory allocation failed\n");
        return 1;
    }

    memset(g_iq_buffer, 0, FFT_SIZE * sizeof(iq_sample_t));
    g_iq_buffer_idx = 0;
    memset(pixels, 0, WINDOW_WIDTH * WINDOW_HEIGHT * 3);

    float *window_func = (float *)malloc(FFT_SIZE * sizeof(float));
    for (int i = 0; i < FFT_SIZE; i++) {
        window_func[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (FFT_SIZE - 1)));
    }

    printf("\nWaterfall ready. Window: %dx%d, FFT: %d bins\n", WINDOW_WIDTH, WINDOW_HEIGHT, FFT_SIZE);
    printf("Keys: +/- gain, D=detect toggle, S=stats, Q/Esc quit\n\n");

    g_tick_detector = tick_detector_create("wwv_ticks.csv");
    if (!g_tick_detector) {
        fprintf(stderr, "Failed to create tick detector\n");
        return 1;
    }
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
                } else if (event.key.keysym.sym == SDLK_MINUS || event.key.keysym.sym == SDLK_KP_MINUS) {
                    g_gain_offset -= 3.0f;
                    printf("Gain: %+.0f dB\n", g_gain_offset);
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
            while (samples_collected < FFT_SIZE && running) {
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

                if (!g_display_dsp_initialized) {
                    lowpass_init(&g_display_lowpass_i, IQ_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                    lowpass_init(&g_display_lowpass_q, IQ_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                    g_display_dsp_initialized = true;
                    printf("DSP initialized: lowpass @ %.0f Hz\n", IQ_FILTER_CUTOFF);
                }

                for (uint32_t s = 0; s < frame.num_samples && samples_collected < FFT_SIZE; s++) {
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

                    /* Lowpass filter I and Q */
                    float i_filt = lowpass_process(&g_display_lowpass_i, i_raw);
                    float q_filt = lowpass_process(&g_display_lowpass_q, q_raw);

                    /* Decimate */
                    g_decim_counter++;
                    if (g_decim_counter >= g_decimation_factor) {
                        g_decim_counter = 0;

                        /* Feed tick detector (has its own 256-point FFT) */
                        tick_detector_process_sample(g_tick_detector, i_filt, q_filt);

                        /* Store filtered I/Q for display FFT (1024-point) */
                        g_iq_buffer[g_iq_buffer_idx].i = i_filt;
                        g_iq_buffer[g_iq_buffer_idx].q = q_filt;
                        g_iq_buffer_idx = (g_iq_buffer_idx + 1) % FFT_SIZE;
                        samples_collected++;
                    }
                }
            }
            got_samples = (samples_collected >= FFT_SIZE);
        } else {
            /* Stdin mode - not used in TCP mode */
            SDL_Delay(10);
            continue;
        }

        if (!got_samples) {
            SDL_Delay(10);
            continue;
        }

        /* Complex FFT of I/Q data - shows RF spectrum centered on DC */
        for (int i = 0; i < FFT_SIZE; i++) {
            int buf_idx = (g_iq_buffer_idx + i) % FFT_SIZE;
            fft_in[i].r = g_iq_buffer[buf_idx].i * window_func[i];
            fft_in[i].i = g_iq_buffer[buf_idx].q * window_func[i];
        }
        kiss_fft(fft_cfg, fft_in, fft_out);

        /* Calculate magnitudes with FFT shift (DC in center) */
        float bin_hz = (float)SAMPLE_RATE / FFT_SIZE;
        for (int i = 0; i < WATERFALL_WIDTH; i++) {
            /* Map pixel to frequency: left = -ZOOM_MAX_HZ, center = 0 (DC), right = +ZOOM_MAX_HZ */
            float freq = ((float)i / WATERFALL_WIDTH - 0.5f) * 2.0f * ZOOM_MAX_HZ;

            int bin;
            if (freq >= 0) {
                bin = (int)(freq / bin_hz + 0.5f);
            } else {
                bin = FFT_SIZE + (int)(freq / bin_hz - 0.5f);
            }

            if (bin < 0) bin = 0;
            if (bin >= FFT_SIZE) bin = FFT_SIZE - 1;

            float re = fft_out[bin].r;
            float im = fft_out[bin].i;
            magnitudes[i] = sqrtf(re * re + im * im) / FFT_SIZE;
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

        /* Calculate bucket energies and feed tick detector */
        float hz_per_bin = (float)SAMPLE_RATE / FFT_SIZE;
        for (int f = 0; f < NUM_TICK_FREQS; f++) {
            int freq = TICK_FREQS[f];
            int bandwidth = TICK_BW[f];

            int center_bin = (int)(freq / hz_per_bin + 0.5f);
            int bin_span = (int)(bandwidth / hz_per_bin + 0.5f);
            if (bin_span < 1) bin_span = 1;

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

            g_bucket_energy[f] = pos_energy + neg_energy;
        }

        /* Draw tick flash on waterfall when detected (purple band) */
        if (tick_detector_get_flash_frames(g_tick_detector) > 0) {
            /* Draw purple markers at ±1000 Hz positions */
            float pixels_per_hz = (WATERFALL_WIDTH / 2.0f) / ZOOM_MAX_HZ;
            int center_x = WATERFALL_WIDTH / 2;
            int offset_1000 = (int)(1000.0f * pixels_per_hz);

            /* Mark at +1000 Hz */
            for (int dx = -3; dx <= 3; dx++) {
                int x = center_x + offset_1000 + dx;
                if (x >= 0 && x < WATERFALL_WIDTH) {
                    pixels[x * 3 + 0] = 180;  /* Purple */
                    pixels[x * 3 + 1] = 0;
                    pixels[x * 3 + 2] = 255;
                }
            }
            /* Mark at -1000 Hz */
            for (int dx = -3; dx <= 3; dx++) {
                int x = center_x - offset_1000 + dx;
                if (x >= 0 && x < WATERFALL_WIDTH) {
                    pixels[x * 3 + 0] = 180;  /* Purple */
                    pixels[x * 3 + 1] = 0;
                    pixels[x * 3 + 2] = 255;
                }
            }
        }

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

            /* Flash purple for 1000 Hz bar when tick detected */
            if (f == 4 && tick_detector_get_flash_frames(g_tick_detector) > 0) {
                r = 180; g = 0; b = 255;
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

        if (tick_detector_get_flash_frames(g_tick_detector) > 0) {
            tick_detector_decrement_flash(g_tick_detector);
        }

        SDL_UpdateTexture(texture, NULL, pixels, WINDOW_WIDTH * 3);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        frame_num++;
    }

    printf("\n");
    tick_detector_print_stats(g_tick_detector);
    tick_detector_destroy(g_tick_detector);

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
    free(g_iq_buffer);
    kiss_fft_free(fft_cfg);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("Done.\n");
    return 0;
}
