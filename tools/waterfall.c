/**\n * @file waterfall.c
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
/* wwv_detector_manager.h available for future refactoring - see note below */
#include "tick_detector.h"
#include "marker_detector.h"
#include "sync_detector.h"
#include "tone_tracker.h"
#include "tick_correlator.h"
#include "slow_marker_detector.h"
#include "marker_correlator.h"
#include "bcd_envelope.h"
#include "bcd_decoder.h"
#include "bcd_time_detector.h"
#include "bcd_freq_detector.h"
#include "bcd_correlator.h"
#include "waterfall_flash.h"
#include "waterfall_telemetry.h"
#include "channel_filters.h"

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
#include <windows.h>
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
static bool g_test_pattern = false;  /* Generate synthetic 1000Hz test tone */
static bool g_log_csv = false;  /* Enable CSV logging (default: UDP only) */
static bool g_reload_debug = false;  /* Reload tuned parameters from waterfall.ini */
static char g_tcp_host[256] = "localhost";
static int g_iq_port = DEFAULT_IQ_PORT;
static socket_t g_iq_sock = SOCKET_INVALID;
static uint32_t g_tcp_sample_rate = 2000000;
static uint64_t g_test_sample_count = 0;  /* Phase accumulator for test pattern */
static uint32_t g_tcp_sample_format = IQ_FORMAT_S16;
static uint64_t g_tcp_center_freq = 15000000;
static uint32_t g_tcp_gain_reduction = 0;
static uint32_t g_tcp_lna_state = 0;
static bool g_tcp_streaming = false;

/*============================================================================
 * UDP Command Listener (Runtime Modem Tuning)
 *============================================================================*/
#define CMD_PORT            3006
#define CMD_MAX_LEN         512
#define CMD_RATE_LIMIT_PER_SEC  10

static socket_t g_cmd_sock = SOCKET_INVALID;
static uint32_t g_cmd_count_this_sec = 0;
static time_t g_cmd_rate_limit_sec = 0;

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

/* Channel filters - Parallel sync/data architecture */
static sync_channel_t g_sync_channel_i;
static sync_channel_t g_sync_channel_q;
static data_channel_t g_data_channel_i;
static data_channel_t g_data_channel_q;

/* Signal normalizer - Slow AGC for gain-independent operation */
typedef struct {
    float level;
    int warmup;
} normalizer_t;

static normalizer_t g_normalizer = {0.01f, 0};

/* Periodic sync check tracking */
#define PERIODIC_CHECK_INTERVAL_SAMPLES  5000  /* 100ms at 50kHz */
static int g_periodic_check_counter = 0;

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

/* Window dimensions - can be overridden via command line */
#define DEFAULT_WATERFALL_WIDTH 1024
#define DEFAULT_BUCKET_WIDTH    200
#define DEFAULT_WINDOW_HEIGHT   600

static int g_waterfall_width = DEFAULT_WATERFALL_WIDTH;
static int g_bucket_width = DEFAULT_BUCKET_WIDTH;
static int g_window_width = DEFAULT_WATERFALL_WIDTH + DEFAULT_BUCKET_WIDTH;
static int g_window_height = DEFAULT_WINDOW_HEIGHT;
static int g_window_x = SDL_WINDOWPOS_CENTERED;
static int g_window_y = SDL_WINDOWPOS_CENTERED;

/*----------------------------------------------------------------------------
 * DETECTOR PATH (unchanged from original)
 * Purpose: Feed tick detector with samples optimized for pulse detection
 *----------------------------------------------------------------------------*/
#define DETECTOR_SAMPLE_RATE    50000       /* 50 kHz for detector (2MHz/40 = exact) */
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
/* Anti-alias filter cutoff reduced from 6 kHz to 5 kHz. At 6 kHz cutoff with
 * 12 kHz sample rate, the filter was exactly at Nyquist with no guard band.
 * Signal content above 6 kHz could alias into the passband. 5 kHz provides
 * 1 kHz margin below Nyquist. No impact on WWV detection since all signal
 * content is within ±2 kHz of center. Added v1.0.1+19, 2025-12-17. */
#define DISPLAY_FILTER_CUTOFF   5000.0f     /* 5 kHz lowpass (1 kHz Nyquist guard) */
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
static const int   TICK_BW[NUM_TICK_FREQS]    = { 10,    5,    30,   30,   100,  100,  20   };
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
 * Signal Normalization (Step 5: WWV Tick/BCD Separation)
 *============================================================================*/

static float normalize(normalizer_t *n, float i, float q) {
    float mag = sqrtf(i*i + q*q);
    float alpha = (n->warmup < 50000) ? 0.01f : 0.0001f;  /* Fast warmup, then slow */
    n->level += alpha * (mag - n->level);
    n->warmup++;
    if (n->level < 0.0001f) n->level = 0.0001f;
    return 1.0f / n->level;
}

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

/* Forward declarations for functions that use g_tick_detector (defined after detector globals) */
static void save_tick_params_to_ini(void);
static void load_tick_params_from_ini(void);
static void set_tick_threshold(float mult);
static void set_tick_adapt_down(float alpha);
static void set_tick_adapt_up(float alpha);
static void set_tick_min_duration(float ms);
static void set_corr_epoch_confidence(float threshold);
static void set_corr_max_misses(int max_misses);
static void set_marker_threshold(float mult);
static void set_marker_adapt_rate(float rate);
static void set_marker_min_duration(float ms);
static void set_sync_weight_tick(float weight);
static void set_sync_weight_marker(float weight);
static void set_sync_weight_p_marker(float weight);
static void set_sync_weight_tick_hole(float weight);
static void set_sync_weight_combined(float weight);
static void set_sync_locked_threshold(float threshold);
static void set_sync_min_retain(float threshold);
static void set_sync_tentative_init(float threshold);
static void set_sync_decay_normal(float decay);
static void set_sync_decay_recovering(float decay);
static void set_sync_tick_tolerance(float ms);
static void set_sync_marker_tolerance(float ms);
static void set_sync_p_marker_tolerance(float ms);

/*============================================================================
 * UDP Command Processor
 *============================================================================*/
