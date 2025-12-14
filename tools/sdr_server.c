/**
 * @file sdr_server.c
 * @brief Phoenix SDR TCP Control Server
 *
 * TCP server for remote control of SDRplay RSP2 Pro.
 * Implements protocol defined in docs/SDR_TCP_CONTROL_INTERFACE.md
 *
 * Usage: sdr_server.exe [-p port] [-T addr] [-n (no hardware)]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#include "tcp_server.h"
#include "phoenix_sdr.h"
#include "version.h"
#include <stdarg.h>

/*============================================================================
 * Globals
 *============================================================================*/

static volatile bool g_running = true;
static SOCKET g_listen_socket = INVALID_SOCKET;
static SOCKET g_client_socket = INVALID_SOCKET;
static tcp_sdr_state_t g_sdr_state;

/*============================================================================
 * Notification Functions (thread-safe async notifications to client)
 *============================================================================*/

void tcp_notify_init(tcp_sdr_state_t *state) {
#ifdef _WIN32
    InitializeCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_init(&state->notify_mutex, NULL);
#endif
    state->client_socket = TCP_INVALID_SOCKET;
    state->notify_enabled = false;
}

void tcp_notify_cleanup(tcp_sdr_state_t *state) {
#ifdef _WIN32
    DeleteCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_destroy(&state->notify_mutex);
#endif
}

void tcp_notify_set_client(tcp_sdr_state_t *state, tcp_socket_t client) {
#ifdef _WIN32
    EnterCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_lock(&state->notify_mutex);
#endif
    state->client_socket = client;
    state->notify_enabled = true;
#ifdef _WIN32
    LeaveCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_unlock(&state->notify_mutex);
#endif
}

void tcp_notify_clear_client(tcp_sdr_state_t *state) {
#ifdef _WIN32
    EnterCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_lock(&state->notify_mutex);
#endif
    state->client_socket = TCP_INVALID_SOCKET;
    state->notify_enabled = false;
#ifdef _WIN32
    LeaveCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_unlock(&state->notify_mutex);
#endif
}

int tcp_send_notification(tcp_sdr_state_t *state, const char *format, ...) {
    if (!state) return -1;
    
    char buf[TCP_MAX_LINE_LENGTH];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf, sizeof(buf) - 1, format, args);
    va_end(args);
    
    if (len <= 0) return -1;
    
    /* Ensure newline termination */
    if (buf[len-1] != '\n') {
        buf[len++] = '\n';
        buf[len] = '\0';
    }
    
    int result = -1;
    
#ifdef _WIN32
    EnterCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_lock(&state->notify_mutex);
#endif
    
    if (state->notify_enabled && state->client_socket != TCP_INVALID_SOCKET) {
        int sent = send(state->client_socket, buf, len, 0);
        result = (sent == len) ? 0 : -1;
    }
    
#ifdef _WIN32
    LeaveCriticalSection(&state->notify_mutex);
#else
    pthread_mutex_unlock(&state->notify_mutex);
#endif
    
    return result;
}

/*============================================================================
 * SDR Callbacks
 *============================================================================*/

static void on_samples(const int16_t *xi, const int16_t *xq,
                      uint32_t count, bool reset, void *user_ctx) {
    (void)xi; (void)xq; (void)count; (void)reset; (void)user_ctx;
    /* Samples are received here - for now just count/discard */
    /* In the future, could stream over TCP or process in some way */
}

static void on_gain_change(double gain_db, int lna_db, void *user_ctx) {
    tcp_sdr_state_t *state = (tcp_sdr_state_t*)user_ctx;
    if (state) {
        int old_gain = state->gain_reduction;
        int old_lna = state->lna_state;
        
        state->gain_reduction = (int)gain_db;
        state->lna_state = lna_db;
        
        /* Only notify if values actually changed */
        if (old_gain != (int)gain_db || old_lna != lna_db) {
            printf("[SDR] Gain changed: GR=%.0f dB, LNA=%d\n", gain_db, lna_db);
            tcp_send_notification(state, "! GAIN_CHANGE GAIN=%d LNA=%d",
                                  (int)gain_db, lna_db);
        }
    }
}

