/**
 * @file waterfall_telemetry.c
 * @brief UDP broadcast telemetry implementation
 *
 * Non-blocking UDP broadcast for remote monitoring.
 * Uses broadcast address for zero-configuration discovery.
 */

#include "waterfall_telemetry.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define socket_close closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int socket_t;
#define SOCKET_INVALID (-1)
#define socket_close close
#endif

/*============================================================================
 * Module State
 *============================================================================*/

static socket_t g_sock = SOCKET_INVALID;
static struct sockaddr_in g_broadcast_addr;
static uint32_t g_enabled_channels = TELEM_NONE;
static uint32_t g_stats_sent = 0;
static uint32_t g_stats_dropped = 0;
static bool g_initialized = false;

/*============================================================================
 * Console Buffer (for hot-path performance)
 *============================================================================*/

#define CONSOLE_BUFFER_SIZE 8192

static char g_console_buffer[CONSOLE_BUFFER_SIZE];
static int g_console_buffer_len = 0;
static uint32_t g_console_dropped = 0;

/*============================================================================
 * Channel Prefixes
 *============================================================================*/

static const char *g_channel_prefixes[] = {
    "????",  /* TELEM_NONE (unused) */
    "CHAN",  /* TELEM_CHANNEL */
    "TICK",  /* TELEM_TICKS */
    "MARK",  /* TELEM_MARKERS */
    "CARR",  /* TELEM_CARRIER */
    "SYNC",  /* TELEM_SYNC */
    "SUBC",  /* TELEM_SUBCAR */
    "CORR",  /* TELEM_CORR */
    "T500",  /* TELEM_TONE500 */
    "T600",  /* TELEM_TONE600 */
    "BCDE",  /* TELEM_BCD_ENV (100 Hz envelope) */
    "BCDS",  /* TELEM_BCDS (BCD symbols/time) */
    "CONS",  /* TELEM_CONSOLE (console messages) */
    "CTRL",  /* TELEM_CTRL (control commands) */
    "RESP",  /* TELEM_RESP (command responses) */
};

/*============================================================================
 * Internal Helpers
 *============================================================================*/

static int get_channel_index(telem_channel_t channel) {
    switch (channel) {
        case TELEM_CHANNEL: return 1;
        case TELEM_TICKS:   return 2;
        case TELEM_MARKERS: return 3;
        case TELEM_CARRIER: return 4;
        case TELEM_SYNC:    return 5;
        case TELEM_SUBCAR:  return 6;
        case TELEM_CORR:    return 7;
        case TELEM_TONE500: return 8;
        case TELEM_TONE600: return 9;
        case TELEM_BCD_ENV: return 10;
        case TELEM_BCDS:    return 11;
        case TELEM_CONSOLE: return 12;
        case TELEM_CTRL:    return 13;
        case TELEM_RESP:    return 14;
        default:            return 0;
    }
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

bool telem_init(int port) {
    if (g_initialized) {
        return true;  /* Already initialized */
    }

    if (port <= 0) {
        port = TELEM_DEFAULT_PORT;
    }

#ifdef _WIN32
    /* Initialize Winsock if needed */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[TELEM] WSAStartup failed\n");
        return false;
    }
#endif

    /* Create UDP socket */
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == SOCKET_INVALID) {
        fprintf(stderr, "[TELEM] Failed to create UDP socket\n");
        return false;
    }

    /* Enable broadcast */
    int broadcast = 1;
    if (setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST,
                   (const char *)&broadcast, sizeof(broadcast)) < 0) {
        fprintf(stderr, "[TELEM] Failed to enable broadcast\n");
        socket_close(g_sock);
        g_sock = SOCKET_INVALID;
        return false;
    }

    /* Set non-blocking */
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(g_sock, FIONBIO, &mode);
#else
    int flags = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, flags | O_NONBLOCK);
#endif

    /* Setup broadcast address */
    memset(&g_broadcast_addr, 0, sizeof(g_broadcast_addr));
    g_broadcast_addr.sin_family = AF_INET;
    g_broadcast_addr.sin_port = htons((uint16_t)port);
    g_broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);  /* 255.255.255.255 */

    /* Enable all channels by default */
    g_enabled_channels = TELEM_ALL;
    g_stats_sent = 0;
    g_stats_dropped = 0;
    g_initialized = true;

    printf("[TELEM] UDP broadcast initialized on port %d\n", port);
    return true;
}

void telem_cleanup(void) {
    if (!g_initialized) return;

    if (g_sock != SOCKET_INVALID) {
        socket_close(g_sock);
        g_sock = SOCKET_INVALID;
    }

    g_enabled_channels = TELEM_NONE;
    g_initialized = false;

    printf("[TELEM] Cleanup complete. Sent: %u, Dropped: %u\n",
           g_stats_sent, g_stats_dropped);
}

