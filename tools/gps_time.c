/**
 * @file gps_time.c
 * @brief GPS time reader for Windows - reads from Arduino GPS on COM port
 * 
 * Parses output format: 2025-12-12T14:30:45.123 [VALID, SAT:8, NMEA:42, ms:123]
 * 
 * Usage: gps_time [COM_PORT] [duration_sec]
 *        gps_time COM6 10
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <time.h>

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int millisecond;
    int valid;
    int satellites;
    unsigned long nmea_pulse;
    unsigned long ms_offset;
    LARGE_INTEGER pc_timestamp;  /* QueryPerformanceCounter when received */
} gps_time_t;

static HANDLE hSerial = INVALID_HANDLE_VALUE;
static LARGE_INTEGER pc_freq;

/**
 * Open COM port
 */
static int serial_open(const char *port, DWORD baud_rate) {
    char full_port[32];
    snprintf(full_port, sizeof(full_port), "\\\\.\\%s", port);
    
    hSerial = CreateFileA(full_port,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);
    
    if (hSerial == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to open %s (error %lu)\n", port, GetLastError());
        return -1;
    }
    
    /* Configure serial parameters */
    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    
    if (!GetCommState(hSerial, &dcb)) {
        CloseHandle(hSerial);
        return -1;
    }
    
    dcb.BaudRate = baud_rate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    
    if (!SetCommState(hSerial, &dcb)) {
        CloseHandle(hSerial);
        return -1;
    }
    
    /* Set timeouts */
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 200;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hSerial, &timeouts);
    
    /* Purge any existing data */
    PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
    
    return 0;
}

static void serial_close(void) {
    if (hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(hSerial);
        hSerial = INVALID_HANDLE_VALUE;
    }
}

/**
 * Read a line from serial port
 */
static int serial_read_line(char *buffer, int max_len, DWORD timeout_ms) {
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
 * Parse GPS output line - handles multiple formats
 * Format 1: 2025-12-12T14:30:45.123 [VALID, SAT:8, NMEA:42, ms:123]
 * Format 2: Waiting for GPS fix... Valid:N
 */
static int parse_gps_line(const char *line, gps_time_t *gps) {
    /* Check for "Waiting" message */
    if (strstr(line, "Waiting for GPS fix") != NULL) {
        gps->valid = 0;
        gps->satellites = 0;
        return 0;  /* Valid parse, but no fix */
    }
    
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
        
        /* Parse ms offset */
        const char *ms_pos = strstr(line, "ms:");
        if (ms_pos) {
            gps->ms_offset = strtoul(ms_pos + 3, NULL, 10);
        }
        
        return 0;
    }
    
    return -1;  /* Unrecognized format */
}

/**
 * Convert GPS time to Unix timestamp (seconds since epoch)
 */
static double gps_to_unix_time(const gps_time_t *gps) {
    struct tm tm = {0};
    tm.tm_year = gps->year - 1900;
    tm.tm_mon = gps->month - 1;
    tm.tm_mday = gps->day;
    tm.tm_hour = gps->hour;
    tm.tm_min = gps->minute;
    tm.tm_sec = gps->second;
    
    time_t t = _mkgmtime(&tm);  /* Windows-specific UTC mktime */
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
    
    /* Convert from 100ns intervals since 1601 to seconds since 1970 */
    return (uli.QuadPart - 116444736000000000ULL) / 10000000.0;
}

static void print_usage(const char *prog) {
    printf("GPS Time Reader\n");
    printf("Usage: %s [COM_PORT] [duration_sec]\n", prog);
    printf("\nDefaults: COM6, 10 seconds\n");
    printf("\nOutput: GPS time with PC timestamp for synchronization\n");
}

int main(int argc, char *argv[]) {
    const char *port = "COM6";
    int duration_sec = 10;
    
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        port = argv[1];
    }
    if (argc > 2) {
        duration_sec = atoi(argv[2]);
    }
    
    /* Initialize performance counter */
    QueryPerformanceFrequency(&pc_freq);
    
    printf("===========================================\n");
    printf("GPS Time Reader\n");
    printf("===========================================\n");
    printf("Port: %s\n", port);
    printf("Duration: %d seconds\n", duration_sec);
    printf("\n");
    
    if (serial_open(port, 115200) < 0) {
        fprintf(stderr, "Failed to open serial port\n");
        return 1;
    }
    
    printf("Connected to %s. Waiting for GPS data...\n\n", port);
    fflush(stdout);
    
    char line[256];
    gps_time_t gps;
    unsigned long last_pulse = 0;
    int valid_count = 0;
    double offset_sum = 0;
    
    DWORD start_time = GetTickCount();
    
    while ((GetTickCount() - start_time) / 1000 < (DWORD)duration_sec) {
        if (serial_read_line(line, sizeof(line), 500) > 0) {
            /* Skip header/info lines */
            if (strstr(line, "GPS Time Output") || strstr(line, "Module:") ||
                strstr(line, "Mode:") || strstr(line, "Initializing") ||
                strstr(line, "Ready.")) {
                continue;
            }
            
            /* Skip very short lines */
            if (strlen(line) < 10) {
                continue;
            }
            
            /* Timestamp when we received this line */
            LARGE_INTEGER pc_now;
            QueryPerformanceCounter(&pc_now);
            gps.pc_timestamp = pc_now;
            
            if (parse_gps_line(line, &gps) == 0) {
                if (gps.valid) {
                    double gps_time = gps_to_unix_time(&gps);
                    double sys_time = get_system_time();
                    double offset_ms = (sys_time - gps_time) * 1000.0;
                    
                    /* Detect new second */
                    int new_second = (gps.nmea_pulse != last_pulse);
                    last_pulse = gps.nmea_pulse;
                    
                    if (new_second) {
                        printf(">>> ");
                    } else {
                        printf("    ");
                    }
                    
                    printf("%04d-%02d-%02dT%02d:%02d:%02d.%03d UTC  ",
                           gps.year, gps.month, gps.day,
                           gps.hour, gps.minute, gps.second,
                           gps.millisecond);
                    printf("SAT:%2d  PC offset: %+.0f ms\n",
                           gps.satellites, offset_ms);
                    fflush(stdout);
                    
                    if (new_second) {
                        offset_sum += offset_ms;
                        valid_count++;
                    }
                } else {
                    static int wait_count = 0;
                    if (wait_count++ % 10 == 0) {  /* Print every 10th */
                        printf("    Waiting for GPS fix...\n");
                        fflush(stdout);
                    }
                }
            }
        }
    }
    
    serial_close();
    
    printf("\n===========================================\n");
    printf("SUMMARY\n");
    printf("===========================================\n");
    printf("Valid seconds received: %d\n", valid_count);
    if (valid_count > 0) {
        printf("Average PC-GPS offset: %.0f ms\n", offset_sum / valid_count);
        printf("(Positive = PC ahead of GPS)\n");
    }
    printf("===========================================\n");
    
    return 0;
}