static void on_overload(bool overloaded, void *user_ctx) {
    tcp_sdr_state_t *state = (tcp_sdr_state_t*)user_ctx;
    if (state) {
        bool was_overloaded = state->overload;
        state->overload = overloaded;
        
        /* Only notify if state changed */
        if (was_overloaded != overloaded) {
            printf("[SDR] %s\n", overloaded ? "OVERLOAD DETECTED" : "Overload cleared");
            tcp_send_notification(state, "! OVERLOAD %s",
                                  overloaded ? "DETECTED" : "CLEARED");
        }
    }
}

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    printf("\nShutting down...\n");
    g_running = false;

    /* Send DISCONNECT notification to client before closing */
    if (g_client_socket != INVALID_SOCKET) {
        const char *disconnect_msg = "! DISCONNECT server shutdown\n";
        send(g_client_socket, disconnect_msg, (int)strlen(disconnect_msg), 0);
    }

    /* Stop SDR streaming if active */
    if (g_sdr_state.streaming && g_sdr_state.sdr_ctx) {
        printf("Stopping SDR streaming...\n");
        psdr_stop(g_sdr_state.sdr_ctx);
        g_sdr_state.streaming = false;
    }

    /* Close sockets to unblock accept/recv */
    if (g_client_socket != INVALID_SOCKET) {
        closesocket(g_client_socket);
        g_client_socket = INVALID_SOCKET;
    }
    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
    }
}

/*============================================================================
 * Socket Helpers
 *============================================================================*/

static int socket_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    return 0;
#endif
}

static void socket_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

static int send_line(SOCKET sock, const char *line) {
    size_t len = strlen(line);
    int sent = send(sock, line, (int)len, 0);
    return (sent == (int)len) ? 0 : -1;
}

static int recv_line(SOCKET sock, char *buf, int buf_size) {
    int total = 0;
    while (total < buf_size - 1) {
        char c;
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) {
            /* Connection closed or error */
            return -1;
        }
        if (c == '\n') {
            buf[total] = '\0';
            return total;
        }
        if (c != '\r') {
            buf[total++] = c;
        }
    }
    buf[total] = '\0';
    return total;
}

/*============================================================================
 * Client Handler
 *============================================================================*/

static void handle_client(SOCKET client, tcp_sdr_state_t *state) {
    char line[TCP_MAX_LINE_LENGTH];
    char response[TCP_MAX_LINE_LENGTH + 16];
    tcp_command_t cmd;
    tcp_response_t resp;

    printf("Client connected\n");
    
    /* Enable async notifications for this client */
    tcp_notify_set_client(state, client);

    while (g_running) {
        /* Receive command */
        int len = recv_line(client, line, sizeof(line));
        if (len < 0) {
            printf("Client disconnected\n");
            break;
        }
        if (len == 0) {
            continue;  /* Empty line */
        }

        printf("< %s\n", line);

        /* Parse command */
        tcp_error_t err = tcp_parse_command(line, &cmd);

        if (err == TCP_OK) {
            /* Execute command */
            tcp_execute_command(&cmd, state, &resp);
        } else {
            /* Parse error */
            const char *msg = NULL;
            switch (err) {
                case TCP_ERR_SYNTAX:   msg = "malformed command"; break;
                case TCP_ERR_UNKNOWN:  msg = "unknown command"; break;
                case TCP_ERR_RANGE:    msg = "value out of range"; break;
                case TCP_ERR_PARAM:    msg = "invalid parameter"; break;
                default:               msg = NULL; break;
            }
            tcp_response_error(&resp, err, msg);
        }

        /* Format and send response */
        tcp_format_response(&resp, response, sizeof(response));
        printf("> %s", response);  /* Already has \n */

        if (send_line(client, response) < 0) {
            printf("Send failed\n");
            break;
        }

        /* Handle QUIT */
        if (cmd.type == CMD_QUIT) {
            break;
        }
    }

    /* Stop streaming if client disconnects */
    if (state->streaming) {
        printf("Stopping streaming (client disconnect)\n");
        if (state->hardware_connected && state->sdr_ctx) {
            psdr_stop(state->sdr_ctx);
        }
        state->streaming = false;
    }
    
    /* Disable async notifications */
    tcp_notify_clear_client(state);
}