void telem_enable(uint32_t channels) {
    g_enabled_channels |= channels;
}

void telem_disable(uint32_t channels) {
    g_enabled_channels &= ~channels;
}

void telem_set_channels(uint32_t channels) {
    g_enabled_channels = channels;
}

uint32_t telem_get_channels(void) {
    return g_enabled_channels;
}

bool telem_is_enabled(telem_channel_t channel) {
    return (g_enabled_channels & channel) != 0;
}

const char *telem_channel_prefix(telem_channel_t channel) {
    int idx = get_channel_index(channel);
    if (idx >= 0 && idx < (int)(sizeof(g_channel_prefixes) / sizeof(g_channel_prefixes[0]))) {
        return g_channel_prefixes[idx];
    }
    return "????";
}

void telem_send(telem_channel_t channel, const char *csv_line) {
    /* Fast path: check if enabled before any work */
    if (!g_initialized || g_sock == SOCKET_INVALID) {
        return;
    }

    if ((g_enabled_channels & channel) == 0) {
        g_stats_dropped++;
        return;
    }

    if (!csv_line || csv_line[0] == '\0') {
        return;
    }

    /* Format message with prefix */
    char buffer[TELEM_MAX_MESSAGE_LEN];
    const char *prefix = telem_channel_prefix(channel);

    /* Check if csv_line already has newline */
    size_t len = strlen(csv_line);
    bool has_newline = (len > 0 && csv_line[len - 1] == '\n');

    int written;
    if (has_newline) {
        written = snprintf(buffer, sizeof(buffer), "%s,%s", prefix, csv_line);
    } else {
        written = snprintf(buffer, sizeof(buffer), "%s,%s\n", prefix, csv_line);
    }

    if (written <= 0 || written >= (int)sizeof(buffer)) {
        g_stats_dropped++;
        return;
    }

    /* Send (non-blocking, ignore errors) */
    sendto(g_sock, buffer, written, 0,
           (struct sockaddr *)&g_broadcast_addr, sizeof(g_broadcast_addr));

    g_stats_sent++;
}

void telem_sendf(telem_channel_t channel, const char *fmt, ...) {
    /* Fast path: check if enabled before formatting */
    if (!g_initialized || (g_enabled_channels & channel) == 0) {
        if (g_initialized) g_stats_dropped++;
        return;
    }

    char csv_buffer[TELEM_MAX_MESSAGE_LEN - 8];  /* Reserve space for prefix */
    va_list args;
    va_start(args, fmt);
    vsnprintf(csv_buffer, sizeof(csv_buffer), fmt, args);
    va_end(args);

    telem_send(channel, csv_buffer);
}

void telem_get_stats(uint32_t *sent, uint32_t *dropped) {
    if (sent) *sent = g_stats_sent;
    if (dropped) *dropped = g_stats_dropped;
}

void telem_console_flush(void) {
    if (g_console_buffer_len == 0) {
        return;
    }

    /* Send buffered console data */
    if (g_initialized && (g_enabled_channels & TELEM_CONSOLE)) {
        /* Ensure null termination */
        if (g_console_buffer_len < CONSOLE_BUFFER_SIZE) {
            g_console_buffer[g_console_buffer_len] = '\0';
        } else {
            g_console_buffer[CONSOLE_BUFFER_SIZE - 1] = '\0';
        }
        telem_send(TELEM_CONSOLE, g_console_buffer);
    }

    /* Reset buffer */
    g_console_buffer_len = 0;
}

void telem_console(const char *fmt, ...) {
    /* Fast path: check if enabled */
    if (!g_initialized || (g_enabled_channels & TELEM_CONSOLE) == 0) {
        return;
    }

    /* Format message into temp buffer */
    char temp[512];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);

    if (written <= 0) {
        return;
    }

    /* Truncate if too long */
    if (written >= (int)sizeof(temp)) {
        written = (int)sizeof(temp) - 1;
    }

    /* Check if buffer has space */
    int space_available = CONSOLE_BUFFER_SIZE - g_console_buffer_len - 1;
    if (written > space_available) {
        /* Flush current buffer first */
        telem_console_flush();
        space_available = CONSOLE_BUFFER_SIZE - 1;

        /* If still doesn't fit, drop it and count */
        if (written > space_available) {
            g_console_dropped++;
            return;
        }
    }

    /* Append to buffer */
    memcpy(g_console_buffer + g_console_buffer_len, temp, written);
    g_console_buffer_len += written;

    /* Auto-flush on newline */
    if (written > 0 && temp[written - 1] == '\n') {
        telem_console_flush();
    }
}
