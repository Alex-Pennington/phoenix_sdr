/**
 * @file tcp_commands.c
 * @brief TCP Command Parser and Executor
 *
 * Parses text commands and executes them against SDR state.
 */

#include "tcp_server.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*============================================================================
 * Command Name Table
 *============================================================================*/

typedef struct {
    const char     *name;
    tcp_cmd_type_t  type;
    int             min_args;
    int             max_args;
} cmd_def_t;

static const cmd_def_t g_commands[] = {
    /* Frequency */
    { "SET_FREQ",    CMD_SET_FREQ,    1, 1 },
    { "GET_FREQ",    CMD_GET_FREQ,    0, 0 },

    /* Gain */
    { "SET_GAIN",    CMD_SET_GAIN,    1, 1 },
    { "GET_GAIN",    CMD_GET_GAIN,    0, 0 },
    { "SET_LNA",     CMD_SET_LNA,     1, 1 },
    { "GET_LNA",     CMD_GET_LNA,     0, 0 },
    { "SET_AGC",     CMD_SET_AGC,     1, 1 },
    { "GET_AGC",     CMD_GET_AGC,     0, 0 },

    /* Sample Rate / Bandwidth */
    { "SET_SRATE",   CMD_SET_SRATE,   1, 1 },
    { "GET_SRATE",   CMD_GET_SRATE,   0, 0 },
    { "SET_BW",      CMD_SET_BW,      1, 1 },
    { "GET_BW",      CMD_GET_BW,      0, 0 },

    /* Hardware */
    { "SET_ANTENNA", CMD_SET_ANTENNA, 1, 1 },
    { "GET_ANTENNA", CMD_GET_ANTENNA, 0, 0 },
    { "SET_BIAST",   CMD_SET_BIAST,   1, 2 },  /* ON [CONFIRM] */
    { "SET_NOTCH",   CMD_SET_NOTCH,   1, 1 },
    { "SET_DECIM",   CMD_SET_DECIM,   1, 1 },
    { "GET_DECIM",   CMD_GET_DECIM,   0, 0 },
    { "SET_IFMODE",  CMD_SET_IFMODE,  1, 1 },
    { "GET_IFMODE",  CMD_GET_IFMODE,  0, 0 },
    { "SET_DCOFFSET", CMD_SET_DCOFFSET, 1, 1 },
    { "GET_DCOFFSET", CMD_GET_DCOFFSET, 0, 0 },
    { "SET_IQCORR",  CMD_SET_IQCORR,  1, 1 },
    { "GET_IQCORR",  CMD_GET_IQCORR,  0, 0 },
    { "SET_AGC_SETPOINT", CMD_SET_AGC_SETPOINT, 1, 1 },
    { "GET_AGC_SETPOINT", CMD_GET_AGC_SETPOINT, 0, 0 },

    /* Streaming */
    { "START",       CMD_START,       0, 0 },
    { "STOP",        CMD_STOP,        0, 0 },
    { "STATUS",      CMD_STATUS,      0, 0 },

    /* Utility */
    { "PING",        CMD_PING,        0, 0 },
    { "VER",         CMD_VER,         0, 0 },
    { "CAPS",        CMD_CAPS,        0, 0 },
    { "HELP",        CMD_HELP,        0, 0 },
    { "QUIT",        CMD_QUIT,        0, 0 },

    { NULL,          CMD_UNKNOWN,     0, 0 }   /* Sentinel */
};

/*============================================================================
 * Helper: Case-insensitive string compare
 *============================================================================*/