/*============================================================================
 * Usage
 *============================================================================*/

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -p PORT    Listen port (default: %d)\n", TCP_DEFAULT_PORT);
    printf("  -T ADDR    Listen address (default: 127.0.0.1)\n");
    printf("  -n         No hardware mode (testing without SDR)\n");
    printf("  -d INDEX   Select SDR device index (default: 0)\n");
    printf("  -h         Show this help\n");
    printf("\nProtocol: See docs/SDR_TCP_CONTROL_INTERFACE.md\n");
}

/*============================================================================
 * SDR Initialization
 *============================================================================*/

static bool init_sdr(tcp_sdr_state_t *state, int device_idx) {
    printf("Initializing SDR hardware...\n");

    /* Enumerate devices */
    psdr_device_info_t devices[4];
    size_t num_devices = 0;

    psdr_error_t err = psdr_enumerate(devices, 4, &num_devices);
    if (err != PSDR_OK) {
        fprintf(stderr, "SDR enumeration failed: %s\n", psdr_strerror(err));
        return false;
    }

    if (num_devices == 0) {
        fprintf(stderr, "No SDR devices found\n");
        return false;
    }

    printf("Found %zu SDR device(s):\n", num_devices);
    for (size_t i = 0; i < num_devices; i++) {
        printf("  [%zu] %s (HW v%d)%s\n", i,
               devices[i].serial,
               devices[i].hw_version,
               devices[i].available ? "" : " [in use]");
    }

    if (device_idx >= (int)num_devices) {
        fprintf(stderr, "Device index %d out of range (0-%zu)\n",
                device_idx, num_devices - 1);
        return false;
    }

    if (!devices[device_idx].available) {
        fprintf(stderr, "Device %d is in use by another application\n", device_idx);
        return false;
    }

    /* Open selected device */
    err = psdr_open(&state->sdr_ctx, (unsigned int)device_idx);
    if (err != PSDR_OK) {
        fprintf(stderr, "Failed to open SDR: %s\n", psdr_strerror(err));
        return false;
    }

    /* Set up callbacks */
    state->sdr_callbacks.on_samples = on_samples;
    state->sdr_callbacks.on_gain_change = on_gain_change;
    state->sdr_callbacks.on_overload = on_overload;
    state->sdr_callbacks.user_ctx = state;

    /* Configure with defaults from state */
    psdr_config_defaults(&state->sdr_config);
    state->sdr_config.freq_hz = state->freq_hz;
    state->sdr_config.sample_rate_hz = (double)state->sample_rate;
    state->sdr_config.bandwidth = (psdr_bandwidth_t)state->bandwidth_khz;
    state->sdr_config.gain_reduction = state->gain_reduction;
    state->sdr_config.lna_state = state->lna_state;

    err = psdr_configure(state->sdr_ctx, &state->sdr_config);
    if (err != PSDR_OK) {
        fprintf(stderr, "Failed to configure SDR: %s\n", psdr_strerror(err));
        psdr_close(state->sdr_ctx);
        state->sdr_ctx = NULL;
        return false;
    }

    state->hardware_connected = true;
    printf("SDR initialized: %s\n", devices[device_idx].serial);
    printf("  Frequency:    %.3f MHz\n", state->freq_hz / 1e6);
    printf("  Sample Rate:  %.3f MSPS\n", state->sample_rate / 1e6);
    printf("  Bandwidth:    %d kHz\n", state->bandwidth_khz);
    printf("  Gain Red:     %d dB\n", state->gain_reduction);
    printf("  LNA State:    %d\n", state->lna_state);

    return true;
}