static void process_modem_command(const char *cmd_buf, int len) {
    /* Rate limiting check */
    time_t now = time(NULL);
    if (now != g_cmd_rate_limit_sec) {
        g_cmd_rate_limit_sec = now;
        g_cmd_count_this_sec = 0;
    }
    if (g_cmd_count_this_sec >= CMD_RATE_LIMIT_PER_SEC) {
        telem_sendf(TELEM_RESP, "ERR RATE_LIMIT exceeded (%d/sec)\n", CMD_RATE_LIMIT_PER_SEC);
        return;
    }
    g_cmd_count_this_sec++;

    /* Null-terminate and log */
    char cmd_str[CMD_MAX_LEN + 1];
    int copy_len = (len < CMD_MAX_LEN) ? len : CMD_MAX_LEN;
    memcpy(cmd_str, cmd_buf, copy_len);
    cmd_str[copy_len] = '\0';

    /* Remove trailing newline */
    char *nl = strchr(cmd_str, '\n');
    if (nl) *nl = '\0';
    char *cr = strchr(cmd_str, '\r');
    if (cr) *cr = '\0';

    telem_sendf(TELEM_CTRL, "%s\n", cmd_str);

    /* Parse command */
    char cmd_name[64];
    float value;
    int parsed = sscanf(cmd_str, "%63s %f", cmd_name, &value);

    if (parsed < 1) {
        telem_sendf(TELEM_RESP, "ERR PARSE empty command\n");
        return;
    }

    /* Telemetry control commands (working) */
    if (strcmp(cmd_name, "ENABLE_TELEM") == 0) {
        char channel_name[64];
        if (sscanf(cmd_str, "%*s %63s", channel_name) == 1) {
            if (strcmp(channel_name, "TICK") == 0) telem_enable(TELEM_TICKS);
            else if (strcmp(channel_name, "MARK") == 0) telem_enable(TELEM_MARKERS);
            else if (strcmp(channel_name, "SYNC") == 0) telem_enable(TELEM_SYNC);
            else if (strcmp(channel_name, "CORR") == 0) telem_enable(TELEM_CORR);
            else if (strcmp(channel_name, "CONS") == 0) telem_enable(TELEM_CONSOLE);
            else {
                telem_sendf(TELEM_RESP, "ERR UNKNOWN_CHANNEL %s\n", channel_name);
                return;
            }
            telem_sendf(TELEM_RESP, "OK ENABLED %s\n", channel_name);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE ENABLE_TELEM requires channel name\n");
        }
        return;
    }

    if (strcmp(cmd_name, "DISABLE_TELEM") == 0) {
        char channel_name[64];
        if (sscanf(cmd_str, "%*s %63s", channel_name) == 1) {
            if (strcmp(channel_name, "TICK") == 0) telem_disable(TELEM_TICKS);
            else if (strcmp(channel_name, "MARK") == 0) telem_disable(TELEM_MARKERS);
            else if (strcmp(channel_name, "SYNC") == 0) telem_disable(TELEM_SYNC);
            else if (strcmp(channel_name, "CORR") == 0) telem_disable(TELEM_CORR);
            else if (strcmp(channel_name, "CONS") == 0) telem_disable(TELEM_CONSOLE);
            else {
                telem_sendf(TELEM_RESP, "ERR UNKNOWN_CHANNEL %s\n", channel_name);
                return;
            }
            telem_sendf(TELEM_RESP, "OK DISABLED %s\n", channel_name);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE DISABLE_TELEM requires channel name\n");
        }
        return;
    }

/* Tick detector parameter commands (fully functional) */
    if (strcmp(cmd_name, "SET_TICK_THRESHOLD") == 0) {
        if (parsed == 2) {
            set_tick_threshold(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_TICK_THRESHOLD requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_TICK_ADAPT_DOWN") == 0) {
        if (parsed == 2) {
            set_tick_adapt_down(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_TICK_ADAPT_DOWN requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_TICK_ADAPT_UP") == 0) {
        if (parsed == 2) {
            set_tick_adapt_up(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_TICK_ADAPT_UP requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_TICK_MIN_DURATION") == 0) {
        if (parsed == 2) {
            set_tick_min_duration(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_TICK_MIN_DURATION requires numeric value\n");
        }
        return;
    }

    /* Tick correlator parameter commands */
    if (strcmp(cmd_name, "SET_CORR_CONFIDENCE") == 0) {
        if (parsed == 2) {
            set_corr_epoch_confidence(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_CORR_CONFIDENCE requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_CORR_MAX_MISSES") == 0) {
        if (parsed == 2) {
            set_corr_max_misses((int)value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_CORR_MAX_MISSES requires numeric value\n");
        }
        return;
    }

    /* Marker detector parameter commands */
    if (strcmp(cmd_name, "SET_MARKER_THRESHOLD") == 0) {
        if (parsed == 2) {
            set_marker_threshold(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_MARKER_THRESHOLD requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_MARKER_ADAPT_RATE") == 0) {
        if (parsed == 2) {
            set_marker_adapt_rate(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_MARKER_ADAPT_RATE requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_MARKER_MIN_DURATION") == 0) {
        if (parsed == 2) {
            set_marker_min_duration(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_MARKER_MIN_DURATION requires numeric value\n");
        }
        return;
    }

    /* Sync detector parameter commands */
    if (strcmp(cmd_name, "SET_SYNC_WEIGHT_TICK") == 0) {
        if (parsed == 2) {
            set_sync_weight_tick(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_WEIGHT_TICK requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_WEIGHT_MARKER") == 0) {
        if (parsed == 2) {
            set_sync_weight_marker(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_WEIGHT_MARKER requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_WEIGHT_P_MARKER") == 0) {
        if (parsed == 2) {
            set_sync_weight_p_marker(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_WEIGHT_P_MARKER requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_WEIGHT_TICK_HOLE") == 0) {
        if (parsed == 2) {
            set_sync_weight_tick_hole(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_WEIGHT_TICK_HOLE requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_WEIGHT_COMBINED") == 0) {
        if (parsed == 2) {
            set_sync_weight_combined(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_WEIGHT_COMBINED requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_LOCKED_THRESHOLD") == 0) {
        if (parsed == 2) {
            set_sync_locked_threshold(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_LOCKED_THRESHOLD requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_MIN_RETAIN") == 0) {
        if (parsed == 2) {
            set_sync_min_retain(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_MIN_RETAIN requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_TENTATIVE_INIT") == 0) {
        if (parsed == 2) {
            set_sync_tentative_init(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_TENTATIVE_INIT requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_DECAY_NORMAL") == 0) {
        if (parsed == 2) {
            set_sync_decay_normal(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_DECAY_NORMAL requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_DECAY_RECOVERING") == 0) {
        if (parsed == 2) {
            set_sync_decay_recovering(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_DECAY_RECOVERING requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_TICK_TOLERANCE") == 0) {
        if (parsed == 2) {
            set_sync_tick_tolerance(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_TICK_TOLERANCE requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_MARKER_TOLERANCE") == 0) {
        if (parsed == 2) {
            set_sync_marker_tolerance(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_MARKER_TOLERANCE requires numeric value\n");
        }
        return;
    }

    if (strcmp(cmd_name, "SET_SYNC_P_MARKER_TOLERANCE") == 0) {
        if (parsed == 2) {
            set_sync_p_marker_tolerance(value);
        } else {
            telem_sendf(TELEM_RESP, "ERR PARSE SET_SYNC_P_MARKER_TOLERANCE requires numeric value\n");
        }
        return;
    }

    /* Unknown command */
    telem_sendf(TELEM_RESP, "ERR UNKNOWN_CMD %s\n", cmd_name);
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

    /* Reset normalizer */
    g_normalizer.level = 0.01f;
    g_normalizer.warmup = 0;

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
    printf("  -t, --tcp HOST[:PORT]   Connect to SDR server I/Q port (default localhost:%d)\n", DEFAULT_IQ_PORT);
    printf("  --stdin                 Read from stdin instead of TCP\n");
    printf("  --test-pattern          Generate synthetic 1000Hz test tone (no SDR needed)\n");
    printf("  -w, --width WIDTH       Set waterfall width (default: %d)\n", DEFAULT_WATERFALL_WIDTH);
    printf("  -H, --height HEIGHT     Set window height (default: %d)\n", DEFAULT_WINDOW_HEIGHT);
    printf("  -x, --pos-x X           Set window X position (default: centered)\n");
    printf("  -y, --pos-y Y           Set window Y position (default: centered)\n");
    printf("  -l, --log-csv           Enable CSV file logging (default: UDP telemetry only)\n");
    printf("  --reload-debug          Reload tuned parameters from waterfall.ini\n");
    printf("  -h, --help              Show this help\n\n");
    printf("UDP Telemetry:          Broadcast on port 3005 (always enabled)\n");
    printf("Control Interface:      Type commands in console (freq, gain, status, etc.)\n");
    printf("See: docs/UDP_TELEMETRY_OUTPUT_PROTOCOL.md\n");
}

/*============================================================================
 * WWV Detector Instances
 *
 * NOTE: wwv_detector_manager.c/.h provides a cleaner abstraction that
 * encapsulates all detector orchestration. However, migrating to it
 * requires updating the flash system and UI code that directly access
 * detector state. For now, we keep direct detector ownership here.
 *
 * See wwv_detector_manager.h for the recommended architecture.
 *============================================================================*/

static tick_detector_t *g_tick_detector = NULL;
static marker_detector_t *g_marker_detector = NULL;
static bcd_envelope_t *g_bcd_envelope = NULL;  /* DEPRECATED: Use bcd_time/freq_detector + bcd_correlator */
static bcd_decoder_t *g_bcd_decoder = NULL;    /* DEPRECATED: Use bcd_correlator */
static sync_detector_t *g_sync_detector = NULL;
static slow_marker_detector_t *g_slow_marker = NULL;
static marker_correlator_t *g_marker_correlator = NULL;
static bcd_time_detector_t *g_bcd_time_detector = NULL;
static bcd_freq_detector_t *g_bcd_freq_detector = NULL;
static bcd_correlator_t *g_bcd_correlator = NULL;
static FILE *g_channel_csv = NULL;
static uint64_t g_channel_log_interval = 0;  /* Log every N frames */
static FILE *g_subcarrier_csv = NULL;
static tone_tracker_t *g_tone_carrier = NULL;
static tone_tracker_t *g_tone_500 = NULL;
static tone_tracker_t *g_tone_600 = NULL;
static tick_correlator_t *g_tick_correlator = NULL;

/*============================================================================
 * Tick Detector Parameter Control (calls setters + persists to INI)
 *============================================================================*/

static void set_tick_threshold(float mult) {
    if (tick_detector_set_threshold_mult(g_tick_detector, mult)) {
        save_tick_params_to_ini();
        telem_sendf(TELEM_RESP, "OK threshold_multiplier=%.3f\n", mult);
    } else {
        telem_sendf(TELEM_RESP, "ERR 400 Invalid threshold_multiplier=%.3f (range 1.0-5.0)\n", mult);
    }
}

static void set_tick_adapt_down(float alpha) {
    if (tick_detector_set_adapt_alpha_down(g_tick_detector, alpha)) {
        save_tick_params_to_ini();
        telem_sendf(TELEM_RESP, "OK adapt_alpha_down=%.6f\n", alpha);
    } else {
        telem_sendf(TELEM_RESP, "ERR 400 Invalid adapt_alpha_down=%.6f (range 0.9-0.999)\n", alpha);
    }
}

static void set_tick_adapt_up(float alpha) {
    if (tick_detector_set_adapt_alpha_up(g_tick_detector, alpha)) {
        save_tick_params_to_ini();
        telem_sendf(TELEM_RESP, "OK adapt_alpha_up=%.6f\n", alpha);
    } else {
        telem_sendf(TELEM_RESP, "ERR 400 Invalid adapt_alpha_up=%.6f (range 0.001-0.1)\n", alpha);
    }
}

static void set_tick_min_duration(float ms) {
    if (tick_detector_set_min_duration_ms(g_tick_detector, ms)) {
        save_tick_params_to_ini();
        telem_sendf(TELEM_RESP, "OK min_duration_ms=%.2f\n", ms);
    } else {
        telem_sendf(TELEM_RESP, "ERR 400 Invalid min_duration_ms=%.2f (range 1.0-10.0)\n", ms);
    }
}

static void set_corr_epoch_confidence(float threshold) {
    tick_correlator_set_epoch_confidence(g_tick_correlator, threshold);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK epoch_confidence_threshold=%.3f\n", threshold);
}

static void set_corr_max_misses(int max_misses) {
    tick_correlator_set_max_misses(g_tick_correlator, max_misses);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK max_consecutive_misses=%d\n", max_misses);
}

static void set_marker_threshold(float mult) {
    marker_detector_set_threshold_mult(g_marker_detector, mult);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK marker_threshold_multiplier=%.3f\n", mult);
}

static void set_marker_adapt_rate(float rate) {
    marker_detector_set_noise_adapt_rate(g_marker_detector, rate);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK marker_noise_adapt_rate=%.6f\n", rate);
}

static void set_marker_min_duration(float ms) {
    marker_detector_set_min_duration_ms(g_marker_detector, ms);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK marker_min_duration_ms=%.2f\n", ms);
}

static void set_sync_weight_tick(float weight) {
    sync_detector_set_weight_tick(g_sync_detector, weight);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK weight_tick=%.3f\n", weight);
}

static void set_sync_weight_marker(float weight) {
    sync_detector_set_weight_marker(g_sync_detector, weight);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK weight_marker=%.3f\n", weight);
}

static void set_sync_weight_p_marker(float weight) {
    sync_detector_set_weight_p_marker(g_sync_detector, weight);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK weight_p_marker=%.3f\n", weight);
}

static void set_sync_weight_tick_hole(float weight) {
    sync_detector_set_weight_tick_hole(g_sync_detector, weight);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK weight_tick_hole=%.3f\n", weight);
}

static void set_sync_weight_combined(float weight) {
    sync_detector_set_weight_combined(g_sync_detector, weight);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK weight_combined_hole_marker=%.3f\n", weight);
}

static void set_sync_locked_threshold(float threshold) {
    sync_detector_set_locked_threshold(g_sync_detector, threshold);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK confidence_locked_threshold=%.3f\n", threshold);
}

static void set_sync_min_retain(float threshold) {
    sync_detector_set_min_retain(g_sync_detector, threshold);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK confidence_min_retain=%.3f\n", threshold);
}

static void set_sync_tentative_init(float threshold) {
    sync_detector_set_tentative_init(g_sync_detector, threshold);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK confidence_tentative_init=%.3f\n", threshold);
}

static void set_sync_decay_normal(float decay) {
    sync_detector_set_decay_normal(g_sync_detector, decay);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK confidence_decay_normal=%.4f\n", decay);
}

static void set_sync_decay_recovering(float decay) {
    sync_detector_set_decay_recovering(g_sync_detector, decay);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK confidence_decay_recovering=%.4f\n", decay);
}

static void set_sync_tick_tolerance(float ms) {
    sync_detector_set_tick_tolerance(g_sync_detector, ms);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK tick_phase_tolerance_ms=%.1f\n", ms);
}

static void set_sync_marker_tolerance(float ms) {
    sync_detector_set_marker_tolerance(g_sync_detector, ms);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK marker_tolerance_ms=%.1f\n", ms);
}

static void set_sync_p_marker_tolerance(float ms) {
    sync_detector_set_p_marker_tolerance(g_sync_detector, ms);
    save_tick_params_to_ini();
    telem_sendf(TELEM_RESP, "OK p_marker_tolerance_ms=%.1f\n", ms);
}

/*============================================================================
 * INI File Persistence for Tunable Parameters
 *============================================================================*/

static void save_tick_params_to_ini(void) {
    FILE *f = fopen("waterfall.ini", "w");
    if (!f) {
        telem_sendf(TELEM_CONSOLE, "[WARN] Could not write waterfall.ini\n");
        return;
    }

    fprintf(f, "[tick_detector]\n");
    fprintf(f, "threshold_multiplier=%.3f\n", tick_detector_get_threshold_mult(g_tick_detector));
    fprintf(f, "adapt_alpha_down=%.6f\n", tick_detector_get_adapt_alpha_down(g_tick_detector));
    fprintf(f, "adapt_alpha_up=%.6f\n", tick_detector_get_adapt_alpha_up(g_tick_detector));
    fprintf(f, "min_duration_ms=%.2f\n", tick_detector_get_min_duration_ms(g_tick_detector));

    fprintf(f, "\n[tick_correlator]\n");
    fprintf(f, "epoch_confidence_threshold=%.3f\n", tick_correlator_get_epoch_confidence(g_tick_correlator));
    fprintf(f, "max_consecutive_misses=%d\n", tick_correlator_get_max_misses(g_tick_correlator));

    fprintf(f, "\n[marker_detector]\n");
    fprintf(f, "threshold_multiplier=%.3f\n", marker_detector_get_threshold_mult(g_marker_detector));
    fprintf(f, "noise_adapt_rate=%.6f\n", marker_detector_get_noise_adapt_rate(g_marker_detector));
    fprintf(f, "min_duration_ms=%.2f\n", marker_detector_get_min_duration_ms(g_marker_detector));

    fprintf(f, "\n[sync_detector]\n");
    fprintf(f, "weight_tick=%.3f\n", sync_detector_get_weight_tick(g_sync_detector));
    fprintf(f, "weight_marker=%.3f\n", sync_detector_get_weight_marker(g_sync_detector));
    fprintf(f, "weight_p_marker=%.3f\n", sync_detector_get_weight_p_marker(g_sync_detector));
    fprintf(f, "weight_tick_hole=%.3f\n", sync_detector_get_weight_tick_hole(g_sync_detector));
    fprintf(f, "weight_combined_hole_marker=%.3f\n", sync_detector_get_weight_combined(g_sync_detector));
    fprintf(f, "confidence_locked_threshold=%.3f\n", sync_detector_get_locked_threshold(g_sync_detector));
    fprintf(f, "confidence_min_retain=%.3f\n", sync_detector_get_min_retain(g_sync_detector));
    fprintf(f, "confidence_tentative_init=%.3f\n", sync_detector_get_tentative_init(g_sync_detector));
    fprintf(f, "confidence_decay_normal=%.4f\n", sync_detector_get_decay_normal(g_sync_detector));
    fprintf(f, "confidence_decay_recovering=%.4f\n", sync_detector_get_decay_recovering(g_sync_detector));
    fprintf(f, "tick_phase_tolerance_ms=%.1f\n", sync_detector_get_tick_tolerance(g_sync_detector));
    fprintf(f, "marker_tolerance_ms=%.1f\n", sync_detector_get_marker_tolerance(g_sync_detector));
    fprintf(f, "p_marker_tolerance_ms=%.1f\n", sync_detector_get_p_marker_tolerance(g_sync_detector));

    fclose(f);
}

static void load_tick_params_from_ini(void) {
    FILE *f = fopen("waterfall.ini", "r");
    if (!f) {
        telem_sendf(TELEM_CONSOLE, "[INIT] No waterfall.ini found, using defaults\n");
        return;
    }

    char line[256];
    bool in_tick_section = false;
    bool in_corr_section = false;
    bool in_marker_section = false;
    bool in_sync_section = false;
    int params_loaded = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Check for section header */
        if (strstr(line, "[tick_detector]")) {
            in_tick_section = true;
            in_corr_section = false;
            in_marker_section = false;
            continue;
        }
        if (strstr(line, "[tick_correlator]")) {
            in_tick_section = false;
            in_corr_section = true;
            in_marker_section = false;
            continue;
        }
        if (strstr(line, "[marker_detector]")) {
            in_tick_section = false;
            in_corr_section = false;
            in_marker_section = true;
            in_sync_section = false;
            continue;
        }
        if (strstr(line, "[sync_detector]")) {
            in_tick_section = false;
            in_corr_section = false;
            in_marker_section = false;
            in_sync_section = true;
            continue;
        }
        if (line[0] == '[') {
            in_tick_section = false;
            in_corr_section = false;
            in_marker_section = false;
            in_sync_section = false;
            continue;
        }

        /* Parse key=value */
        char *eq = strchr(line, '=');
        if (!eq) continue;

        float value = (float)atof(eq + 1);

        if (in_tick_section) {
            if (strstr(line, "threshold_multiplier=")) {
                if (tick_detector_set_threshold_mult(g_tick_detector, value)) {
                    params_loaded++;
                } else {
                    telem_sendf(TELEM_CONSOLE, "[WARN] Invalid threshold_multiplier=%.3f in INI, using default\n", value);
                }
            } else if (strstr(line, "adapt_alpha_down=")) {
                if (tick_detector_set_adapt_alpha_down(g_tick_detector, value)) {
                    params_loaded++;
                } else {
                    telem_sendf(TELEM_CONSOLE, "[WARN] Invalid adapt_alpha_down=%.6f in INI, using default\n", value);
                }
            } else if (strstr(line, "adapt_alpha_up=")) {
                if (tick_detector_set_adapt_alpha_up(g_tick_detector, value)) {
                    params_loaded++;
                } else {
                    telem_sendf(TELEM_CONSOLE, "[WARN] Invalid adapt_alpha_up=%.6f in INI, using default\n", value);
                }
            } else if (strstr(line, "min_duration_ms=")) {
                if (tick_detector_set_min_duration_ms(g_tick_detector, value)) {
                    params_loaded++;
                } else {
                    telem_sendf(TELEM_CONSOLE, "[WARN] Invalid min_duration_ms=%.2f in INI, using default\n", value);
                }
            }
        } else if (in_corr_section) {
            if (strstr(line, "epoch_confidence_threshold=")) {
                tick_correlator_set_epoch_confidence(g_tick_correlator, value);
                params_loaded++;
            } else if (strstr(line, "max_consecutive_misses=")) {
                tick_correlator_set_max_misses(g_tick_correlator, (int)value);
                params_loaded++;
            }
        } else if (in_marker_section) {
            if (strstr(line, "threshold_multiplier=")) {
                marker_detector_set_threshold_mult(g_marker_detector, value);
                params_loaded++;
            } else if (strstr(line, "noise_adapt_rate=")) {
                marker_detector_set_noise_adapt_rate(g_marker_detector, value);
                params_loaded++;
            } else if (strstr(line, "min_duration_ms=")) {
                marker_detector_set_min_duration_ms(g_marker_detector, value);
                params_loaded++;
            }
        } else if (in_sync_section) {
            if (strstr(line, "weight_tick=")) {
                sync_detector_set_weight_tick(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "weight_marker=")) {
                sync_detector_set_weight_marker(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "weight_p_marker=")) {
                sync_detector_set_weight_p_marker(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "weight_tick_hole=")) {
                sync_detector_set_weight_tick_hole(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "weight_combined_hole_marker=")) {
                sync_detector_set_weight_combined(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "confidence_locked_threshold=")) {
                sync_detector_set_locked_threshold(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "confidence_min_retain=")) {
                sync_detector_set_min_retain(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "confidence_tentative_init=")) {
                sync_detector_set_tentative_init(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "confidence_decay_normal=")) {
                sync_detector_set_decay_normal(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "confidence_decay_recovering=")) {
                sync_detector_set_decay_recovering(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "tick_phase_tolerance_ms=")) {
                sync_detector_set_tick_tolerance(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "marker_tolerance_ms=")) {
                sync_detector_set_marker_tolerance(g_sync_detector, value);
                params_loaded++;
            } else if (strstr(line, "p_marker_tolerance_ms=")) {
                sync_detector_set_p_marker_tolerance(g_sync_detector, value);
                params_loaded++;
            }
        }
    }

    fclose(f);

    if (params_loaded > 0) {
        telem_sendf(TELEM_CONSOLE, "[INIT] Loaded %d debug parameters from waterfall.ini\n", params_loaded);
    }
}

/*============================================================================
 * Sync Detector Callback Wrappers
 *============================================================================*/

/**
 * Tick chain epoch callback - called when correlator establishes precise second epoch
 */
static void on_tick_chain_epoch(float epoch_offset_ms, float std_dev_ms, float confidence, void *user_data) {
    (void)user_data;

    /* Tick chain has established precise second epoch - update tick detector */
    if (g_tick_detector) {
        /* Only update if we don't already have a better source, or this is higher confidence */
        epoch_source_t current_source = tick_detector_get_epoch_source(g_tick_detector);
        float current_confidence = tick_detector_get_epoch_confidence(g_tick_detector);

        if (current_source != EPOCH_SOURCE_TICK_CHAIN || confidence > current_confidence) {
            tick_detector_set_epoch_with_source(g_tick_detector, epoch_offset_ms,
                                                 EPOCH_SOURCE_TICK_CHAIN, confidence);

            /* DISABLED: Testing whether BCD 10th harmonic actually causes false positives */
            // if (!tick_detector_is_gating_enabled(g_tick_detector)) {
            //     tick_detector_set_gating_enabled(g_tick_detector, true);
            // }
        }
    }
}

static void on_tick_marker(const tick_marker_event_t *event, void *user_data) {
    (void)user_data;

    /* Use leading edge directly from tick_detector (already compensated for filter delay).
     * start_timestamp_ms = trailing - duration - TICK_FILTER_DELAY_MS
     * This is the fast path (256-pt FFT @ 50kHz) - most precise timing. */
    float leading_edge_ms = event->start_timestamp_ms;

    /* Feed sync detector with LEADING EDGE (actual second boundary).
     * BCD correlator uses sync's last_marker_ms as anchor for 1-second windows.
     * Passing trailing edge would misalign windows by ~800ms (pulse duration). */
    if (g_sync_detector) {
        sync_detector_tick_marker(g_sync_detector, leading_edge_ms,
                                   event->duration_ms, event->corr_ratio);
    }

    /* Set epoch from tick detector's precise measurement - but only as fallback
     * if tick chain hasn't already established better epoch. */
    if (g_tick_detector) {
        epoch_source_t current_source = tick_detector_get_epoch_source(g_tick_detector);

        telem_console("[EPOCH] FAST trailing=%.1fms dur=%.0fms leading=%.1fms\n",
                      event->timestamp_ms, event->duration_ms, leading_edge_ms);

        /* Only set marker epoch if we don't have tick chain epoch yet */
        if (current_source == EPOCH_SOURCE_NONE) {
            tick_detector_set_epoch_with_source(g_tick_detector, leading_edge_ms,
                                                 EPOCH_SOURCE_MARKER, 0.7f);

            /* DISABLED: Testing whether BCD 10th harmonic actually causes false positives */
            // if (!tick_detector_is_gating_enabled(g_tick_detector)) {
            //     tick_detector_set_gating_enabled(g_tick_detector, true);
            // }
        }
    }
}

static void on_slow_marker_frame(const slow_marker_frame_t *frame, void *user_data) {
    (void)user_data;

    /* Feed correlator */
    if (g_marker_correlator) {
        marker_correlator_slow_frame(g_marker_correlator,
                                      frame->timestamp_ms,
                                      frame->energy,
                                      frame->snr_db,
                                      frame->above_threshold);
    }

    /* DISABLED: External baseline from slow marker doesn't work - different FFT configs
    * Slow marker: 12kHz/2048-pt FFT (5.86 Hz/bin)
    * Fast marker: 50kHz/256-pt FFT (195 Hz/bin)
    * The noise_floor values have incompatible scaling.
    * Self-tracking baseline works correctly (proven in v133).
    *
     * The external baseline API has been removed from marker_detector.c entirely.
     */
}

static void on_marker_event(const marker_event_t *event, void *user_data) {
    (void)user_data;

    /* Feed correlator */
    if (g_marker_correlator) {
        marker_correlator_fast_event(g_marker_correlator,
                                      event->timestamp_ms,
                                      event->duration_ms);
    }

    /* Get pending tick info BEFORE sync_detector_marker_event() clears it.
     * Order of operations bug: sync_detector_marker_event() -> confirm_marker()
     * clears tick_pending, so we must capture the tick info first. */
    float tick_timestamp_ms = 0.0f, tick_duration_ms = 0.0f;
    bool have_tick = false;
    if (g_sync_detector) {
        have_tick = sync_detector_get_pending_tick(g_sync_detector,
                                                    &tick_timestamp_ms,
                                                    &tick_duration_ms);
    }

    /* Feed sync detector (this will clear tick_pending internally) */
    if (g_sync_detector) {
        sync_detector_marker_event(g_sync_detector, event->timestamp_ms,
                                    event->accumulated_energy, event->duration_ms);
    }

    /* Feed tick detector timing gate (marker bootstrap).
     * Dual-path agreement validation: compare fast (tick_detector) to slow (marker_detector). */
    if (g_tick_detector) {
        epoch_source_t current_source = tick_detector_get_epoch_source(g_tick_detector);
        float leading_edge_ms;
        float slow_marker_leading_edge_ms;

        /* Slow marker path: trailing edge - estimated total delay - filter delay
         * (800ms pulse + ~400ms accumulator delay = 1200ms + 3ms filter) */
        #define SLOW_MARKER_TOTAL_DELAY_MS  1200.0f
        slow_marker_leading_edge_ms = event->timestamp_ms - SLOW_MARKER_TOTAL_DELAY_MS - 3.0f;

        /* Prefer tick detector timestamp/duration when available (more precise).
         * Only fall back to slow marker estimate when tick detector didn't see it. */
        if (have_tick && tick_duration_ms > 0.0f) {
            /* Fast path (tick detector): trailing edge - actual duration - filter delay (3.0ms) */
            leading_edge_ms = tick_timestamp_ms - tick_duration_ms - 3.0f;

            /* Dual-path agreement check */
            float disagreement_ms = fabsf(leading_edge_ms - slow_marker_leading_edge_ms);
            const char *quality = (disagreement_ms < 20.0f) ? "GOOD" :
                                  (disagreement_ms < 50.0f) ? "FAIR" : "POOR";

            telem_console("[EPOCH] FAST=%.1fms SLOW=%.1fms diff=%.1fms [%s]\n",
                          leading_edge_ms, slow_marker_leading_edge_ms, disagreement_ms, quality);

            if (disagreement_ms > 50.0f) {
                telem_console("[WARN] Dual-path disagreement >50ms - possible fading or interference\n");
            }
        } else {
            /* Fallback to slow marker only */
            leading_edge_ms = slow_marker_leading_edge_ms;
            telem_console("[EPOCH] SLOW-ONLY trailing=%.1fms total_delay=%.0fms leading=%.1fms\n",
                          event->timestamp_ms, SLOW_MARKER_TOTAL_DELAY_MS, leading_edge_ms);
        }

        /* Only set marker epoch if we don't have tick chain epoch yet */
        if (current_source == EPOCH_SOURCE_NONE) {
            tick_detector_set_epoch_with_source(g_tick_detector, leading_edge_ms,
                                                 EPOCH_SOURCE_MARKER, 0.7f);

            /* DISABLED: Testing whether BCD 10th harmonic actually causes false positives */
            // if (!tick_detector_is_gating_enabled(g_tick_detector)) {
            //     tick_detector_set_gating_enabled(g_tick_detector, true);
            // }
        }
    }
}

/*============================================================================
 * Marker Correlator Callback (Orphaned Markers -> P-markers)
 *============================================================================*/

static void on_orphaned_marker(const correlated_marker_t *marker, void *user_data) {
    (void)user_data;

    /* Feed orphaned markers to sync detector as P-marker evidence */
    if (g_sync_detector && marker->confidence == MARKER_CONF_LOW) {
        /* Low confidence = only one path triggered = orphaned = potential P-marker */
        sync_detector_p_marker_event(g_sync_detector, marker->timestamp_ms,
                                      marker->duration_ms);
    }
}

/*============================================================================
 * Tick Correlator Callback
 *============================================================================*/

static void on_tick_event(const tick_event_t *event, void *user_data) {
    (void)user_data;
    if (g_tick_correlator) {
        /* Get wall clock time string */
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_str[16];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

        tick_correlator_add_tick(g_tick_correlator,
                                  time_str,
                                  event->timestamp_ms,
                                  event->tick_number,
                                  "TICK",  /* TODO: get from wwv_clock */
                                  event->peak_energy,
                                  event->duration_ms,
                                  event->interval_ms,
                                  event->avg_interval_ms,
                                  event->noise_floor,
                                  event->corr_peak,
                                  event->corr_ratio);
    }
}

/*============================================================================
 * BCD Symbol Callback
 *============================================================================*/

static void on_bcd_symbol(bcd_symbol_t symbol, float timestamp_ms,
                          float pulse_width_ms, void *user_data) {
    (void)user_data;
    const char *sym_str = (symbol == BCD_SYMBOL_ZERO) ? "0" :
                          (symbol == BCD_SYMBOL_ONE) ? "1" :
                          (symbol == BCD_SYMBOL_MARKER) ? "P" : "?";
    telem_sendf(TELEM_BCDS, "SYM,%s,%.1f,%.1f", sym_str, timestamp_ms, pulse_width_ms);
}

/*============================================================================
 * BCD Dual-Path Detector Callbacks (wrappers for correlator)
 *============================================================================*/

static void on_bcd_time_event(const bcd_time_event_t *event, void *user_data) {
    bcd_correlator_t *corr = (bcd_correlator_t *)user_data;
    if (corr && event) {
        bcd_correlator_time_event(corr, event->timestamp_ms, event->duration_ms, event->peak_energy);
    }
}

static void on_bcd_freq_event(const bcd_freq_event_t *event, void *user_data) {
    bcd_correlator_t *corr = (bcd_correlator_t *)user_data;
    if (corr && event) {
        bcd_correlator_freq_event(corr, event->timestamp_ms, event->duration_ms, event->accumulated_energy);
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
        if ((strcmp(argv[i], "--tcp") == 0 || strcmp(argv[i], "-t") == 0) && i + 1 < argc) {
            g_tcp_mode = true;
            g_stdin_mode = false;
            g_test_pattern = false;
            parse_tcp_arg(argv[++i]);
        } else if (strcmp(argv[i], "--stdin") == 0) {
            g_stdin_mode = true;
            g_tcp_mode = false;
            g_test_pattern = false;
        } else if (strcmp(argv[i], "--test-pattern") == 0) {
            g_test_pattern = true;
            g_tcp_mode = false;
            g_stdin_mode = false;
        } else if ((strcmp(argv[i], "--width") == 0 || strcmp(argv[i], "-w") == 0) && i + 1 < argc) {
            g_waterfall_width = atoi(argv[++i]);
            if (g_waterfall_width < 400) g_waterfall_width = 400;  /* Minimum */
            g_window_width = g_waterfall_width + g_bucket_width;
        } else if ((strcmp(argv[i], "--height") == 0 || strcmp(argv[i], "-H") == 0) && i + 1 < argc) {
            g_window_height = atoi(argv[++i]);
            if (g_window_height < 300) g_window_height = 300;  /* Minimum */
        } else if ((strcmp(argv[i], "--pos-x") == 0 || strcmp(argv[i], "-x") == 0) && i + 1 < argc) {
            g_window_x = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--pos-y") == 0 || strcmp(argv[i], "-y") == 0) && i + 1 < argc) {
            g_window_y = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log-csv") == 0 || strcmp(argv[i], "-l") == 0) {
            g_log_csv = true;
        } else if (strcmp(argv[i], "--reload-debug") == 0) {
            g_reload_debug = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    print_version("Phoenix SDR - Waterfall");

    if (g_test_pattern) {
        printf("Test Pattern Mode: Generating synthetic 1000Hz tone\n");
    } else if (g_tcp_mode) {
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

    const char *title = g_test_pattern ? "Waterfall (Test Pattern)" :
                        g_tcp_mode ? "Waterfall (TCP)" : "Waterfall";
    SDL_Window *window = SDL_CreateWindow(
        title,
        g_window_x, g_window_y,
        g_window_width, g_window_height,
        SDL_WINDOW_SHOWN
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

#ifdef _WIN32
    /* Minimize the console window now that GUI is up */
    HWND console = GetConsoleWindow();
    if (console) {
        ShowWindow(console, SW_MINIMIZE);
    }
#endif

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
    uint8_t *pixels = (uint8_t *)malloc(g_window_width * g_window_height * 3);
    float *magnitudes = (float *)malloc(g_waterfall_width * sizeof(float));

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
    memset(pixels, 0, g_window_width * g_window_height * 3);

    /* Blackman-Harris window for display FFT (better sidelobe suppression) */
    float *window_func = (float *)malloc(DISPLAY_FFT_SIZE * sizeof(float));
    generate_blackman_harris(window_func, DISPLAY_FFT_SIZE);

    printf("\nWaterfall ready. Window: %dx%d\n", g_window_width, g_window_height);
    printf("Display: %d-pt FFT, Blackman-Harris, 50%% overlap\n", DISPLAY_FFT_SIZE);
    printf("Resolution: %.1f Hz/bin, %.1f ms effective update\n", DISPLAY_HZ_PER_BIN, DISPLAY_EFFECTIVE_MS);
    printf("Keys: +/- gain, D=detect toggle, S=stats, Q/Esc quit\n\n");

    g_tick_detector = tick_detector_create(g_log_csv ? "wwv_ticks.csv" : NULL);
    if (!g_tick_detector) {
        fprintf(stderr, "Failed to create tick detector\n");
        return 1;
    }

    /* Load tuned parameters from INI if --reload-debug flag set */
    if (g_reload_debug) {
        load_tick_params_from_ini();
    }

    g_marker_detector = marker_detector_create(g_log_csv ? "wwv_markers.csv" : NULL);
    if (!g_marker_detector) {
        fprintf(stderr, "Failed to create marker detector\n");
        return 1;
    }
    /* DISABLED: Subcarrier baseline doesn't work - 500/600 Hz energy is ~720x lower
     * than 1000 Hz energy due to different frequency characteristics. Using it causes
     * baseline to collapse to ~2.5, making everything look like a marker.
     * See analysis from overnight 12/17/2025 run: 5 hours, 0 detections, stuck in
     * IN_MARKER->COOLDOWN cycle due to baseline=2.5 vs accumulated=1800.
     * Self-tracking baseline works correctly (proven in v133).
     *
     * The external baseline API has been removed from marker_detector.c entirely.
     */

    /* Create slow marker detector */
    g_slow_marker = slow_marker_detector_create();
    if (!g_slow_marker) {
        fprintf(stderr, "Failed to create slow marker detector\n");
        return 1;
    }
    slow_marker_detector_set_callback(g_slow_marker, on_slow_marker_frame, NULL);

    /* DEPRECATED: Create BCD envelope tracker (100 Hz)
     * Use bcd_time_detector + bcd_freq_detector + bcd_correlator instead */
    g_bcd_envelope = bcd_envelope_create(g_log_csv ? "wwv_bcd.csv" : NULL);
    if (!g_bcd_envelope) {
        fprintf(stderr, "Failed to create BCD envelope tracker\n");
        return 1;
    }

    /* DEPRECATED: BCD decoder still created for legacy telemetry but callback DISABLED
     * Symbol output now comes from bcd_correlator which gates on sync LOCKED */
    g_bcd_decoder = bcd_decoder_create();
    if (!g_bcd_decoder) {
        fprintf(stderr, "Failed to create BCD decoder\n");
        return 1;
    }
    /* REMOVED: bcd_decoder_set_symbol_callback - was causing 2x symbol output
     * bcd_decoder_set_symbol_callback(g_bcd_decoder, on_bcd_symbol, NULL); */

    /* Create BCD dual-path detectors (robust symbol demodulator) */
    g_bcd_time_detector = bcd_time_detector_create(g_log_csv ? "logs/wwv_bcd_time.csv" : NULL);
    g_bcd_freq_detector = bcd_freq_detector_create(g_log_csv ? "logs/wwv_bcd_freq.csv" : NULL);
    g_bcd_correlator = bcd_correlator_create(g_log_csv ? "logs/wwv_bcd_corr.csv" : NULL);
    if (g_bcd_time_detector && g_bcd_freq_detector && g_bcd_correlator) {
        /* Wire time and freq detectors to correlator via wrapper callbacks */
        bcd_time_detector_set_callback(g_bcd_time_detector, on_bcd_time_event, g_bcd_correlator);
        bcd_freq_detector_set_callback(g_bcd_freq_detector, on_bcd_freq_event, g_bcd_correlator);
        /* NOTE: Sync source linked below after g_sync_detector is created */
    }

    /* Create marker correlator and wire orphaned marker callback */
    g_marker_correlator = marker_correlator_create(g_log_csv ? "wwv_markers_corr.csv" : NULL);
    if (!g_marker_correlator) {
        fprintf(stderr, "Failed to create marker correlator\n");
        return 1;
    }
    marker_correlator_set_callback(g_marker_correlator, on_orphaned_marker, NULL);

    /* Create sync detector and wire up callbacks */
    g_sync_detector = sync_detector_create(g_log_csv ? "wwv_sync.csv" : NULL);
    if (!g_sync_detector) {
        fprintf(stderr, "Failed to create sync detector\n");
        return 1;
    }
    tick_detector_set_marker_callback(g_tick_detector, on_tick_marker, NULL);
    tick_detector_set_callback(g_tick_detector, on_tick_event, NULL);
    marker_detector_set_callback(g_marker_detector, on_marker_event, NULL);

    /* Link BCD correlator to sync detector for window-based demodulation
     * Correlator will only emit symbols when sync is LOCKED */
    if (g_bcd_correlator && g_sync_detector) {
        bcd_correlator_set_sync_source(g_bcd_correlator, g_sync_detector);
    }

    /* Create tick correlator */
    g_tick_correlator = tick_correlator_create(g_log_csv ? "wwv_tick_corr.csv" : NULL);
    if (!g_tick_correlator) {
        fprintf(stderr, "Failed to create tick correlator\n");
        return 1;
    }

    /* Wire tick chain epoch callback */
    tick_correlator_set_epoch_callback(g_tick_correlator, on_tick_chain_epoch, NULL);

    /* g_channel_csv removed - use UDP telemetry (TELEM_CHANNEL) instead */

    /* g_subcarrier_csv removed - use UDP telemetry (TELEM_SUBCAR) instead */

    /* Create tone trackers for receiver characterization */
    g_tone_carrier = tone_tracker_create(0.0f, g_log_csv ? "wwv_carrier.csv" : NULL);
    g_tone_500 = tone_tracker_create(500.0f, g_log_csv ? "wwv_tone_500.csv" : NULL);
    g_tone_600 = tone_tracker_create(600.0f, g_log_csv ? "wwv_tone_600.csv" : NULL);

    /* Initialize UDP telemetry broadcast */
    telem_init(3005);
    /* All channels enabled by default in telem_init() */

    /* Initialize UDP command listener */
    g_cmd_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_cmd_sock != SOCKET_INVALID) {
        struct sockaddr_in cmd_addr;
        memset(&cmd_addr, 0, sizeof(cmd_addr));
        cmd_addr.sin_family = AF_INET;
        cmd_addr.sin_port = htons(CMD_PORT);
        cmd_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* localhost only */

        if (bind(g_cmd_sock, (struct sockaddr*)&cmd_addr, sizeof(cmd_addr)) < 0) {
            fprintf(stderr, "[CMD] Failed to bind UDP command socket on port %d\n", CMD_PORT);
            socket_close(g_cmd_sock);
            g_cmd_sock = SOCKET_INVALID;
        } else {
            /* Set non-blocking */
#ifdef _WIN32
            u_long mode = 1;
            ioctlsocket(g_cmd_sock, FIONBIO, &mode);
#else
            int flags = fcntl(g_cmd_sock, F_GETFL, 0);
            fcntl(g_cmd_sock, F_SETFL, flags | O_NONBLOCK);
#endif
            printf("[CMD] UDP command listener on localhost:%d (rate limit: %d/sec)\n",
                   CMD_PORT, CMD_RATE_LIMIT_PER_SEC);
        }
    } else {
        fprintf(stderr, "[CMD] Failed to create UDP command socket\n");
    }

    /* Output initial sync state to console and telemetry */
    telem_console("[SYNC] Startup state: %s (markers=%d, good_intervals=%d)\n",
                  sync_state_name(sync_detector_get_state(g_sync_detector)),
                  sync_detector_get_confirmed_count(g_sync_detector),
                  sync_detector_get_good_intervals(g_sync_detector));
    sync_detector_broadcast_state(g_sync_detector);

    /* Initialize flash system and register detectors */
    flash_init();
    flash_register(&(flash_source_t){
        .name = "tick",
        .get_flash_frames = (int (*)(void*))tick_detector_get_flash_frames,
        .decrement_flash = (void (*)(void*))tick_detector_decrement_flash,
        .ctx = g_tick_detector,
        .freq_hz = 1000,
        .band_half_width = 2,       /* 100 Hz bandwidth → ~4 pixels at current zoom */
        .band_r = 180, .band_g = 0, .band_b = 255,  /* Purple */
        .bar_index = 4,
        .bar_r = 180, .bar_g = 0, .bar_b = 255
    });
    flash_register(&(flash_source_t){
        .name = "marker",
        .get_flash_frames = (int (*)(void*))marker_detector_get_flash_frames,
        .decrement_flash = (void (*)(void*))marker_detector_decrement_flash,
        .ctx = g_marker_detector,
        .freq_hz = 1000,
        .band_half_width = 4,       /* 200 Hz bandwidth → ~8 pixels at current zoom */
        .band_r = 180, .band_g = 0, .band_b = 255,  /* Purple */
        .bar_index = 4,
        .bar_r = 180, .bar_g = 0, .bar_b = 255
    });

    uint64_t frame_num = 0;

    bool running = true;

    while (running) {
        /* Poll UDP command socket (non-blocking) */
        if (g_cmd_sock != SOCKET_INVALID) {
            char cmd_buf[CMD_MAX_LEN];
            int n = recvfrom(g_cmd_sock, cmd_buf, sizeof(cmd_buf) - 1, 0, NULL, NULL);
            if (n > 0) {
                process_modem_command(cmd_buf, n);
            }
        }

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

        if (g_test_pattern) {
            /* Generate synthetic 1000Hz tone: I = cos(2πft), Q = sin(2πft) */
            static int16_t test_samples[DISPLAY_OVERLAP * 2];  /* I/Q interleaved */
            int num_samples = DISPLAY_OVERLAP;
            for (int i = 0; i < num_samples; i++) {
                double phase = 2.0 * 3.14159265358979323846 * 1000.0 * g_test_sample_count / g_tcp_sample_rate;
                test_samples[i * 2]     = (int16_t)(cos(phase) * 16384);  /* I channel */
                test_samples[i * 2 + 1] = (int16_t)(sin(phase) * 16384);  /* Q channel */
                g_test_sample_count++;
            }
            samples_collected = num_samples;
            got_samples = true;

            /* Process test pattern samples through DSP (same as TCP path) */
            if (!g_detector_dsp_initialized) {
                lowpass_init(&g_detector_lowpass_i, DETECTOR_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                lowpass_init(&g_detector_lowpass_q, DETECTOR_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                sync_channel_init(&g_sync_channel_i);
                sync_channel_init(&g_sync_channel_q);
                data_channel_init(&g_data_channel_i);
                data_channel_init(&g_data_channel_q);
                g_detector_dsp_initialized = true;
            }
            if (!g_display_dsp_initialized) {
                lowpass_init(&g_display_lowpass_i, DISPLAY_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                lowpass_init(&g_display_lowpass_q, DISPLAY_FILTER_CUTOFF, (float)g_tcp_sample_rate);
                g_display_dsp_initialized = true;
            }

            for (int s = 0; s < num_samples; s++) {
                float i_raw = (float)test_samples[s * 2] / 32768.0f;
                float q_raw = (float)test_samples[s * 2 + 1] / 32768.0f;

                /* DETECTOR PATH */
                float det_i = lowpass_process(&g_detector_lowpass_i, i_raw);
                float det_q = lowpass_process(&g_detector_lowpass_q, q_raw);

                g_detector_decim_counter++;
                if (g_detector_decim_counter >= g_detector_decimation) {
                    g_detector_decim_counter = 0;
                    float norm_factor = normalize(&g_normalizer, det_i, det_q);
                    det_i *= norm_factor;
                    det_q *= norm_factor;

                    float sync_i = sync_channel_process(&g_sync_channel_i, det_i);
                    float sync_q = sync_channel_process(&g_sync_channel_q, det_q);
                    float data_i = data_channel_process(&g_data_channel_i, det_i);
                    float data_q = data_channel_process(&g_data_channel_q, det_q);

                    tick_detector_process_sample(g_tick_detector, sync_i, sync_q);
                    marker_detector_process_sample(g_marker_detector, sync_i, sync_q);
                    if (g_bcd_time_detector) bcd_time_detector_process_sample(g_bcd_time_detector, data_i, data_q);
                    if (g_bcd_freq_detector) bcd_freq_detector_process_sample(g_bcd_freq_detector, data_i, data_q);

                    g_periodic_check_counter++;
                    if (g_periodic_check_counter >= PERIODIC_CHECK_INTERVAL_SAMPLES) {
                        g_periodic_check_counter = 0;
                        if (g_sync_detector) {
                            float timestamp_ms = (float)(frame_num * DISPLAY_EFFECTIVE_MS);
                            sync_detector_periodic_check(g_sync_detector, timestamp_ms);
                        }
                    }
                }

                /* DISPLAY PATH */
                float disp_i = lowpass_process(&g_display_lowpass_i, i_raw);
                float disp_q = lowpass_process(&g_display_lowpass_q, q_raw);

                g_display_decim_counter++;
                if (g_display_decim_counter >= g_display_decimation) {
                    g_display_decim_counter = 0;
                    g_display_buffer[g_display_buffer_idx].i = disp_i;
                    g_display_buffer[g_display_buffer_idx].q = disp_q;
                    g_display_buffer_idx = (g_display_buffer_idx + 1) % DISPLAY_FFT_SIZE;
                    g_display_new_samples++;
                }
            }
        } else if (g_tcp_mode) {
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
                    sync_channel_init(&g_sync_channel_i);
                    sync_channel_init(&g_sync_channel_q);
                    data_channel_init(&g_data_channel_i);
                    data_channel_init(&g_data_channel_q);
                    g_detector_dsp_initialized = true;
                    printf("Detector DSP: lowpass @ %.0f Hz\n", DETECTOR_FILTER_CUTOFF);
                    printf("Channel filters: Sync 800-1400 Hz, Data 0-150 Hz\n");
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
                        /* Normalize S16 to [-1, 1] range. Without this, raw int16 values
                         * (-32768 to +32767) become floats of the same magnitude, causing
                         * energy values ~10^9 instead of ~1. Adaptive thresholds self-adjust
                         * so detection still works, but debugging is confusing and log plots
                         * are nonsensical. Added v1.0.1+19, 2025-12-17. */
                        i_raw = (float)samples[s * 2] / 32768.0f;
                        q_raw = (float)samples[s * 2 + 1] / 32768.0f;
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
                     * Parallel filter architecture - WWV Tick/BCD Separation
                     *========================================================*/
                    float det_i = lowpass_process(&g_detector_lowpass_i, i_raw);
                    float det_q = lowpass_process(&g_detector_lowpass_q, q_raw);

                    g_detector_decim_counter++;
                    if (g_detector_decim_counter >= g_detector_decimation) {
                        g_detector_decim_counter = 0;

                        /* Signal normalization (slow AGC) */
                        float norm_factor = normalize(&g_normalizer, det_i, det_q);
                        det_i *= norm_factor;
                        det_q *= norm_factor;

                        /* SYNC CHANNEL: 800-1400 Hz bandpass for ticks/markers */
                        float sync_i = sync_channel_process(&g_sync_channel_i, det_i);
                        float sync_q = sync_channel_process(&g_sync_channel_q, det_q);

                        /* DATA CHANNEL: 0-150 Hz lowpass for BCD subcarrier */
                        float data_i = data_channel_process(&g_data_channel_i, det_i);
                        float data_q = data_channel_process(&g_data_channel_q, det_q);

                        /* Feed sync channel to tick/marker detectors (1000 Hz tones) */
                        tick_detector_process_sample(g_tick_detector, sync_i, sync_q);
                        marker_detector_process_sample(g_marker_detector, sync_i, sync_q);

                        /* Feed data channel to BCD detectors (100 Hz subcarrier) */
                        if (g_bcd_time_detector) bcd_time_detector_process_sample(g_bcd_time_detector, data_i, data_q);
                        if (g_bcd_freq_detector) bcd_freq_detector_process_sample(g_bcd_freq_detector, data_i, data_q);

                        /* Periodic signal check for sync detector */
                        g_periodic_check_counter++;
                        if (g_periodic_check_counter >= PERIODIC_CHECK_INTERVAL_SAMPLES) {
                            g_periodic_check_counter = 0;
                            if (g_sync_detector) {
                                float timestamp_ms = (float)(frame_num * DISPLAY_EFFECTIVE_MS);
                                sync_detector_periodic_check(g_sync_detector, timestamp_ms);
                            }
                        }
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

                        /* Feed tone trackers (same 12 kHz samples) */
                        tone_tracker_process_sample(g_tone_carrier, disp_i, disp_q);
                        tone_tracker_process_sample(g_tone_500, disp_i, disp_q);
                        tone_tracker_process_sample(g_tone_600, disp_i, disp_q);
                        bcd_envelope_process_sample(g_bcd_envelope, disp_i, disp_q);  /* DEPRECATED */

                        /* DEPRECATED: Feed BCD decoder with envelope data
                         * Use bcd_correlator callback instead */
                        if (g_bcd_decoder && g_bcd_envelope) {
                            float timestamp_ms = (float)(frame_num * DISPLAY_EFFECTIVE_MS) +
                                                 (float)g_display_new_samples * (1000.0f / DISPLAY_SAMPLE_RATE);
                            bcd_decoder_process_sample(g_bcd_decoder,
                                                       timestamp_ms,
                                                       bcd_envelope_get_envelope(g_bcd_envelope),
                                                       bcd_envelope_get_snr_db(g_bcd_envelope),
                                                       (bcd_status_t)bcd_envelope_get_status(g_bcd_envelope));
                        }

                        /* Note: marker_detector now tracks 500/600 Hz in its own FFT path
                         * (same units, no scaling mismatch). No cross-path integration needed. */

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

        /* Feed slow marker detector with display FFT output */
        if (g_slow_marker) {
            float timestamp_ms = frame_num * DISPLAY_EFFECTIVE_MS;
            slow_marker_detector_process_fft(g_slow_marker, fft_out, timestamp_ms);
        }

        /* Calculate magnitudes with FFT shift (DC in center) */
        float bin_hz = DISPLAY_HZ_PER_BIN;
        for (int i = 0; i < g_waterfall_width; i++) {
            /* Map pixel to frequency: left = -ZOOM_MAX_HZ, center = 0 (DC), right = +ZOOM_MAX_HZ */
            float freq = ((float)i / g_waterfall_width - 0.5f) * 2.0f * ZOOM_MAX_HZ;

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
        for (int i = 0; i < g_waterfall_width; i++) {
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
        memmove(pixels + g_window_width * 3,
                pixels,
                g_window_width * (g_window_height - 1) * 3);

        /* Draw new row at top */
        for (int x = 0; x < g_waterfall_width; x++) {
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

            /* Use BCD envelope tracker SNR for 100 Hz bucket display */
            if (f == 0 && g_bcd_envelope) {
                float snr = bcd_envelope_get_snr_db(g_bcd_envelope);
                /* Convert SNR to energy-like value for display scaling */
                g_bucket_energy[0] = powf(10.0f, snr / 20.0f) * 0.001f;
            }
        }

        /* Log channel conditions every ~1 second (12 frames at 85ms effective) */
        if ((frame_num % 12) == 0) {
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

            float timestamp_ms = frame_num * DISPLAY_EFFECTIVE_MS;
            float carrier_db = 20.0f * log10f(magnitudes[g_waterfall_width/2] + 1e-10f);
            float sub500_db = 20.0f * log10f(g_bucket_energy[2] + 1e-10f);  /* 500 Hz */
            float sub600_db = 20.0f * log10f(g_bucket_energy[3] + 1e-10f);  /* 600 Hz */
            float tone1000_db = 20.0f * log10f(g_bucket_energy[4] + 1e-10f); /* 1000 Hz */
            float noise_db = g_floor_db;
            float snr_db = tone1000_db - noise_db;
            const char *quality = (snr_db > 15) ? "GOOD" : (snr_db > 8) ? "FAIR" : (snr_db > 3) ? "POOR" : "NONE";

            /* UDP telemetry broadcast */
            telem_sendf(TELEM_CHANNEL, "%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%s",
                        time_str, timestamp_ms, carrier_db, snr_db, sub500_db, sub600_db, tone1000_db, noise_db, quality);

            /* Also send carrier tracker data */
            if (tone_tracker_is_valid(g_tone_carrier)) {
                telem_sendf(TELEM_CARRIER, "%s,%.1f,%.3f,%.3f,%.2f,%.1f",
                            time_str, timestamp_ms,
                            tone_tracker_get_measured_hz(g_tone_carrier),
                            tone_tracker_get_offset_hz(g_tone_carrier),
                            tone_tracker_get_offset_ppm(g_tone_carrier),
                            tone_tracker_get_snr_db(g_tone_carrier));
            }

            /* Send tone 500 tracker data */
            if (tone_tracker_is_valid(g_tone_500)) {
                telem_sendf(TELEM_TONE500, "%s,%.1f,%.3f,%.3f,%.2f,%.1f",
                            time_str, timestamp_ms,
                            tone_tracker_get_measured_hz(g_tone_500),
                            tone_tracker_get_offset_hz(g_tone_500),
                            tone_tracker_get_offset_ppm(g_tone_500),
                            tone_tracker_get_snr_db(g_tone_500));
            }

            /* Send tone 600 tracker data */
            if (tone_tracker_is_valid(g_tone_600)) {
                telem_sendf(TELEM_TONE600, "%s,%.1f,%.3f,%.3f,%.2f,%.1f",
                            time_str, timestamp_ms,
                            tone_tracker_get_measured_hz(g_tone_600),
                            tone_tracker_get_offset_hz(g_tone_600),
                            tone_tracker_get_offset_ppm(g_tone_600),
                            tone_tracker_get_snr_db(g_tone_600));
            }

            /* DEPRECATED: Send BCD 100 Hz envelope telemetry
             * Use bcd_correlator BCDS telemetry instead */
            if (g_bcd_envelope) {
                float snr = bcd_envelope_get_snr_db(g_bcd_envelope);
                float envelope = bcd_envelope_get_envelope(g_bcd_envelope);
                float noise = bcd_envelope_get_noise_floor_db(g_bcd_envelope);
                bcd_envelope_status_t status = bcd_envelope_get_status(g_bcd_envelope);

                const char *status_str;
                switch (status) {
                    case BCD_ENV_ABSENT:  status_str = "ABSENT";  break;
                    case BCD_ENV_WEAK:    status_str = "WEAK";    break;
                    case BCD_ENV_PRESENT: status_str = "PRESENT"; break;
                    case BCD_ENV_STRONG:  status_str = "STRONG";  break;
                    default: status_str = "UNKNOWN";
                }

                telem_sendf(TELEM_BCD_ENV, "%s,%.1f,%.6f,%.2f,%.2f,%s",
                            time_str, timestamp_ms,
                            envelope, snr, noise, status_str);
            }

            /* Send BCD decoder status telemetry */
            if (g_bcd_decoder) {
                uint32_t symbols = bcd_decoder_get_symbol_count(g_bcd_decoder);

                /* Modem only reports symbol count - sync/decode is controller's job */
                telem_sendf(TELEM_BCDS, "STATUS,%s,%.1f,MODEM,-1,0,0,%u",
                            time_str, timestamp_ms, symbols);
            }
        }

        /* Log subcarrier conditions every ~1 second (12 frames) */
        if ((frame_num % 12) == 0) {
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

            /* UDP telemetry broadcast */
            telem_sendf(TELEM_SUBCAR, "%s,%.1f,%d,%s,%.1f,%.1f,%.1f,%s,%s",
                        time_str, frame_num * DISPLAY_EFFECTIVE_MS, minute,
                        expected, sub500_db, sub600_db, delta_db, detected, match);
        }

        /* Draw flash bands on waterfall for all registered detectors */
        flash_draw_waterfall_bands(pixels, 0, g_waterfall_width, g_window_width, ZOOM_MAX_HZ);

        /* === RIGHT PANEL: Bucket energy bars === */
        int bar_width = g_bucket_width / NUM_TICK_FREQS;
        int bar_gap = 2;

        /* Clear right panel */
        for (int y = 0; y < g_window_height; y++) {
            for (int x = g_waterfall_width; x < g_window_width; x++) {
                int idx = (y * g_window_width + x) * 3;
                pixels[idx + 0] = 20;  /* Dark gray background */
                pixels[idx + 1] = 20;
                pixels[idx + 2] = 20;
            }
        }

        /* Draw each bucket bar */
        for (int f = 0; f < NUM_TICK_FREQS; f++) {
            int bar_x = g_waterfall_width + f * bar_width + bar_gap;
            int bar_w = bar_width - bar_gap * 2;

            /* Use waterfall's tracked dB scale */
            float db = 20.0f * log10f(g_bucket_energy[f] + 1e-10f);
            float norm = (db - g_floor_db) / (g_peak_db - g_floor_db);
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;

            int bar_height = (int)(norm * g_window_height);

            uint8_t r, g, b;

            /* Check if any detector wants to flash this bar */
            if (flash_get_bar_override(f, &r, &g, &b)) {
                bar_height = g_window_height;
            } else {
                /* Color based on energy level */
                magnitude_to_rgb(g_bucket_energy[f], -20.0f, -80.0f, &r, &g, &b);
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

            /* For 1000 Hz bar: show adaptive threshold (cyan) and noise floor (green) */
            if (f == 4) {
                /* Cyan = threshold */
                float thresh_db = 20.0f * log10f(tick_detector_get_threshold(g_tick_detector) + 1e-10f);
                float thresh_norm = (thresh_db - (-80.0f)) / 60.0f;
                if (thresh_norm < 0.0f) thresh_norm = 0.0f;
                if (thresh_norm > 1.0f) thresh_norm = 1.0f;
                int thresh_y = g_window_height - (int)(thresh_norm * g_window_height);
                for (int dy = -1; dy <= 1; dy++) {
                    int y = thresh_y + dy;
                    if (y >= 0 && y < g_window_height) {
                        for (int x = bar_x; x < bar_x + bar_w && x < g_window_width; x++) {
                            int idx = (y * g_window_width + x) * 3;
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
                int noise_y = g_window_height - (int)(noise_norm * g_window_height);
                for (int dy = -1; dy <= 1; dy++) {
                    int y = noise_y + dy;
                    if (y >= 0 && y < g_window_height) {
                        for (int x = bar_x; x < bar_x + bar_w && x < g_window_width; x++) {
                            int idx = (y * g_window_width + x) * 3;
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

        SDL_UpdateTexture(texture, NULL, pixels, g_window_width * 3);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        /* Flush console telemetry buffer periodically */
        if ((frame_num % 12) == 0) {
            telem_console_flush();
        }

        frame_num++;
    }

    printf("\n");
    tick_detector_print_stats(g_tick_detector);
    marker_detector_print_stats(g_marker_detector);
    tick_correlator_print_stats(g_tick_correlator);
    tick_detector_destroy(g_tick_detector);
    marker_detector_destroy(g_marker_detector);
    bcd_envelope_destroy(g_bcd_envelope);
    bcd_decoder_destroy(g_bcd_decoder);
    /* BCD dual-path detectors (robust symbol demodulator) */
    if (g_bcd_time_detector) bcd_time_detector_destroy(g_bcd_time_detector);
    if (g_bcd_freq_detector) bcd_freq_detector_destroy(g_bcd_freq_detector);
    if (g_bcd_correlator) bcd_correlator_destroy(g_bcd_correlator);
    slow_marker_detector_destroy(g_slow_marker);
    marker_correlator_destroy(g_marker_correlator);
    sync_detector_destroy(g_sync_detector);
    tick_correlator_destroy(g_tick_correlator);
    tone_tracker_destroy(g_tone_carrier);
    tone_tracker_destroy(g_tone_500);
    tone_tracker_destroy(g_tone_600);
    telem_cleanup();

    /* Cleanup UDP command socket */
    if (g_cmd_sock != SOCKET_INVALID) {
        socket_close(g_cmd_sock);
        g_cmd_sock = SOCKET_INVALID;
    }

    if (g_tcp_mode && !g_test_pattern) {
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
