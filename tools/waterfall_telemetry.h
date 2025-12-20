/**
 * @file waterfall_telemetry.h
 * @brief UDP broadcast telemetry for waterfall statistics
 *
 * Non-blocking UDP broadcast of CSV-format telemetry data.
 * Enables remote monitoring without affecting waterfall performance.
 *
 * Usage:
 *   telem_init(3005);                    // Initialize on port 3005
 *   telem_enable(TELEM_CHANNEL | TELEM_TICKS);  // Enable channels
 *   telem_send(TELEM_CHANNEL, csv_line); // Send when data ready
 *   telem_cleanup();                     // Shutdown
 */

#ifndef WATERFALL_TELEMETRY_H
#define WATERFALL_TELEMETRY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

#define TELEM_DEFAULT_PORT      3005
#define TELEM_MAX_MESSAGE_LEN   512

/*============================================================================
 * Telemetry Channels (bitmask)
 *============================================================================*/

typedef enum {
    TELEM_NONE      = 0,
    TELEM_CHANNEL   = (1 << 0),  /* Channel quality: carrier_db, snr_db, noise_db */
    TELEM_TICKS     = (1 << 1),  /* Tick pulse events */
    TELEM_MARKERS   = (1 << 2),  /* Minute marker events */
    TELEM_CARRIER   = (1 << 3),  /* Carrier frequency tracking (DC) */
    TELEM_SYNC      = (1 << 4),  /* Sync state changes */
    TELEM_SUBCAR    = (1 << 5),  /* Subcarrier (500/600 Hz) analysis */
    TELEM_CORR      = (1 << 6),  /* Tick correlation data */
    TELEM_TONE500   = (1 << 7),  /* 500 Hz tone tracker */
    TELEM_TONE600   = (1 << 8),  /* 600 Hz tone tracker */
    TELEM_BCD_ENV   = (1 << 9),  /* DEPRECATED: 100 Hz BCD envelope tracker */
    TELEM_BCDS      = (1 << 10), /* BCD decoder symbols and time */
    TELEM_CONSOLE   = (1 << 11), /* Console/status messages (buffered) */
    TELEM_CTRL      = (1 << 12), /* Control commands received (from controller) */
    TELEM_RESP      = (1 << 13), /* Responses to control commands (to controller) */
    TELEM_ALL       = 0x3FFF     /* All channels */
} telem_channel_t;

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Initialize UDP telemetry broadcast
 * @param port  UDP port to broadcast on (use 0 for default 3005)
 * @return true on success, false on socket error
 */
bool telem_init(int port);

/**
 * Cleanup and close UDP socket
 */
void telem_cleanup(void);

/**
 * Enable telemetry channels
 * @param channels  Bitmask of channels to enable (ORed TELEM_* values)
 */
void telem_enable(uint32_t channels);

/**
 * Disable telemetry channels
 * @param channels  Bitmask of channels to disable
 */
void telem_disable(uint32_t channels);

/**
 * Set enabled channels (replaces current mask)
 * @param channels  Bitmask of channels to enable
 */
void telem_set_channels(uint32_t channels);

/**
 * Get currently enabled channels
 * @return Current channel bitmask
 */
uint32_t telem_get_channels(void);

/**
 * Check if a channel is enabled
 * @param channel  Single channel to check
 * @return true if enabled
 */
bool telem_is_enabled(telem_channel_t channel);

/**
 * Send telemetry message (non-blocking, fire-and-forget)
 *
 * Message is only sent if the channel is enabled.
 * Format: "PREFIX,csv_data\n"
 *
 * @param channel   Which channel this data belongs to
 * @param csv_line  CSV-formatted data (without prefix, with or without newline)
 */
void telem_send(telem_channel_t channel, const char *csv_line);

/**
 * Send formatted telemetry message (printf-style)
 * @param channel  Which channel this data belongs to
 * @param fmt      Printf format string
 * @param ...      Format arguments
 */
void telem_sendf(telem_channel_t channel, const char *fmt, ...);

/**
 * Get channel prefix string for message formatting
 * @param channel  Channel enum value
 * @return 4-character prefix string (e.g., "CHAN", "TICK")
 */
const char *telem_channel_prefix(telem_channel_t channel);

/**
 * Get statistics
 * @param sent     Receives count of messages sent (can be NULL)
 * @param dropped  Receives count of messages dropped/filtered (can be NULL)
 */
void telem_get_stats(uint32_t *sent, uint32_t *dropped);

/**
 * Send console message (buffered for hot-path performance)
 *
 * Messages are buffered and automatically flushed on newline or buffer full.
 * Call telem_console_flush() periodically to ensure messages are sent.
 *
 * @param fmt  Printf format string
 * @param ...  Format arguments
 */
void telem_console(const char *fmt, ...);

/**
 * Manually flush console buffer
 *
 * Call this at frame boundaries or periodically to drain buffered console messages.
 * Automatic flush occurs on newline or buffer full.
 */
void telem_console_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* WATERFALL_TELEMETRY_H */