static void cleanup_sdr(tcp_sdr_state_t *state) {
    if (state->sdr_ctx) {
        if (state->streaming) {
            printf("Stopping SDR streaming...\n");
            psdr_stop(state->sdr_ctx);
            state->streaming = false;
        }
        printf("Closing SDR...\n");
        psdr_close(state->sdr_ctx);
        state->sdr_ctx = NULL;
        state->hardware_connected = false;
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    print_version("Phoenix SDR - TCP Control Server");

    int port = TCP_DEFAULT_PORT;
    const char *bind_addr = "127.0.0.1";
    bool no_hardware = false;
    int device_idx = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0) {
            no_hardware = true;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            device_idx = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Initialize SDR state */
    tcp_state_defaults(&g_sdr_state);
    
    /* Initialize notification mutex */
    tcp_notify_init(&g_sdr_state);

    /* Initialize SDR hardware (unless -n flag) */
    if (!no_hardware) {
        if (!init_sdr(&g_sdr_state, device_idx)) {
            fprintf(stderr, "Warning: Running without SDR hardware\n");
            fprintf(stderr, "Use -n flag to suppress this warning\n\n");
        }
    } else {
        printf("No-hardware mode: SDR commands will only update state\n\n");
    }

    /* Initialize sockets */
    if (socket_init() != 0) {
        fprintf(stderr, "Socket initialization failed\n");
        cleanup_sdr(&g_sdr_state);
        return 1;
    }

    /* Set up signal handler */
    signal(SIGINT, signal_handler);
#ifndef _WIN32
    signal(SIGTERM, signal_handler);
#endif

    /* Create listen socket */
    g_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_socket == INVALID_SOCKET) {
        fprintf(stderr, "Failed to create socket\n");
        cleanup_sdr(&g_sdr_state);
        socket_cleanup();
        return 1;
    }

    /* Allow address reuse */
    int optval = 1;
    setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval));

    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind address: %s\n", bind_addr);
        closesocket(g_listen_socket);
        cleanup_sdr(&g_sdr_state);
        socket_cleanup();
        return 1;
    }

    if (bind(g_listen_socket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Bind failed on %s:%d\n", bind_addr, port);
        closesocket(g_listen_socket);
        cleanup_sdr(&g_sdr_state);
        socket_cleanup();
        return 1;
    }

    /* Listen */
    if (listen(g_listen_socket, 1) == SOCKET_ERROR) {
        fprintf(stderr, "Listen failed\n");
        closesocket(g_listen_socket);
        cleanup_sdr(&g_sdr_state);
        socket_cleanup();
        return 1;
    }

    printf("Listening on %s:%d\n", bind_addr, port);
    printf("Hardware: %s\n", g_sdr_state.hardware_connected ? "CONNECTED" : "NOT CONNECTED");
    printf("Press Ctrl+C to stop\n\n");

    /* Accept loop */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        g_client_socket = accept(g_listen_socket, (struct sockaddr*)&client_addr, &client_len);
        if (g_client_socket == INVALID_SOCKET) {
            if (g_running) {
                fprintf(stderr, "Accept failed\n");
            }
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("Connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        /* Handle this client (blocking - single client only) */
        handle_client(g_client_socket, &g_sdr_state);

        closesocket(g_client_socket);
        g_client_socket = INVALID_SOCKET;
        printf("Client session ended\n\n");
    }

    /* Cleanup */
    cleanup_sdr(&g_sdr_state);
    tcp_notify_cleanup(&g_sdr_state);

    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
    }
    socket_cleanup();

    printf("Server stopped\n");
    return 0;
}
