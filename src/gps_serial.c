/**
 * @file gps_serial.c
 * @brief GPS serial interface implementation
 * 
 * Reads GPS time from Arduino NEO-6M output format:
 * 2025-12-12T14:30:45.123 [VALID, SAT:8, NMEA:42, ms:123]
 */

#include "gps_serial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32

/* Default serial latency for NEO-6M (measured) */
#define DEFAULT_LATENCY_MS  302.0

/**
 * Read a line from serial port
 */
static int serial_read_line(HANDLE hSerial, char *buffer, int max_len, DWORD timeout_ms) {
    DWORD start_time = GetTickCount();
    int pos = 0;
    
    while (GetTickCount() - start_time < timeout_ms && pos < max_len - 1) {
        char c;
        DWORD bytes_read = 0;
        
        if (ReadFile(hSerial, &c, 1, &bytes_read, NULL) && bytes_read > 0) {
            if (c == '\n') {
                buffer[pos] = '\0';
                /* Remove trailing \r */
                if (pos > 0 && buffer[pos-1] == '\r') {
                    buffer[pos-1] = '\0';
                }
                return pos;
            }
            buffer[pos++] = c;
        }
    }
    
    buffer[pos] = '\0';
    return pos;
}

/**
 * Parse GPS output line
 * Format: 2025-12-12T14:30:45.123 [VALID, SAT:8, NMEA:42, ms:123]
 */
static int parse_gps_line(const char *line, gps_reading_t *gps) {
    /* Check for "Waiting" message */
    if (strstr(line, "Waiting for GPS fix") != NULL) {
        gps->valid = false;
        gps->satellites = 0;
        return 0;
    }
    
    /* Initialize millisecond to detect parse failures */
    gps->millisecond = -1;
    
    /* Try format: YYYY-MM-DDTHH:MM:SS.mmm */
    int parsed = sscanf(line, "%d-%d-%dT%d:%d:%d.%d",
        &gps->year, &gps->month, &gps->day,
        &gps->hour, &gps->minute, &gps->second,
        &gps->millisecond);
    
    if (parsed == 7) {
        /* Check for VALID status */
        gps->valid = (strstr(line, "VALID") != NULL && strstr(line, "NO FIX") == NULL);
        
        /* Parse satellites */
        const char *sat_pos = strstr(line, "SAT:");
        if (sat_pos) {
            gps->satellites = atoi(sat_pos + 4);
        }
        
        /* Parse NMEA pulse count */
        const char *nmea_pos = strstr(line, "NMEA:");
        if (nmea_pos) {
            gps->nmea_pulse = strtoul(nmea_pos + 5, NULL, 10);
        }
        
        /* Format ISO string */
        snprintf(gps->iso_string, sizeof(gps->iso_string),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 gps->year, gps->month, gps->day,
                 gps->hour, gps->minute, gps->second,
                 gps->millisecond);
        
        return 0;
    }
    
    return -1;  /* Unrecognized format */
}

/**
 * Convert GPS time to Unix timestamp
 */
static double gps_to_unix_time(const gps_reading_t *gps) {
    struct tm tm = {0};
    tm.tm_year = gps->year - 1900;
    tm.tm_mon = gps->month - 1;
    tm.tm_mday = gps->day;
    tm.tm_hour = gps->hour;
    tm.tm_min = gps->minute;
    tm.tm_sec = gps->second;
    
    time_t t = _mkgmtime(&tm);
    return (double)t + gps->millisecond / 1000.0;
}

/**
 * Get current system time as Unix timestamp
 */
static double get_system_time(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    
    return (uli.QuadPart - 116444736000000000ULL) / 10000000.0;
}

int gps_open(gps_context_t *ctx, const char *port, int baud_rate) {
    if (!ctx || !port) return -1;
    
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->port, port, sizeof(ctx->port) - 1);
    ctx->latency_ms = DEFAULT_LATENCY_MS;
    
    /* Initialize performance counter */
    QueryPerformanceFrequency(&ctx->pc_freq);
    
    /* Build full port name */
    char full_port[64];
    snprintf(full_port, sizeof(full_port), "\\\\.\\%s", port);
    
    ctx->hSerial = CreateFileA(full_port,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);
    
    if (ctx->hSerial == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[GPS] Failed to open %s (error %lu)\n", port, GetLastError());
        return -1;
    }
    
    /* Configure serial parameters */
    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    
    if (!GetCommState(ctx->hSerial, &dcb)) {
        CloseHandle(ctx->hSerial);
        ctx->hSerial = INVALID_HANDLE_VALUE;
        return -1;
    }
    
    dcb.BaudRate = (baud_rate > 0) ? baud_rate : 115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    
    if (!SetCommState(ctx->hSerial, &dcb)) {
        CloseHandle(ctx->hSerial);
        ctx->hSerial = INVALID_HANDLE_VALUE;
        return -1;
    }
    
    /* Set timeouts */
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 200;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    SetCommTimeouts(ctx->hSerial, &timeouts);
    
    /* Purge any existing data */
    PurgeComm(ctx->hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
    
    ctx->connected = true;
    fprintf(stderr, "[GPS] Connected to %s at %lu baud\n", port, (unsigned long)dcb.BaudRate);
    
    return 0;
}

