/**
 * @file ntp_time.c
 * @brief Simple NTP client implementation
 */

#include "ntp_time.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* NTP epoch is 1900-01-01, Unix epoch is 1970-01-01 */
#define NTP_UNIX_DELTA 2208988800ULL

/* NTP packet structure (48 bytes) */
typedef struct {
    uint8_t  li_vn_mode;      /* LI(2), VN(3), Mode(3) */
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec;
    uint32_t ref_ts_frac;
    uint32_t orig_ts_sec;
    uint32_t orig_ts_frac;
    uint32_t rx_ts_sec;
    uint32_t rx_ts_frac;
    uint32_t tx_ts_sec;       /* Transmit timestamp - what we want */
    uint32_t tx_ts_frac;
} ntp_packet_t;

static int g_winsock_init = 0;

int ntp_init(void) {
#ifdef _WIN32
    if (!g_winsock_init) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return -1;
        }
        g_winsock_init = 1;
    }
#endif
    return 0;
}

void ntp_cleanup(void) {
#ifdef _WIN32
    if (g_winsock_init) {
        WSACleanup();
        g_winsock_init = 0;
    }
#endif
}

ntp_time_t ntp_get_utc(const char *server, int timeout_ms) {
    ntp_time_t result = {0};
    result.valid = false;
    
    if (!server) server = "time.nist.gov";
    if (timeout_ms <= 0) timeout_ms = 2000;
    
    /* Ensure Winsock is initialized */
    if (ntp_init() < 0) {
        fprintf(stderr, "[NTP] Winsock init failed\n");
        return result;
    }
    
    /* Resolve server address */
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    
    if (getaddrinfo(server, "123", &hints, &res) != 0) {
        fprintf(stderr, "[NTP] Failed to resolve %s\n", server);
        return result;
    }
    
    /* Create UDP socket */
#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
#else
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
#endif
        fprintf(stderr, "[NTP] Socket creation failed\n");
        freeaddrinfo(res);
        return result;
    }
    
    /* Set timeout */
#ifdef _WIN32
    DWORD tv = timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    
    /* Build NTP request packet */
    ntp_packet_t packet = {0};
    packet.li_vn_mode = (0 << 6) | (3 << 3) | 3;  /* LI=0, VN=3, Mode=3 (client) */
    
    /* Send request */
    if (sendto(sock, (const char*)&packet, sizeof(packet), 0,
               res->ai_addr, (int)res->ai_addrlen) < 0) {
        fprintf(stderr, "[NTP] Send failed\n");
        goto cleanup;
    }
    
    /* Receive response */
    int n = recvfrom(sock, (char*)&packet, sizeof(packet), 0, NULL, NULL);
    if (n < (int)sizeof(packet)) {
        fprintf(stderr, "[NTP] Receive failed or timeout\n");
        goto cleanup;
    }
    
    /* Extract transmit timestamp (network byte order -> host) */
    result.seconds = ntohl(packet.tx_ts_sec);
    result.fraction = ntohl(packet.tx_ts_frac);
    
    /* Convert to Unix time */
    result.unix_seconds = (int64_t)result.seconds - NTP_UNIX_DELTA;
    result.unix_time = (double)result.unix_seconds + 
                       (double)result.fraction / 4294967296.0;
    result.valid = true;
    
cleanup:
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    freeaddrinfo(res);
    return result;
}

char *ntp_format_iso(const ntp_time_t *t, char *buf, size_t len) {
    if (!t || !buf || len < 32 || !t->valid) {
        if (buf && len > 0) buf[0] = '\0';
        return NULL;
    }
    
    time_t unix_sec = (time_t)t->unix_seconds;
    struct tm *utc = gmtime(&unix_sec);
    
    if (!utc) {
        buf[0] = '\0';
        return NULL;
    }
    
    /* Get milliseconds from fractional part */
    int ms = (int)((double)t->fraction / 4294967296.0 * 1000.0);
    
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
             utc->tm_hour, utc->tm_min, utc->tm_sec, ms);
    
    return buf;
}
