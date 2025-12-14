/**
 * @file tcp_server.h
 * @brief Phoenix SDR TCP Control Server
 *
 * Text-based TCP protocol for remote SDR control.
 * See docs/SDR_TCP_CONTROL_INTERFACE.md for protocol specification.
 */

#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Constants
 *============================================================================*/

#define TCP_DEFAULT_PORT        4535
#define TCP_MAX_LINE_LENGTH     256
#define TCP_MAX_ARGS            8

/*============================================================================
 * Command Types
 *============================================================================*/

typedef enum {
    CMD_UNKNOWN = 0,
    
    /* Frequency */
    CMD_SET_FREQ,
    CMD_GET_FREQ,
    
    /* Gain */
    CMD_SET_GAIN,
    CMD_GET_GAIN,
    CMD_SET_LNA,
    CMD_GET_LNA,
    CMD_SET_AGC,
    CMD_GET_AGC,
    
    /* Sample Rate / Bandwidth */
    CMD_SET_SRATE,
    CMD_GET_SRATE,
    CMD_SET_BW,
    CMD_GET_BW,
    
    /* Hardware */
    CMD_SET_ANTENNA,
    CMD_GET_ANTENNA,
    CMD_SET_BIAST,
    CMD_SET_NOTCH,
    
    /* Streaming */
    CMD_START,
    CMD_STOP,
    CMD_STATUS,
    
    /* Utility */
    CMD_PING,
    CMD_VER,
    CMD_CAPS,
    CMD_HELP,
    CMD_QUIT,
    
    CMD_COUNT  /* Number of commands */
} tcp_cmd_type_t;

/*============================================================================
 * Error Codes
 *============================================================================*/

typedef enum {
    TCP_OK = 0,
    TCP_ERR_SYNTAX,     /* Malformed command */
    TCP_ERR_UNKNOWN,    /* Unknown command */
    TCP_ERR_PARAM,      /* Invalid parameter value */
    TCP_ERR_RANGE,      /* Value out of valid range */
    TCP_ERR_STATE,      /* Invalid state for operation */
    TCP_ERR_BUSY,       /* SDR busy / another client */
    TCP_ERR_HARDWARE,   /* SDR hardware error */
    TCP_ERR_TIMEOUT     /* Operation timed out */
} tcp_error_t;

/*============================================================================
 * Parsed Command Structure
 *============================================================================*/

typedef struct {
    tcp_cmd_type_t type;
    int            argc;                       /* Number of arguments */
    char           argv[TCP_MAX_ARGS][64];     /* Argument strings */
    
    /* Parsed values (filled by parse_command_args) */
    union {
        double      freq_hz;
        int         gain_db;
        int         lna_state;
        int         sample_rate;
        int         bandwidth_khz;
        bool        on_off;
        struct {
            char    mode[16];
        } agc;
        struct {
            char    port[8];
        } antenna;
    } value;
} tcp_command_t;

/*============================================================================
 * Response Structure
 *============================================================================*/

typedef struct {
    tcp_error_t     error;
    char            message[TCP_MAX_LINE_LENGTH];
} tcp_response_t;

/*============================================================================
 * SDR State (for commands to query/modify)
 *============================================================================*/

typedef struct {
    bool        streaming;
    double      freq_hz;
    int         gain_reduction;
    int         lna_state;
    char        agc_mode[16];
    int         sample_rate;
    int         bandwidth_khz;
    char        antenna[8];
    bool        bias_t;
    bool        notch;
    bool        overload;
} tcp_sdr_state_t;

/*============================================================================
 * Command Parsing Functions
 *============================================================================*/

/**
 * @brief Parse a command line into structured command
 * 
 * @param line      Input line (null-terminated, may include \n)
 * @param cmd       Output command structure
 * @return          TCP_OK on success, error code on failure
 */
tcp_error_t tcp_parse_command(const char *line, tcp_command_t *cmd);

/**
 * @brief Get command name from type
 */
const char* tcp_cmd_name(tcp_cmd_type_t type);

/**
 * @brief Get error code string
 */
const char* tcp_error_name(tcp_error_t err);

/*============================================================================
 * Command Execution Functions
 *============================================================================*/

/**
 * @brief Execute a parsed command
 * 
 * @param cmd       Parsed command
 * @param state     SDR state (queried/modified)
 * @param response  Output response
 * @return          TCP_OK on success
 */
tcp_error_t tcp_execute_command(
    const tcp_command_t *cmd,
    tcp_sdr_state_t *state,
    tcp_response_t *response
);

/*============================================================================
 * Response Formatting Functions
 *============================================================================*/

/**
 * @brief Format a success response
 */
void tcp_response_ok(tcp_response_t *resp, const char *value);

/**
 * @brief Format an error response
 */
void tcp_response_error(tcp_response_t *resp, tcp_error_t err, const char *msg);

/**
 * @brief Format response as string for sending
 * 
 * @param resp      Response structure
 * @param buf       Output buffer
 * @param buf_size  Size of output buffer
 * @return          Number of bytes written
 */
int tcp_format_response(const tcp_response_t *resp, char *buf, size_t buf_size);

/*============================================================================
 * Server Functions (implemented in sdr_server.c)
 *============================================================================*/

/**
 * @brief Initialize default SDR state
 */
void tcp_state_defaults(tcp_sdr_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* TCP_SERVER_H */