void gps_close(gps_context_t *ctx) {
    if (!ctx) return;
    
    if (ctx->hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(ctx->hSerial);
        ctx->hSerial = INVALID_HANDLE_VALUE;
    }
    ctx->connected = false;
}

bool gps_is_connected(const gps_context_t *ctx) {
    return ctx && ctx->connected && ctx->hSerial != INVALID_HANDLE_VALUE;
}

int gps_read_time(gps_context_t *ctx, gps_reading_t *reading, int timeout_ms) {
    if (!ctx || !reading || !gps_is_connected(ctx)) return -1;
    
    memset(reading, 0, sizeof(*reading));
    
    char line[256];
    DWORD start = GetTickCount();
    
    while (GetTickCount() - start < (DWORD)timeout_ms) {
        if (serial_read_line(ctx->hSerial, line, sizeof(line), 500) > 0) {
            /* Skip header/info lines */
            if (strstr(line, "GPS Time Output") || strstr(line, "Module:") ||
                strstr(line, "Mode:") || strstr(line, "Initializing") ||
                strstr(line, "Ready.") || strlen(line) < 10) {
                continue;
            }
            
            if (parse_gps_line(line, reading) == 0 && reading->valid) {
                /* Calculate times */
                reading->unix_time = gps_to_unix_time(reading);
                double sys_time = get_system_time();
                
                /* PC offset = system_time - gps_time (positive = PC ahead) */
                /* Compensate for serial latency */
                reading->pc_offset_ms = (sys_time - reading->unix_time) * 1000.0 - ctx->latency_ms;
                
                return 0;
            }
        }
    }
    
    return -1;  /* Timeout */
}

int gps_wait_second(gps_context_t *ctx, gps_reading_t *reading, int timeout_ms) {
    if (!ctx || !reading || !gps_is_connected(ctx)) return -1;
    
    char line[256];
    DWORD start = GetTickCount();
    
    while (GetTickCount() - start < (DWORD)timeout_ms) {
        if (serial_read_line(ctx->hSerial, line, sizeof(line), 500) > 0) {
            if (strlen(line) < 10) continue;
            
            gps_reading_t temp;
            if (parse_gps_line(line, &temp) == 0 && temp.valid) {
                /* Check if this is a new second (NMEA pulse changed) */
                if (temp.nmea_pulse != ctx->last_pulse) {
                    ctx->last_pulse = temp.nmea_pulse;
                    
                    /* Fill in full reading */
                    *reading = temp;
                    reading->unix_time = gps_to_unix_time(reading);
                    double sys_time = get_system_time();
                    reading->pc_offset_ms = (sys_time - reading->unix_time) * 1000.0 - ctx->latency_ms;
                    
                    return 0;
                }
                ctx->last_pulse = temp.nmea_pulse;
            }
        }
    }
    
    return -1;  /* Timeout */
}

void gps_set_latency(gps_context_t *ctx, double latency_ms) {
    if (ctx) ctx->latency_ms = latency_ms;
}

double gps_get_latency(const gps_context_t *ctx) {
    return ctx ? ctx->latency_ms : DEFAULT_LATENCY_MS;
}

void gps_format_reading(const gps_reading_t *reading, char *buffer, size_t len) {
    if (!reading || !buffer || len == 0) return;
    
    if (reading->valid) {
        snprintf(buffer, len, "%02d:%02d:%02d.%03d SAT:%d Δ:%+.0fms",
                 reading->hour, reading->minute, reading->second,
                 reading->millisecond, reading->satellites,
                 reading->pc_offset_ms);
    } else {
        snprintf(buffer, len, "NO FIX");
    }
}

#else
/* Linux/POSIX implementation stub */
int gps_open(gps_context_t *ctx, const char *port, int baud_rate) {
    (void)ctx; (void)port; (void)baud_rate;
    fprintf(stderr, "[GPS] Linux not yet implemented\n");
    return -1;
}

void gps_close(gps_context_t *ctx) { (void)ctx; }
bool gps_is_connected(const gps_context_t *ctx) { (void)ctx; return false; }
int gps_read_time(gps_context_t *ctx, gps_reading_t *reading, int timeout_ms) {
    (void)ctx; (void)reading; (void)timeout_ms; return -1;
}
int gps_wait_second(gps_context_t *ctx, gps_reading_t *reading, int timeout_ms) {
    (void)ctx; (void)reading; (void)timeout_ms; return -1;
}
void gps_set_latency(gps_context_t *ctx, double latency_ms) { (void)ctx; (void)latency_ms; }
double gps_get_latency(const gps_context_t *ctx) { (void)ctx; return 0; }
void gps_format_reading(const gps_reading_t *reading, char *buffer, size_t len) {
    (void)reading; if (buffer && len > 0) buffer[0] = '\0';
}
#endif
