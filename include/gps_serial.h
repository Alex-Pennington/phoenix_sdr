/**
 * @file gps_serial.h
 * @brief GPS serial interface for timing
 * 
 * Reads GPS time from Arduino NEO-6M on COM port.
 * Provides UTC time with PC offset calculation.
 */

#ifndef GPS_SERIAL_H
#define GPS_SERIAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#endif

/**
 * GPS time reading
 */
typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int millisecond;
    
    bool valid;              /* GPS has fix */
    int satellites;          /* Number of satellites */
    unsigned long nmea_pulse; /* NMEA message count (for detecting new second) */
    
    double unix_time;        /* Unix timestamp (seconds.milliseconds) */
    double pc_offset_ms;     /* PC time - GPS time in milliseconds */
    
    char iso_string[32];     /* ISO 8601 formatted string */
} gps_reading_t;

/**
 * GPS serial context
 */
typedef struct {
#ifdef _WIN32
    HANDLE hSerial;
    LARGE_INTEGER pc_freq;
#else
    int fd;
#endif
    char port[32];
    bool connected;
    unsigned long last_pulse;
    
    /* Latency compensation (measured serial delay) */
    double latency_ms;
} gps_context_t;

/**
 * Open GPS serial connection
 * 
 * @param ctx       GPS context to initialize
 * @param port      COM port name (e.g., "COM6")
 * @param baud_rate Baud rate (default 115200)
 * @return 0 on success, -1 on error
 */
int gps_open(gps_context_t *ctx, const char *port, int baud_rate);

/**
 * Close GPS serial connection
 */
void gps_close(gps_context_t *ctx);

/**
 * Check if GPS is connected
 */
bool gps_is_connected(const gps_context_t *ctx);

/**
 * Read current GPS time (blocking, with timeout)
 * 
 * @param ctx       GPS context
 * @param reading   Receives GPS time data
 * @param timeout_ms Maximum time to wait for valid reading
 * @return 0 on success, -1 on timeout/error
 */
int gps_read_time(gps_context_t *ctx, gps_reading_t *reading, int timeout_ms);

/**
 * Wait for next GPS second boundary
 * Useful for synchronizing recording start
 * 
 * @param ctx       GPS context
 * @param reading   Receives GPS time at second boundary
 * @param timeout_ms Maximum time to wait
 * @return 0 on success, -1 on timeout/error
 */
int gps_wait_second(gps_context_t *ctx, gps_reading_t *reading, int timeout_ms);

/**
 * Set latency compensation (serial delay)
 * Default is 302ms based on measured NEO-6M delay
 */
void gps_set_latency(gps_context_t *ctx, double latency_ms);

/**
 * Get current latency compensation value
 */
double gps_get_latency(const gps_context_t *ctx);

/**
 * Format GPS reading as string for display
 */
void gps_format_reading(const gps_reading_t *reading, char *buffer, size_t len);

#endif /* GPS_SERIAL_H */