static int strcasecmp_local(const char *a, const char *b) {
    while (*a && *b) {
        int ca = toupper((unsigned char)*a);
        int cb = toupper((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}

/*============================================================================
 * Command Name Lookup
 *============================================================================*/

const char* tcp_cmd_name(tcp_cmd_type_t type) {
    for (int i = 0; g_commands[i].name; i++) {
        if (g_commands[i].type == type) {
            return g_commands[i].name;
        }
    }
    return "UNKNOWN";
}

/*============================================================================
 * Error Name Lookup
 *============================================================================*/

const char* tcp_error_name(tcp_error_t err) {
    switch (err) {
        case TCP_OK:           return "OK";
        case TCP_ERR_SYNTAX:   return "SYNTAX";
        case TCP_ERR_UNKNOWN:  return "UNKNOWN";
        case TCP_ERR_PARAM:    return "PARAM";
        case TCP_ERR_RANGE:    return "RANGE";
        case TCP_ERR_STATE:    return "STATE";
        case TCP_ERR_BUSY:     return "BUSY";
        case TCP_ERR_HARDWARE: return "HARDWARE";
        case TCP_ERR_TIMEOUT:  return "TIMEOUT";
        default:               return "ERROR";
    }
}

/*============================================================================
 * Command Parser
 *============================================================================*/

tcp_error_t tcp_parse_command(const char *line, tcp_command_t *cmd) {
    if (!line || !cmd) {
        return TCP_ERR_SYNTAX;
    }

    /* Initialize command structure */
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = CMD_UNKNOWN;

    /* Skip leading whitespace */
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }

    /* Empty line */
    if (*line == '\0' || *line == '\n' || *line == '\r') {
        return TCP_ERR_SYNTAX;
    }

    /* Tokenize: first token is command name */
    char linebuf[TCP_MAX_LINE_LENGTH];
    strncpy(linebuf, line, sizeof(linebuf) - 1);
    linebuf[sizeof(linebuf) - 1] = '\0';

    /* Remove trailing newline/carriage return */
    size_t len = strlen(linebuf);
    while (len > 0 && (linebuf[len-1] == '\n' || linebuf[len-1] == '\r')) {
        linebuf[--len] = '\0';
    }

    /* Parse tokens */
    char *saveptr = NULL;
    char *token = strtok_r(linebuf, " \t", &saveptr);

    if (!token) {
        return TCP_ERR_SYNTAX;
    }

    /* Look up command */
    const cmd_def_t *def = NULL;
    for (int i = 0; g_commands[i].name; i++) {
        if (strcasecmp_local(token, g_commands[i].name) == 0) {
            def = &g_commands[i];
            cmd->type = def->type;
            break;
        }
    }

    if (!def) {
        return TCP_ERR_UNKNOWN;
    }

    /* Parse arguments */
    cmd->argc = 0;
    while ((token = strtok_r(NULL, " \t", &saveptr)) != NULL) {
        if (cmd->argc >= TCP_MAX_ARGS) {
            return TCP_ERR_SYNTAX;  /* Too many arguments */
        }
        strncpy(cmd->argv[cmd->argc], token, 63);
        cmd->argv[cmd->argc][63] = '\0';
        cmd->argc++;
    }

    /* Check argument count */
    if (cmd->argc < def->min_args) {
        return TCP_ERR_SYNTAX;  /* Missing arguments */
    }
    if (cmd->argc > def->max_args) {
        return TCP_ERR_SYNTAX;  /* Too many arguments */
    }

    /* Parse type-specific values */
    switch (cmd->type) {
        case CMD_SET_FREQ:
            cmd->value.freq_hz = atof(cmd->argv[0]);
            if (cmd->value.freq_hz < 1000 || cmd->value.freq_hz > 2000000000) {
                return TCP_ERR_RANGE;
            }
            break;

        case CMD_SET_GAIN:
            cmd->value.gain_db = atoi(cmd->argv[0]);
            if (cmd->value.gain_db < 20 || cmd->value.gain_db > 59) {
                return TCP_ERR_RANGE;
            }
            break;

        case CMD_SET_LNA:
            cmd->value.lna_state = atoi(cmd->argv[0]);
            /* Range check done in execute based on antenna port */
            if (cmd->value.lna_state < 0 || cmd->value.lna_state > 8) {
                return TCP_ERR_RANGE;
            }
            break;

        case CMD_SET_AGC:
            strncpy(cmd->value.agc.mode, cmd->argv[0], 15);
            cmd->value.agc.mode[15] = '\0';
            /* Convert to uppercase for comparison */
            for (char *p = cmd->value.agc.mode; *p; p++) {
                *p = toupper((unsigned char)*p);
            }
            if (strcmp(cmd->value.agc.mode, "OFF") != 0 &&
                strcmp(cmd->value.agc.mode, "5HZ") != 0 &&
                strcmp(cmd->value.agc.mode, "50HZ") != 0 &&
                strcmp(cmd->value.agc.mode, "100HZ") != 0) {
                return TCP_ERR_PARAM;
            }
            break;

        case CMD_SET_SRATE:
            cmd->value.sample_rate = atoi(cmd->argv[0]);
            if (cmd->value.sample_rate < 2000000 || cmd->value.sample_rate > 10000000) {
                return TCP_ERR_RANGE;
            }
            break;

        case CMD_SET_BW:
            cmd->value.bandwidth_khz = atoi(cmd->argv[0]);
            /* Valid bandwidths: 200, 300, 600, 1536, 5000, 6000, 7000, 8000 */
            if (cmd->value.bandwidth_khz != 200 &&
                cmd->value.bandwidth_khz != 300 &&
                cmd->value.bandwidth_khz != 600 &&
                cmd->value.bandwidth_khz != 1536 &&
                cmd->value.bandwidth_khz != 5000 &&
                cmd->value.bandwidth_khz != 6000 &&
                cmd->value.bandwidth_khz != 7000 &&
                cmd->value.bandwidth_khz != 8000) {
                return TCP_ERR_PARAM;
            }
            break;

        case CMD_SET_ANTENNA:
            strncpy(cmd->value.antenna.port, cmd->argv[0], 7);
            cmd->value.antenna.port[7] = '\0';
            for (char *p = cmd->value.antenna.port; *p; p++) {
                *p = toupper((unsigned char)*p);
            }
            if (strcmp(cmd->value.antenna.port, "A") != 0 &&
                strcmp(cmd->value.antenna.port, "B") != 0 &&
                strcmp(cmd->value.antenna.port, "HIZ") != 0) {
                return TCP_ERR_PARAM;
            }
            break;

        case CMD_SET_BIAST:
        case CMD_SET_NOTCH:
            /* Parse ON/OFF */
            for (char *p = cmd->argv[0]; *p; p++) {
                *p = toupper((unsigned char)*p);
            }
            if (strcmp(cmd->argv[0], "ON") == 0) {
                cmd->value.on_off = true;
            } else if (strcmp(cmd->argv[0], "OFF") == 0) {
                cmd->value.on_off = false;
            } else {
                return TCP_ERR_PARAM;
            }
            break;

        case CMD_SET_DCOFFSET:
        case CMD_SET_IQCORR:
            /* Parse ON/OFF */
            for (char *p = cmd->argv[0]; *p; p++) {
                *p = toupper((unsigned char)*p);
            }
            if (strcmp(cmd->argv[0], "ON") == 0) {
                cmd->value.on_off = true;
            } else if (strcmp(cmd->argv[0], "OFF") == 0) {
                cmd->value.on_off = false;
            } else {
                return TCP_ERR_PARAM;
            }
            break;

        case CMD_SET_DECIM:
            cmd->value.decimation = atoi(cmd->argv[0]);
            /* Valid decimation factors: 1, 2, 4, 8, 16, 32 */
            if (cmd->value.decimation != 1 &&
                cmd->value.decimation != 2 &&
                cmd->value.decimation != 4 &&
                cmd->value.decimation != 8 &&
                cmd->value.decimation != 16 &&
                cmd->value.decimation != 32) {
                return TCP_ERR_PARAM;
            }
            break;

        case CMD_SET_IFMODE:
            strncpy(cmd->value.if_mode, cmd->argv[0], 15);
            cmd->value.if_mode[15] = '\0';
            for (char *p = cmd->value.if_mode; *p; p++) {
                *p = toupper((unsigned char)*p);
            }
            if (strcmp(cmd->value.if_mode, "ZERO") != 0 &&
                strcmp(cmd->value.if_mode, "LOW") != 0) {
                return TCP_ERR_PARAM;
            }
            break;

        case CMD_SET_AGC_SETPOINT:
            cmd->value.agc_setpoint = atoi(cmd->argv[0]);
            if (cmd->value.agc_setpoint < -72 || cmd->value.agc_setpoint > 0) {
                return TCP_ERR_RANGE;
            }
            break;

        default:
            break;
    }

    return TCP_OK;
}

/*============================================================================
 * Response Formatting
 *============================================================================*/

void tcp_response_ok(tcp_response_t *resp, const char *value) {
    resp->error = TCP_OK;
    if (value && *value) {
        snprintf(resp->message, sizeof(resp->message), "OK %s", value);
    } else {
        strcpy(resp->message, "OK");
    }
}

void tcp_response_error(tcp_response_t *resp, tcp_error_t err, const char *msg) {
    resp->error = err;
    if (msg && *msg) {
        snprintf(resp->message, sizeof(resp->message), "ERR %s %s",
                 tcp_error_name(err), msg);
    } else {
        snprintf(resp->message, sizeof(resp->message), "ERR %s",
                 tcp_error_name(err));
    }
}

int tcp_format_response(const tcp_response_t *resp, char *buf, size_t buf_size) {
    return snprintf(buf, buf_size, "%s\n", resp->message);
}

/*============================================================================
 * State Defaults
 *============================================================================*/

void tcp_state_defaults(tcp_sdr_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->sdr_ctx = NULL;
    state->hardware_connected = false;
    state->streaming = false;
    state->freq_hz = 15000000.0;  /* 15 MHz */
    state->gain_reduction = 40;
    state->lna_state = 4;
    strcpy(state->agc_mode, "OFF");
    state->sample_rate = 2000000;
    state->bandwidth_khz = 200;
    strcpy(state->antenna, "A");
    state->bias_t = false;
    state->notch = false;
    state->overload = false;

    /* Advanced settings */
    state->decimation = 1;
    strcpy(state->if_mode, "ZERO");
    state->dc_offset_corr = true;
    state->iq_imbalance_corr = true;
    state->agc_setpoint = -60;

    /* Initialize psdr_config with defaults */
    psdr_config_defaults(&state->sdr_config);
}

/*============================================================================
 * Helper: Sync state to psdr_config_t
 *============================================================================*/

static void sync_state_to_config(tcp_sdr_state_t *state) {
    psdr_config_t *cfg = &state->sdr_config;

    cfg->freq_hz = state->freq_hz;
    cfg->sample_rate_hz = (double)state->sample_rate;
    cfg->gain_reduction = state->gain_reduction;
    cfg->lna_state = state->lna_state;

    /* Map bandwidth */
    cfg->bandwidth = (psdr_bandwidth_t)state->bandwidth_khz;

    /* Map antenna */
    if (strcmp(state->antenna, "A") == 0) {
        cfg->antenna = PSDR_ANT_A;
    } else if (strcmp(state->antenna, "B") == 0) {
        cfg->antenna = PSDR_ANT_B;
    } else if (strcmp(state->antenna, "HIZ") == 0) {
        cfg->antenna = PSDR_ANT_HIZ;
    }

    /* Map AGC mode */
    if (strcmp(state->agc_mode, "OFF") == 0) {
        cfg->agc_mode = PSDR_AGC_DISABLED;
    } else if (strcmp(state->agc_mode, "5HZ") == 0) {
        cfg->agc_mode = PSDR_AGC_5HZ;
    } else if (strcmp(state->agc_mode, "50HZ") == 0) {
        cfg->agc_mode = PSDR_AGC_50HZ;
    } else if (strcmp(state->agc_mode, "100HZ") == 0) {
        cfg->agc_mode = PSDR_AGC_100HZ;
    }

    cfg->bias_t = state->bias_t;
    cfg->rf_notch = state->notch;

    /* New advanced settings */
    cfg->decimation = state->decimation;
    cfg->agc_setpoint_dbfs = state->agc_setpoint;
    cfg->dc_offset_corr = state->dc_offset_corr;
    cfg->iq_imbalance_corr = state->iq_imbalance_corr;

    /* Map IF mode */
    if (strcmp(state->if_mode, "ZERO") == 0) {
        cfg->if_mode = PSDR_IF_ZERO;
    } else if (strcmp(state->if_mode, "LOW") == 0) {
        cfg->if_mode = PSDR_IF_LOW;
    }
}

/*============================================================================
 * Helper: Apply config to hardware (update while streaming or configure)
 *============================================================================*/

static tcp_error_t apply_config_to_hardware(tcp_sdr_state_t *state) {
    if (!state->hardware_connected || !state->sdr_ctx) {
        return TCP_OK;  /* No hardware, just update state */
    }

    sync_state_to_config(state);

    psdr_error_t err;
    if (state->streaming) {
        /* Use update for live changes */
        err = psdr_update(state->sdr_ctx, &state->sdr_config);
    } else {
        /* Use configure for stopped SDR */
        err = psdr_configure(state->sdr_ctx, &state->sdr_config);
    }

    if (err != PSDR_OK) {
        return TCP_ERR_HARDWARE;
    }

    return TCP_OK;
}

/*============================================================================
 * Command Execution
 *============================================================================*/

tcp_error_t tcp_execute_command(
    const tcp_command_t *cmd,
    tcp_sdr_state_t *state,
    tcp_response_t *response
) {
    char buf[128];
    tcp_error_t hw_err;

    switch (cmd->type) {
        /* ----- Frequency ----- */
        case CMD_SET_FREQ: {
            double old_freq = state->freq_hz;
            state->freq_hz = cmd->value.freq_hz;
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                state->freq_hz = old_freq;  /* Restore on failure */
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;
        }

        case CMD_GET_FREQ:
            snprintf(buf, sizeof(buf), "%.0f", state->freq_hz);
            tcp_response_ok(response, buf);
            break;

        /* ----- Gain ----- */
        case CMD_SET_GAIN: {
            int old_gain = state->gain_reduction;
            state->gain_reduction = cmd->value.gain_db;
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                state->gain_reduction = old_gain;  /* Restore on failure */
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;
        }

        case CMD_GET_GAIN:
            snprintf(buf, sizeof(buf), "%d", state->gain_reduction);
            tcp_response_ok(response, buf);
            break;

        case CMD_SET_LNA: {
            /* Hi-Z antenna only supports LNA states 0-4 */
            int max_lna = (strcmp(state->antenna, "HIZ") == 0) ? 4 : 8;
            if (cmd->value.lna_state > max_lna) {
                char errmsg[64];
                snprintf(errmsg, sizeof(errmsg), "LNA must be 0-%d for %s antenna",
                         max_lna, state->antenna);
                tcp_response_error(response, TCP_ERR_RANGE, errmsg);
                return TCP_ERR_RANGE;
            }
            int old_lna = state->lna_state;
            state->lna_state = cmd->value.lna_state;
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                state->lna_state = old_lna;  /* Restore on failure */
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;
        }

        case CMD_GET_LNA:
            snprintf(buf, sizeof(buf), "%d", state->lna_state);
            tcp_response_ok(response, buf);
            break;

        case CMD_SET_AGC: {
            char old_agc[16];
            strncpy(old_agc, state->agc_mode, sizeof(old_agc) - 1);
            old_agc[15] = '\0';
            strncpy(state->agc_mode, cmd->value.agc.mode, sizeof(state->agc_mode) - 1);
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                strncpy(state->agc_mode, old_agc, sizeof(state->agc_mode) - 1);  /* Restore */
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;
        }

        case CMD_GET_AGC:
            tcp_response_ok(response, state->agc_mode);
            break;

        /* ----- Sample Rate / Bandwidth ----- */
        case CMD_SET_SRATE: {
            if (state->streaming) {
                tcp_response_error(response, TCP_ERR_STATE, "stop streaming first");
                return TCP_ERR_STATE;
            }
            int old_srate = state->sample_rate;
            state->sample_rate = cmd->value.sample_rate;
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                state->sample_rate = old_srate;  /* Restore */
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;
        }

        case CMD_GET_SRATE:
            /* Return actual sample rate from hardware if available */
            if (state->hardware_connected && state->sdr_ctx) {
                double actual = psdr_get_sample_rate(state->sdr_ctx);
                if (actual > 0) {
                    snprintf(buf, sizeof(buf), "%.0f", actual);
                } else {
                    snprintf(buf, sizeof(buf), "%d", state->sample_rate);
                }
            } else {
                snprintf(buf, sizeof(buf), "%d", state->sample_rate);
            }
            tcp_response_ok(response, buf);
            break;

        case CMD_SET_BW: {
            if (state->streaming) {
                tcp_response_error(response, TCP_ERR_STATE, "stop streaming first");
                return TCP_ERR_STATE;
            }
            int old_bw = state->bandwidth_khz;
            state->bandwidth_khz = cmd->value.bandwidth_khz;
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                state->bandwidth_khz = old_bw;  /* Restore */
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;
        }

        case CMD_GET_BW:
            snprintf(buf, sizeof(buf), "%d", state->bandwidth_khz);
            tcp_response_ok(response, buf);
            break;

        /* ----- Hardware ----- */
        case CMD_SET_ANTENNA: {
            if (state->streaming) {
                tcp_response_error(response, TCP_ERR_STATE, "stop streaming first");
                return TCP_ERR_STATE;
            }
            char old_antenna[8];
            strncpy(old_antenna, state->antenna, sizeof(old_antenna) - 1);
            old_antenna[7] = '\0';
            strncpy(state->antenna, cmd->value.antenna.port, sizeof(state->antenna) - 1);

            /* If switching to Hi-Z, clamp LNA state to valid range (0-4) */
            int old_lna = state->lna_state;
            if (strcmp(state->antenna, "HIZ") == 0 && state->lna_state > 4) {
                state->lna_state = 4;
            }

            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                strncpy(state->antenna, old_antenna, sizeof(state->antenna) - 1);  /* Restore */
                state->lna_state = old_lna;
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;
        }

        case CMD_GET_ANTENNA:
            tcp_response_ok(response, state->antenna);
            break;

        case CMD_SET_BIAST: {
            if (cmd->value.on_off) {
                /* Require CONFIRM for safety */
                if (cmd->argc < 2 || strcasecmp_local(cmd->argv[1], "CONFIRM") != 0) {
                    tcp_response_error(response, TCP_ERR_PARAM,
                        "use SET_BIAST ON CONFIRM (warning: may damage equipment)");
                    return TCP_ERR_PARAM;
                }
            }
            bool old_biast = state->bias_t;
            state->bias_t = cmd->value.on_off;
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                state->bias_t = old_biast;  /* Restore */
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;
        }

        case CMD_SET_NOTCH: {
            bool old_notch = state->notch;
            state->notch = cmd->value.on_off;
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                state->notch = old_notch;  /* Restore */
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;
        }

        /* ----- Advanced Hardware Settings ----- */
        case CMD_SET_DECIM:
            if (state->streaming) {
                tcp_response_error(response, TCP_ERR_STATE, "stop streaming first");
                return TCP_ERR_STATE;
            }
            state->decimation = cmd->value.decimation;
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;

        case CMD_GET_DECIM:
            snprintf(buf, sizeof(buf), "%d", state->decimation);
            tcp_response_ok(response, buf);
            break;

        case CMD_SET_IFMODE:
            if (state->streaming) {
                tcp_response_error(response, TCP_ERR_STATE, "stop streaming first");
                return TCP_ERR_STATE;
            }
            strncpy(state->if_mode, cmd->value.if_mode, sizeof(state->if_mode) - 1);
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;

        case CMD_GET_IFMODE:
            tcp_response_ok(response, state->if_mode);
            break;

        case CMD_SET_DCOFFSET:
            state->dc_offset_corr = cmd->value.on_off;
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;

        case CMD_GET_DCOFFSET:
            tcp_response_ok(response, state->dc_offset_corr ? "ON" : "OFF");
            break;

        case CMD_SET_IQCORR:
            state->iq_imbalance_corr = cmd->value.on_off;
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;

        case CMD_GET_IQCORR:
            tcp_response_ok(response, state->iq_imbalance_corr ? "ON" : "OFF");
            break;

        case CMD_SET_AGC_SETPOINT:
            state->agc_setpoint = cmd->value.agc_setpoint;
            hw_err = apply_config_to_hardware(state);
            if (hw_err != TCP_OK) {
                tcp_response_error(response, hw_err, "hardware update failed");
                return hw_err;
            }
            tcp_response_ok(response, NULL);
            break;

        case CMD_GET_AGC_SETPOINT:
            snprintf(buf, sizeof(buf), "%d", state->agc_setpoint);
            tcp_response_ok(response, buf);
            break;

        /* ----- Streaming ----- */
        case CMD_START:
            if (state->streaming) {
                tcp_response_error(response, TCP_ERR_STATE, "already streaming");
                return TCP_ERR_STATE;
            }

            /* Start actual SDR streaming if hardware is connected */
            if (state->hardware_connected && state->sdr_ctx) {
                /* Ensure config is up to date */
                sync_state_to_config(state);

                psdr_error_t err = psdr_start(state->sdr_ctx, &state->sdr_callbacks);
                if (err != PSDR_OK) {
                    tcp_response_error(response, TCP_ERR_HARDWARE, psdr_strerror(err));
                    return TCP_ERR_HARDWARE;
                }
            }

            state->streaming = true;
            tcp_response_ok(response, NULL);
            break;

        case CMD_STOP:
            if (!state->streaming) {
                tcp_response_error(response, TCP_ERR_STATE, "not streaming");
                return TCP_ERR_STATE;
            }

            /* Stop actual SDR streaming if hardware is connected */
            if (state->hardware_connected && state->sdr_ctx) {
                psdr_error_t err = psdr_stop(state->sdr_ctx);
                if (err != PSDR_OK) {
                    tcp_response_error(response, TCP_ERR_HARDWARE, psdr_strerror(err));
                    return TCP_ERR_HARDWARE;
                }
            }

            state->streaming = false;
            tcp_response_ok(response, NULL);
            break;

        case CMD_STATUS:
            /* Check actual streaming state from hardware if available */
            if (state->hardware_connected && state->sdr_ctx) {
                state->streaming = psdr_is_streaming(state->sdr_ctx);
            }

            snprintf(buf, sizeof(buf),
                "STREAMING=%d FREQ=%.0f GAIN=%d LNA=%d AGC=%s SRATE=%d BW=%d HW=%d",
                state->streaming ? 1 : 0,
                state->freq_hz,
                state->gain_reduction,
                state->lna_state,
                state->agc_mode,
                state->sample_rate,
                state->bandwidth_khz,
                state->hardware_connected ? 1 : 0
            );
            if (state->streaming) {
                char extra[32];
                snprintf(extra, sizeof(extra), " OVERLOAD=%d", state->overload ? 1 : 0);
                strncat(buf, extra, sizeof(buf) - strlen(buf) - 1);
            }
            tcp_response_ok(response, buf);
            break;

        /* ----- Utility ----- */
        case CMD_PING:
            strcpy(response->message, "PONG");
            response->error = TCP_OK;
            break;

        case CMD_VER:
            snprintf(buf, sizeof(buf), "PHOENIX_SDR=%s PROTOCOL=1.0", PHOENIX_VERSION_FULL);
            tcp_response_ok(response, buf);
            break;

        case CMD_CAPS:
            /* Multi-line response */
            snprintf(response->message, sizeof(response->message),
                "OK CAPS\n"
                "FREQ_MIN=1000\n"
                "FREQ_MAX=2000000000\n"
                "GAIN_MIN=20\n"
                "GAIN_MAX=59\n"
                "LNA_STATES=9\n"
                "SRATE_MIN=2000000\n"
                "SRATE_MAX=10000000\n"
                "BW=200,300,600,1536,5000,6000,7000,8000\n"
                "ANTENNA=A,B,HIZ\n"
                "AGC=OFF,5HZ,50HZ,100HZ\n"
                "END"
            );
            response->error = TCP_OK;
            break;

        case CMD_HELP:
            tcp_response_ok(response,
                "COMMANDS: SET_FREQ GET_FREQ SET_GAIN GET_GAIN SET_LNA GET_LNA "
                "SET_AGC GET_AGC SET_SRATE GET_SRATE SET_BW GET_BW SET_ANTENNA "
                "GET_ANTENNA SET_BIAST SET_NOTCH START STOP STATUS PING VER CAPS HELP QUIT"
            );
            break;

        case CMD_QUIT:
            strcpy(response->message, "BYE");
            response->error = TCP_OK;
            break;

        default:
            tcp_response_error(response, TCP_ERR_UNKNOWN, NULL);
            return TCP_ERR_UNKNOWN;
    }

    return TCP_OK;
}
