/**
 * @file wwv_gps_verify.c
 * @brief GPS verification tool for WWV timing
 * 
 * Compares WWV-derived timing with GPS reference to validate accuracy.
 * 
 * Usage:
 *   wwv_gps_verify -t <recording_timestamp> -o <wwv_offset_ms> [-p <COM_port>]
 *   wwv_gps_verify -t "2025-12-13T13:15:30" -o 21350.5
 *   wwv_gps_verify -t "2025-12-13T13:15:30" -o 21350.5 -p COM6
 * 
 * The tool:
 *   1. Takes the recording start timestamp (ISO 8601 or Unix epoch)
 *   2. Takes the WWV offset (ms from file start to minute boundary)
 *   3. Optionally reads current GPS time for live comparison
 *   4. Reports WWV-derived UTC time and accuracy
 * 
 * Part of Phoenix Nest MARS Suite
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#endif

/* GPS time structure */
typedef struct {
    int year, month, day;
    int hour, minute, second;
    int millisecond;
    int valid;
    int satellites;
    double pc_timestamp;  /* PC time when received */
} gps_time_t;

/* Serial port handle */
#ifdef _WIN32
static HANDLE hSerial = INVALID_HANDLE_VALUE;
#else
static int serial_fd = -1;
#endif

/* GPS serial latency compensation (measured ~302ms for Arduino GPS) */
#define GPS_SERIAL_LATENCY_MS 302

/**
 * Get current system time as Unix timestamp with milliseconds
 */
static double get_system_time(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    /* Convert from 100ns intervals since 1601 to seconds since 1970 */
    return (uli.QuadPart - 116444736000000000ULL) / 10000000.0;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
#endif
}

/**
 * Parse ISO 8601 timestamp: YYYY-MM-DDTHH:MM:SS[.sss]
 * Returns Unix timestamp (seconds since epoch)
 */
static double parse_iso_timestamp(const char *str) {
    int year, month, day, hour, minute;
    double second = 0;
    
    /* Try full format with fractional seconds */
    if (sscanf(str, "%d-%d-%dT%d:%d:%lf", &year, &month, &day, &hour, &minute, &second) >= 5) {
        struct tm tm = {0};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = (int)second;
        
#ifdef _WIN32
        time_t t = _mkgmtime(&tm);
#else
        time_t t = timegm(&tm);
#endif
        /* Add fractional seconds */
        return (double)t + (second - (int)second);
    }
    
    /* Try Unix timestamp */
    double ts = atof(str);
    if (ts > 1000000000.0) {  /* Looks like Unix timestamp */
        return ts;
    }
    
    return -1;  /* Failed to parse */
}

/**
 * Convert Unix timestamp to human-readable UTC string
 */
static void timestamp_to_string(double ts, char *buf, size_t len) {
    time_t t = (time_t)ts;
    int ms = (int)((ts - t) * 1000);
    
#ifdef _WIN32
    struct tm *tm = gmtime(&t);
#else
    struct tm tm_buf;
    struct tm *tm = gmtime_r(&t, &tm_buf);
#endif
    
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03d UTC",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, ms);
}

#ifdef _WIN32
/**
 * Open COM port (Windows)
 */
