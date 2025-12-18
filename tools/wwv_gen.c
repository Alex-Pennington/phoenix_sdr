/**
 * @file wwv_gen.c
 * @brief WWV test signal generator - standalone tool
 *
 * Generates WWV/WWVH time signals as IQ recording files or TCP streams.
 * Outputs fixed 2-minute reference signals with exact timing per NIST spec.
 * Supports TCP streaming compatible with Phoenix SDR I/Q protocol (port 4536).
 */

#include "wwv_signal.h"
#include "../include/iq_recorder.h"
#include "../include/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define SAMPLE_RATE 2000000
#define SAMPLES_PER_MINUTE 120000000ULL
#define BUFFER_SIZE 65536  // 64K samples = 32K I/Q pairs
#define IQ_FRAME_SAMPLES 8192  // Match sdr_server frame size
#define IQ_DEFAULT_PORT 4536

/* I/Q Streaming Protocol - Match sdr_server.c */
#define IQ_MAGIC_HEADER 0x50485849  /* "PHXI" */
#define IQ_MAGIC_DATA   0x49514451  /* "IQDQ" */
#define IQ_FORMAT_S16 1

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           /* 0x50485849 = "PHXI" */
    uint32_t version;         /* Protocol version (1) */
    uint32_t sample_rate;     /* Current sample rate in Hz */
    uint32_t sample_format;   /* Format code */
    uint32_t center_freq_lo;  /* Center frequency low 32 bits */
    uint32_t center_freq_hi;  /* Center frequency high 32 bits */
    uint32_t gain_reduction;  /* IF gain reduction in dB */
    uint32_t lna_state;       /* LNA state (0-8) */
} iq_stream_header_t;

typedef struct {
    uint32_t magic;           /* 0x49514451 = "IQDQ" */
    uint32_t sequence;        /* Frame sequence number */
    uint32_t num_samples;     /* Number of I/Q pairs in this frame */
    uint32_t flags;           /* Bit flags */
} iq_data_frame_t;
#pragma pack(pop)

static void print_usage(const char *prog) {
    printf("WWV Test Signal Generator v%s\n", PHOENIX_VERSION_FULL);
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -t HH:MM        Start time (default: 00:00)\n");
    printf("  -d DDD          Day of year (default: 001)\n");
    printf("  -y YY           Year, last 2 digits (default: 25)\n");
    printf("  -s wwv|wwvh     Station type (default: wwv)\n");
    printf("  -o FILE         Output .iqr file (default: wwv_test.iqr)\n");
    printf("  -p PORT         Stream via TCP (default: %d)\n", IQ_DEFAULT_PORT);
    printf("  -c              Continuous streaming (requires -p)\n");
    printf("  -h              Show this help\n");
    printf("\nFile mode: Generates fixed 2-minute signal\n");
    printf("TCP mode:  Streams continuously (default) or 2-minute loop\n");
}

static bool parse_time(const char *str, int *hour, int *minute) {
    if (sscanf(str, "%d:%d", hour, minute) != 2) {
        return false;
    }
    if (*hour < 0 || *hour > 23 || *minute < 0 || *minute > 59) {
        return false;
    }
    return true;
}

/* TCP streaming helper functions */
static SOCKET tcp_listen(int port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "Error: WSAStartup failed\n");
        return INVALID_SOCKET;
    }

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "Error: socket() failed\n");
        WSACleanup();
        return INVALID_SOCKET;
    }

    // Allow reuse
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "Error: bind() failed on port %d\n", port);
        closesocket(listen_sock);
        WSACleanup();
        return INVALID_SOCKET;
    }

    if (listen(listen_sock, 1) != 0) {
        fprintf(stderr, "Error: listen() failed\n");
        closesocket(listen_sock);
        WSACleanup();
        return INVALID_SOCKET;
    }

    return listen_sock;
}

static bool tcp_send_all(SOCKET sock, const void *data, int len) {
    const char *ptr = (const char *)data;
    int sent = 0;
    while (sent < len) {
        int ret = send(sock, ptr + sent, len - sent, 0);
        if (ret <= 0) return false;
        sent += ret;
    }
    return true;
}

static bool tcp_send_header(SOCKET sock, uint32_t sample_rate, uint64_t center_freq) {
    iq_stream_header_t hdr = {0};
    hdr.magic = IQ_MAGIC_HEADER;
    hdr.version = 1;
    hdr.sample_rate = sample_rate;
    hdr.sample_format = IQ_FORMAT_S16;
    hdr.center_freq_lo = (uint32_t)(center_freq & 0xFFFFFFFF);
    hdr.center_freq_hi = (uint32_t)(center_freq >> 32);
    hdr.gain_reduction = 59;  // Match typical SDR setting
    hdr.lna_state = 0;
    return tcp_send_all(sock, &hdr, sizeof(hdr));
}

