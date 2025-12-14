/**
 * @file ntp_time.h
 * @brief Simple NTP client for UTC timestamping
 *
 * Provides accurate UTC time from NTP servers for recording timestamps.
 */

#ifndef NTP_TIME_H
#define NTP_TIME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

/**
 * NTP timestamp - seconds since 1900-01-01 00:00:00 UTC
 */
typedef struct {
    uint32_t seconds;       /* Seconds since NTP epoch (1900) */
    uint32_t fraction;      /* Fractional seconds (2^32 = 1 second) */
    int64_t  unix_seconds;  /* Seconds since Unix epoch (1970) */
    double   unix_time;     /* Unix time with fractional seconds */
    bool     valid;         /* True if NTP query succeeded */
} ntp_time_t;

/**
 * Query NTP server for current UTC time
 * 
 * @param server  NTP server hostname (NULL for default: time.nist.gov)
 * @param timeout_ms  Timeout in milliseconds (0 for default: 2000)
 * @return NTP timestamp structure with valid flag
 */
ntp_time_t ntp_get_utc(const char *server, int timeout_ms);

/**
 * Initialize Winsock (Windows only, call once at startup)
 * Returns 0 on success, -1 on failure
 */
int ntp_init(void);

/**
 * Cleanup Winsock (Windows only, call at shutdown)
 */
void ntp_cleanup(void);

/**
 * Format NTP time as ISO 8601 string
 * 
 * @param t      NTP timestamp
 * @param buf    Output buffer (at least 32 bytes)
 * @param len    Buffer length
 * @return       Pointer to buf, or NULL on error
 */
char *ntp_format_iso(const ntp_time_t *t, char *buf, size_t len);

#endif /* NTP_TIME_H */