static int serial_open(const char *port) {
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
    
    dcb.BaudRate = 115200;
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
    timeouts.ReadTotalTimeoutConstant = 2000;
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
static int serial_read_line(char *buffer, int max_len, int timeout_ms) {
    DWORD start_time = GetTickCount();
    int pos = 0;
    
    while ((int)(GetTickCount() - start_time) < timeout_ms && pos < max_len - 1) {
        char c;
        DWORD bytes_read = 0;
        
        if (ReadFile(hSerial, &c, 1, &bytes_read, NULL) && bytes_read > 0) {
            if (c == '\n') {
                buffer[pos] = '\0';
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
#else
/* Linux/Unix serial implementation */
static int serial_open(const char *port) {
    serial_fd = open(port, O_RDWR | O_NOCTTY);
    if (serial_fd < 0) {
        perror("Failed to open serial port");
        return -1;
    }
    
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    tcgetattr(serial_fd, &tty);
    
    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);
    
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_iflag = 0;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 10;  /* 1 second timeout */
    
    tcsetattr(serial_fd, TCSANOW, &tty);
    return 0;
}

static void serial_close(void) {
    if (serial_fd >= 0) {
        close(serial_fd);
        serial_fd = -1;
    }
}

static int serial_read_line(char *buffer, int max_len, int timeout_ms) {
    int pos = 0;
    time_t start = time(NULL);
    
    while ((time(NULL) - start) * 1000 < timeout_ms && pos < max_len - 1) {
        char c;
        if (read(serial_fd, &c, 1) == 1) {
            if (c == '\n') {
                buffer[pos] = '\0';
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
#endif

/**
 * Parse GPS output line
 * Format: YYYY-MM-DDTHH:MM:SS.mmm [VALID, SAT:n, NMEA:n, ms:n]
 */
static int parse_gps_line(const char *line, gps_time_t *gps) {
    /* Check for waiting message */
    if (strstr(line, "Waiting") != NULL) {
        gps->valid = 0;
        return 0;
    }
    
    /* Parse timestamp */
    int parsed = sscanf(line, "%d-%d-%dT%d:%d:%d.%d",
        &gps->year, &gps->month, &gps->day,
        &gps->hour, &gps->minute, &gps->second,
        &gps->millisecond);
    
    if (parsed == 7) {
        gps->valid = (strstr(line, "VALID") != NULL && strstr(line, "NO FIX") == NULL);
        
        /* Parse satellites */
        const char *sat_pos = strstr(line, "SAT:");
        if (sat_pos) {
            gps->satellites = atoi(sat_pos + 4);
        }
        return 0;
    }
    
    return -1;
}

/**
 * Convert GPS time to Unix timestamp
 */
static double gps_to_unix_time(const gps_time_t *gps) {
    struct tm tm = {0};
    tm.tm_year = gps->year - 1900;
    tm.tm_mon = gps->month - 1;
    tm.tm_mday = gps->day;
    tm.tm_hour = gps->hour;
    tm.tm_min = gps->minute;
    tm.tm_sec = gps->second;
    
#ifdef _WIN32
    time_t t = _mkgmtime(&tm);
#else
    time_t t = timegm(&tm);
#endif
    return (double)t + gps->millisecond / 1000.0;
}

/**
 * Read GPS time with PC timestamp correlation
 */
static int read_gps_time(gps_time_t *gps, int timeout_sec) {
    char line[256];
    time_t start = time(NULL);
    
    while (time(NULL) - start < timeout_sec) {
        if (serial_read_line(line, sizeof(line), 2000) > 0) {
            /* Skip header lines */
            if (strstr(line, "GPS Time Output") || strstr(line, "Hardware:") ||
                strstr(line, "Mode:") || strstr(line, "Ready") ||
                strlen(line) < 10) {
                continue;
            }
            
            /* Record PC time when we receive the line */
            gps->pc_timestamp = get_system_time();
            
            if (parse_gps_line(line, gps) == 0 && gps->valid) {
                return 0;  /* Success */
            }
        }
    }
    
    return -1;  /* Timeout */
}

static void print_usage(const char *prog) {
    printf("WWV-GPS Timing Verification Tool\n");
    printf("Phoenix Nest MARS Suite - Phase 3 GPS Integration\n\n");
    printf("Usage: %s -t <timestamp> -o <offset_ms> [-p <port>] [-d]\n\n", prog);
    printf("Required:\n");
    printf("  -t <timestamp>  Recording start time (ISO 8601 or Unix epoch)\n");
    printf("                  Examples: 2025-12-13T13:15:30 or 1734095730.5\n");
    printf("  -o <offset_ms>  WWV offset from file start to minute boundary (ms)\n");
    printf("                  This is the \"Offset from file start\" from wwv_sync\n");
    printf("\nOptional:\n");
    printf("  -p <port>       GPS serial port (e.g., COM6 or /dev/ttyUSB0)\n");
    printf("                  If provided, reads current GPS for live comparison\n");
    printf("  -d              Decode mode: show expected vs actual minute\n");
    printf("  -h              Show this help\n");
    printf("\nExample workflow:\n");
    printf("  1. Record WWV:    phoenix_sdr -c 10000000 -s 48000 -o wwv.iqr\n");
    printf("  2. Note the time when you start recording\n");
    printf("  3. Analyze:       wwv_sync wwv.iqr 0 300\n");
    printf("  4. Verify:        wwv_gps_verify -t 2025-12-13T13:15:30 -o 21350.5\n");
    printf("\nThe tool calculates:\n");
    printf("  - UTC time of each minute boundary in the recording\n");
    printf("  - Expected vs actual minute (if BCD decode available)\n");
    printf("  - Error vs GPS reference (if -p port specified)\n");
}

int main(int argc, char *argv[]) {
    const char *timestamp_str = NULL;
    const char *gps_port = NULL;
    double wwv_offset_ms = -1;
    int decode_mode = 0;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            timestamp_str = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            wwv_offset_ms = atof(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            gps_port = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0) {
            decode_mode = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }
    
    if (!timestamp_str || wwv_offset_ms < 0) {
        print_usage(argv[0]);
        return 1;
    }
    
    printf("===========================================\n");
    printf("WWV-GPS Timing Verification\n");
    printf("===========================================\n\n");
    
    /* Parse recording timestamp */
    double rec_start_ts = parse_iso_timestamp(timestamp_str);
    if (rec_start_ts < 0) {
        fprintf(stderr, "Error: Could not parse timestamp: %s\n", timestamp_str);
        return 1;
    }
    
    char buf[64];
    timestamp_to_string(rec_start_ts, buf, sizeof(buf));
    printf("Recording start:    %s\n", buf);
    printf("WWV offset:         %.1f ms\n", wwv_offset_ms);
    
    /* Calculate minute boundary time */
    double minute_boundary_ts = rec_start_ts + (wwv_offset_ms / 1000.0);
    timestamp_to_string(minute_boundary_ts, buf, sizeof(buf));
    printf("\n");
    printf("===========================================\n");
    printf("WWV-DERIVED TIMING\n");
    printf("===========================================\n");
    printf("First minute boundary: %s\n", buf);
    
    /* Calculate which UTC minute this should be */
    time_t minute_t = (time_t)minute_boundary_ts;
    struct tm *tm;
#ifdef _WIN32
    tm = gmtime(&minute_t);
#else
    struct tm tm_buf;
    tm = gmtime_r(&minute_t, &tm_buf);
#endif
    
    /* How far into the minute is the boundary? */
    double sec_into_minute = fmod(minute_boundary_ts, 60.0);
    printf("Seconds into minute: %.3f\n", sec_into_minute);
    
    /* The minute boundary should be at :00 of some minute */
    /* If sec_into_minute is close to 0 or 60, we're aligned */
    double alignment_error_ms;
    if (sec_into_minute < 30) {
        alignment_error_ms = sec_into_minute * 1000.0;
    } else {
        alignment_error_ms = (sec_into_minute - 60.0) * 1000.0;
    }
    
    printf("Alignment error:     %.1f ms", alignment_error_ms);
    if (fabs(alignment_error_ms) < 100) {
        printf(" (GOOD - within 100ms)\n");
    } else if (fabs(alignment_error_ms) < 500) {
        printf(" (FAIR - within 500ms)\n");
    } else {
        printf(" (POOR - check recording timestamp)\n");
    }
    
    /* Round to nearest minute for expected UTC minute */
    time_t expected_minute_t = (time_t)(minute_boundary_ts + 0.5);
    expected_minute_t = (expected_minute_t / 60) * 60;  /* Round down to minute */
    if (sec_into_minute > 30) {
        expected_minute_t += 60;  /* Round up */
    }
    
#ifdef _WIN32
    struct tm *expected_tm = gmtime(&expected_minute_t);
#else
    struct tm expected_tm_buf;
    struct tm *expected_tm = gmtime_r(&expected_minute_t, &expected_tm_buf);
#endif
    
    printf("\nExpected UTC minute: %02d:%02d:00\n", 
           expected_tm->tm_hour, expected_tm->tm_min);
    
    if (decode_mode) {
        printf("\n(Compare with BCD decode from wwv_sync output)\n");
    }
    
    /* GPS comparison if port specified */
    if (gps_port) {
        printf("\n");
        printf("===========================================\n");
        printf("GPS REFERENCE\n");
        printf("===========================================\n");
        printf("Opening %s...\n", gps_port);
        
        if (serial_open(gps_port) == 0) {
            printf("Waiting for GPS fix...\n");
            
            gps_time_t gps;
            double gps_offsets[10];
            int gps_count = 0;
            
            /* Collect several GPS readings */
            for (int i = 0; i < 10 && gps_count < 5; i++) {
                if (read_gps_time(&gps, 3) == 0) {
                    double gps_ts = gps_to_unix_time(&gps);
                    
                    /* PC timestamp when we received GPS - compensate for serial latency */
                    double pc_ts_corrected = gps.pc_timestamp - (GPS_SERIAL_LATENCY_MS / 1000.0);
                    
                    /* Offset: how much PC is ahead of GPS */
                    double offset = (pc_ts_corrected - gps_ts) * 1000.0;
                    gps_offsets[gps_count++] = offset;
                    
                    printf("  GPS: %04d-%02d-%02dT%02d:%02d:%02d.%03d UTC  SAT:%2d  PC offset: %+.0f ms\n",
                           gps.year, gps.month, gps.day,
                           gps.hour, gps.minute, gps.second, gps.millisecond,
                           gps.satellites, offset);
                }
            }
            
            serial_close();
            
            if (gps_count > 0) {
                /* Calculate average offset */
                double avg_offset = 0;
                for (int i = 0; i < gps_count; i++) {
                    avg_offset += gps_offsets[i];
                }
                avg_offset /= gps_count;
                
                printf("\n");
                printf("GPS readings: %d\n", gps_count);
                printf("Avg PC-GPS offset: %.1f ms (after %dms latency compensation)\n", 
                       avg_offset, GPS_SERIAL_LATENCY_MS);
                
                /* Now we can estimate the true accuracy of the recording timestamp */
                printf("\n");
                printf("===========================================\n");
                printf("ACCURACY ANALYSIS\n");
                printf("===========================================\n");
                
                /* The recording timestamp may have been taken from PC clock */
                /* If PC is ahead of GPS by avg_offset, then true recording start was: */
                /* rec_start_ts - (avg_offset / 1000) */
                
                double corrected_rec_start = rec_start_ts - (avg_offset / 1000.0);
                double corrected_minute_boundary = corrected_rec_start + (wwv_offset_ms / 1000.0);
                
                timestamp_to_string(corrected_minute_boundary, buf, sizeof(buf));
                printf("GPS-corrected minute boundary: %s\n", buf);
                
                double corrected_sec_into_minute = fmod(corrected_minute_boundary, 60.0);
                if (corrected_sec_into_minute > 30) {
                    corrected_sec_into_minute -= 60.0;
                }
                
                printf("Corrected alignment error: %.1f ms\n", corrected_sec_into_minute * 1000.0);
                
                if (fabs(corrected_sec_into_minute * 1000.0) < 500) {
                    printf("\n>>> WWV TIMING VERIFIED - within %.0f ms of GPS <<<\n",
                           fabs(corrected_sec_into_minute * 1000.0));
                } else {
                    printf("\nWARNING: Large discrepancy between WWV and GPS timing\n");
                    printf("Possible causes:\n");
                    printf("  - Recording timestamp may be inaccurate\n");
                    printf("  - WWV detection may have locked onto wrong feature\n");
                    printf("  - HF propagation delay (~1-3ms per 1000km)\n");
                }
            } else {
                printf("\nFailed to get GPS fix. Check:\n");
                printf("  - GPS antenna has clear sky view\n");
                printf("  - Correct COM port specified\n");
            }
        } else {
            printf("Failed to open GPS port.\n");
        }
    } else {
        printf("\n");
        printf("(Use -p <port> to compare with live GPS reference)\n");
    }
    
    printf("\n===========================================\n");
    
    return 0;
}