static bool tcp_send_frame(SOCKET sock, const short *i_buf, const short *q_buf,
                           int num_samples, uint32_t *sequence) {
    iq_data_frame_t frame_hdr = {0};
    frame_hdr.magic = IQ_MAGIC_DATA;
    frame_hdr.sequence = (*sequence)++;
    frame_hdr.num_samples = num_samples;
    frame_hdr.flags = 0;

    if (!tcp_send_all(sock, &frame_hdr, sizeof(frame_hdr))) return false;

    // Interleave I/Q samples
    short *interleaved = (short *)malloc(num_samples * 2 * sizeof(short));
    if (!interleaved) return false;

    for (int i = 0; i < num_samples; i++) {
        interleaved[i * 2] = i_buf[i];
        interleaved[i * 2 + 1] = q_buf[i];
    }

    bool success = tcp_send_all(sock, interleaved, num_samples * 2 * sizeof(short));
    free(interleaved);
    return success;
}

int main(int argc, char *argv[]) {
    // Default parameters
    int start_hour = 0;
    int start_minute = 0;
    int day_of_year = 1;
    int year = 25;
    wwv_station_t station = WWV_STATION_WWV;
    const char *output_file = "wwv_test.iqr";
    int tcp_port = 0;  // 0 = file mode, non-zero = TCP mode
    bool continuous = false;

    // Parse command line
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            if (!parse_time(argv[++i], &start_hour, &start_minute)) {
                fprintf(stderr, "Error: Invalid time format '%s'\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            day_of_year = atoi(argv[++i]);
            if (day_of_year < 1 || day_of_year > 366) {
                fprintf(stderr, "Error: Day of year must be 1-366\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-y") == 0 && i + 1 < argc) {
            year = atoi(argv[++i]);
            if (year < 0 || year > 99) {
                fprintf(stderr, "Error: Year must be 0-99\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "wwv") == 0) {
                station = WWV_STATION_WWV;
            } else if (strcmp(argv[i], "wwvh") == 0) {
                station = WWV_STATION_WWVH;
            } else {
                fprintf(stderr, "Error: Station must be 'wwv' or 'wwvh'\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            tcp_port = atoi(argv[++i]);
            if (tcp_port < 1 || tcp_port > 65535) {
                fprintf(stderr, "Error: Port must be 1-65535\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-c") == 0) {
            continuous = true;
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    // Validate options
    if (continuous && tcp_port == 0) {
        fprintf(stderr, "Error: -c requires -p (TCP port)\n");
        return 1;
    }

    bool use_tcp = (tcp_port > 0);
    if (tcp_port == 0) tcp_port = IQ_DEFAULT_PORT;

    // Print configuration
    printf("WWV Test Signal Generator\n");
    printf("  Station:    %s\n", station == WWV_STATION_WWV ? "WWV" : "WWVH");
    printf("  Start time: %02d:%02d (day %03d, year 20%02d)\n",
           start_hour, start_minute, day_of_year, year);
    if (use_tcp) {
        printf("  Mode:       TCP streaming on port %d\n", tcp_port);
        printf("  Duration:   %s\n", continuous ? "Continuous (Ctrl+C to stop)" : "2-minute loop");
    } else {
        printf("  Mode:       File output\n");
        printf("  Output:     %s\n", output_file);
        printf("  Duration:   2 minutes (120M samples)\n");
    }

    // Create signal generator
    wwv_signal_t *sig = wwv_signal_create(start_minute, start_hour, day_of_year, year, station);
    if (!sig) {
        fprintf(stderr, "Error: Failed to create signal generator\n");
        return 1;
    }

    int ret = 0;

    if (use_tcp) {
        // TCP STREAMING MODE
        printf("\nStarting TCP server on port %d...\n", tcp_port);
        SOCKET listen_sock = tcp_listen(tcp_port);
        if (listen_sock == INVALID_SOCKET) {
            wwv_signal_destroy(sig);
            return 1;
        }

        printf("Waiting for client connection...\n");
        SOCKET client_sock = accept(listen_sock, NULL, NULL);
        if (client_sock == INVALID_SOCKET) {
            fprintf(stderr, "Error: accept() failed\n");
            closesocket(listen_sock);
            WSACleanup();
            wwv_signal_destroy(sig);
            return 1;
        }

        printf("Client connected! Streaming samples...\n");

        // Send protocol header
        if (!tcp_send_header(client_sock, SAMPLE_RATE, 5000000ULL)) {
            fprintf(stderr, "Error: Failed to send header\n");
            closesocket(client_sock);
            closesocket(listen_sock);
            WSACleanup();
            wwv_signal_destroy(sig);
            return 1;
        }

        // Allocate frame buffers
        short *i_buffer = (short *)malloc(IQ_FRAME_SAMPLES * sizeof(short));
        short *q_buffer = (short *)malloc(IQ_FRAME_SAMPLES * sizeof(short));
        if (!i_buffer || !q_buffer) {
            fprintf(stderr, "Error: Failed to allocate buffers\n");
            free(i_buffer);
            free(q_buffer);
            closesocket(client_sock);
            closesocket(listen_sock);
            WSACleanup();
            wwv_signal_destroy(sig);
            return 1;
        }

        uint32_t sequence = 0;
        uint64_t frames_sent = 0;
        uint64_t total_samples = continuous ? UINT64_MAX : (2 * SAMPLES_PER_MINUTE);
        uint64_t samples_sent = 0;

        // Stream frames
        while (samples_sent < total_samples) {
            // Generate frame
            for (int i = 0; i < IQ_FRAME_SAMPLES; i++) {
                int16_t i_sample, q_sample;
                wwv_signal_get_sample_int16(sig, &i_sample, &q_sample);
                i_buffer[i] = i_sample;
                q_buffer[i] = q_sample;
            }

            // Send frame
            if (!tcp_send_frame(client_sock, i_buffer, q_buffer, IQ_FRAME_SAMPLES, &sequence)) {
                printf("\nClient disconnected\n");
                break;
            }

            samples_sent += IQ_FRAME_SAMPLES;
            frames_sent++;

            // Progress update every second
            if ((frames_sent % (SAMPLE_RATE / IQ_FRAME_SAMPLES)) == 0) {
                int seconds = (int)(samples_sent / SAMPLE_RATE);
                int minutes = seconds / 60;
                seconds %= 60;
                printf("\r  Streaming: %02d:%02d (%llu frames)...",
                       minutes, seconds, (unsigned long long)frames_sent);
                fflush(stdout);
            }

            // Loop back to start after 2 minutes if not continuous
            if (!continuous && samples_sent >= 2 * SAMPLES_PER_MINUTE) {
                printf("\n  Looping back to start...\n");
                samples_sent = 0;
                // Recreate signal generator to reset to start time
                wwv_signal_destroy(sig);
                sig = wwv_signal_create(start_minute, start_hour, day_of_year, year, station);
            }
        }

        printf("\nStreaming stopped\n");
        free(i_buffer);
        free(q_buffer);
        closesocket(client_sock);
        closesocket(listen_sock);
        WSACleanup();

    } else {
        // FILE OUTPUT MODE
        printf("\nGenerating file...\n");

        iqr_recorder_t *rec;
        if (iqr_create(&rec, BUFFER_SIZE) != 0) {
            fprintf(stderr, "Error: Failed to create IQ recorder\n");
            wwv_signal_destroy(sig);
            return 1;
        }

        if (iqr_start(rec, output_file, SAMPLE_RATE, 5000000.0, 2000, 59, 0) != 0) {
            fprintf(stderr, "Error: Failed to start recording\n");
            iqr_destroy(rec);
            wwv_signal_destroy(sig);
            return 1;
        }

        short *i_buffer = (short *)malloc(BUFFER_SIZE * sizeof(short));
        short *q_buffer = (short *)malloc(BUFFER_SIZE * sizeof(short));
        if (!i_buffer || !q_buffer) {
            fprintf(stderr, "Error: Failed to allocate buffers\n");
            free(i_buffer);
            free(q_buffer);
            iqr_stop(rec);
            iqr_destroy(rec);
            wwv_signal_destroy(sig);
            return 1;
        }

        uint64_t total_samples = 2 * SAMPLES_PER_MINUTE;
        uint64_t samples_written = 0;
        int last_progress = -1;

        while (samples_written < total_samples) {
            int samples_to_write = BUFFER_SIZE;
            if (samples_written + samples_to_write > total_samples) {
                samples_to_write = (int)(total_samples - samples_written);
            }

            for (int i = 0; i < samples_to_write; i++) {
                int16_t i_sample, q_sample;
                wwv_signal_get_sample_int16(sig, &i_sample, &q_sample);
                i_buffer[i] = i_sample;
                q_buffer[i] = q_sample;
            }

            if (iqr_write(rec, i_buffer, q_buffer, samples_to_write) != 0) {
                fprintf(stderr, "Error: Failed to write samples\n");
                ret = 1;
                break;
            }

            samples_written += samples_to_write;

            int progress = (int)((samples_written * 100) / total_samples);
            if (progress / 10 != last_progress / 10) {
                int seconds = (int)(samples_written / SAMPLE_RATE);
                printf("\r  Progress: %d%% (%d seconds)...", progress, seconds);
                fflush(stdout);
                last_progress = progress;
            }
        }

        printf("\r  Progress: 100%% (120 seconds)... Done!\n");
        printf("\nGenerated %llu samples (%.1f MB)\n",
               (unsigned long long)samples_written,
               (samples_written * 4.0) / (1024 * 1024));

        free(i_buffer);
        free(q_buffer);
        iqr_stop(rec);
        iqr_destroy(rec);
    }

    wwv_signal_destroy(sig);
    return ret;
}
