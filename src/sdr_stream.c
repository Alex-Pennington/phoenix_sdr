/**
 * @file sdr_stream.c
 * @brief SDRplay streaming - configuration, start, stop, callbacks
 */

#include "phoenix_sdr.h"
#include "sdrplay_api.h"
#include <stdio.h>
#include <string.h>

/* Context structure definition (shared with sdr_device.c) */
struct psdr_context {
    sdrplay_api_DeviceT         device;
    sdrplay_api_DeviceParamsT  *params;
    sdrplay_api_CallbackFnsT    api_callbacks;
    psdr_callbacks_t            user_callbacks;
    psdr_config_t               config;
    bool                        api_open;
    bool                        device_selected;
    bool                        streaming;
    double                      actual_sample_rate;
};

/* Thread-local context pointer for callbacks (single device support) */
static psdr_context_t *g_callback_ctx = NULL;

/*============================================================================
 * Internal Callbacks (SDRplay API -> User callbacks)
 *============================================================================*/

static void stream_callback_a(
    short *xi,
    short *xq,
    sdrplay_api_StreamCbParamsT *params,
    unsigned int numSamples,
    unsigned int reset,
    void *cbContext
) {
    (void)params;  /* Unused but required by API */
    psdr_context_t *ctx = (psdr_context_t *)cbContext;

    if (!ctx || !ctx->user_callbacks.on_samples) return;

    /* Forward to user callback */
    ctx->user_callbacks.on_samples(
        (const int16_t *)xi,
        (const int16_t *)xq,
        numSamples,
        (reset != 0),
        ctx->user_callbacks.user_ctx
    );
}

static void stream_callback_b(
    short *xi,
    short *xq,
    sdrplay_api_StreamCbParamsT *params,
    unsigned int numSamples,
    unsigned int reset,
    void *cbContext
) {
    /* RSP2 single tuner mode - Tuner B not used */
    (void)xi; (void)xq; (void)params;
    (void)numSamples; (void)reset; (void)cbContext;
}

static void event_callback(
    sdrplay_api_EventT eventId,
    sdrplay_api_TunerSelectT tuner,
    sdrplay_api_EventParamsT *params,
    void *cbContext
) {
    psdr_context_t *ctx = (psdr_context_t *)cbContext;

    if (!ctx) return;

    switch (eventId) {
        case sdrplay_api_GainChange:
            if (ctx->user_callbacks.on_gain_change) {
                ctx->user_callbacks.on_gain_change(
                    params->gainParams.currGain,
                    params->gainParams.lnaGRdB,
                    ctx->user_callbacks.user_ctx
                );
            }
            /* Verbose logging removed - user callback handles output */
            break;

        case sdrplay_api_PowerOverloadChange:
            if (ctx->user_callbacks.on_overload) {
                bool overloaded = (params->powerOverloadParams.powerOverloadChangeType
                                   == sdrplay_api_Overload_Detected);
                ctx->user_callbacks.on_overload(
                    overloaded,
                    ctx->user_callbacks.user_ctx
                );
            }
            /* Verbose logging removed - user callback handles output */

            /* CRITICAL: Must acknowledge overload event */
            sdrplay_api_Update(ctx->device.dev, tuner,
                               sdrplay_api_Update_Ctrl_OverloadMsgAck,
                               sdrplay_api_Update_Ext1_None);
            break;

        case sdrplay_api_DeviceRemoved:
            fprintf(stderr, "Event: Device removed!\n");
            ctx->streaming = false;
            break;

        default:
            printf("Event: Unknown event %d\n", eventId);
            break;
    }
}

/*============================================================================
 * Helper: Map our enums to SDRplay enums
 *============================================================================*/

static sdrplay_api_Bw_MHzT map_bandwidth(psdr_bandwidth_t bw) {
    switch (bw) {
        case PSDR_BW_200:  return sdrplay_api_BW_0_200;
        case PSDR_BW_300:  return sdrplay_api_BW_0_300;
        case PSDR_BW_600:  return sdrplay_api_BW_0_600;
        case PSDR_BW_1536: return sdrplay_api_BW_1_536;
        case PSDR_BW_5000: return sdrplay_api_BW_5_000;
        case PSDR_BW_6000: return sdrplay_api_BW_6_000;
        case PSDR_BW_7000: return sdrplay_api_BW_7_000;
        case PSDR_BW_8000: return sdrplay_api_BW_8_000;
        default:          return sdrplay_api_BW_0_200;
    }
}

static sdrplay_api_AgcControlT map_agc(psdr_agc_mode_t agc) {
    switch (agc) {
        case PSDR_AGC_DISABLED: return sdrplay_api_AGC_DISABLE;
        case PSDR_AGC_5HZ:      return sdrplay_api_AGC_5HZ;
        case PSDR_AGC_50HZ:     return sdrplay_api_AGC_50HZ;
        case PSDR_AGC_100HZ:    return sdrplay_api_AGC_100HZ;
        default:               return sdrplay_api_AGC_DISABLE;
    }
}

/*============================================================================
 * Configuration
 *============================================================================*/

psdr_error_t psdr_configure(psdr_context_t *ctx, const psdr_config_t *config) {
    if (!ctx || !config) return PSDR_ERR_INVALID_ARG;
    if (!ctx->device_selected) return PSDR_ERR_NOT_INITIALIZED;
    if (!ctx->params) return PSDR_ERR_DEVICE_PARAMS;

    /* Store config */
    memcpy(&ctx->config, config, sizeof(ctx->config));

    sdrplay_api_DeviceParamsT *dp = ctx->params;
    sdrplay_api_RxChannelParamsT *ch = dp->rxChannelA;

    /* Device-level parameters */
    if (dp->devParams) {
        dp->devParams->fsFreq.fsHz = config->sample_rate_hz;
        ctx->actual_sample_rate = config->sample_rate_hz;
    }

    /* Tuner parameters */
    if (ch) {
        ch->tunerParams.rfFreq.rfHz = config->freq_hz;
        ch->tunerParams.bwType = map_bandwidth(config->bandwidth);

        /* IF mode - Zero-IF or Low-IF */
        if (config->if_mode == PSDR_IF_LOW) {
            ch->tunerParams.ifType = sdrplay_api_IF_0_450;  /* 450 kHz IF */
        } else {
            ch->tunerParams.ifType = sdrplay_api_IF_Zero;   /* Zero-IF (signal at DC) */
        }
        ch->tunerParams.loMode = sdrplay_api_LO_Auto;

        ch->tunerParams.gain.gRdB = config->gain_reduction;
        ch->tunerParams.gain.LNAstate = (unsigned char)config->lna_state;

        /* Control parameters */
        ch->ctrlParams.agc.enable = map_agc(config->agc_mode);
        ch->ctrlParams.agc.setPoint_dBfs = config->agc_setpoint_dbfs;

        /* DC offset and IQ imbalance correction (configurable) */
        ch->ctrlParams.dcOffset.DCenable = config->dc_offset_corr ? 1 : 0;
        ch->ctrlParams.dcOffset.IQenable = config->iq_imbalance_corr ? 1 : 0;

        /* Decimation */
        if (config->decimation > 1) {
            ch->ctrlParams.decimation.enable = 1;
            ch->ctrlParams.decimation.decimationFactor = config->decimation;
            ch->ctrlParams.decimation.wideBandSignal = 0;
        } else {
            ch->ctrlParams.decimation.enable = 0;
        }

        /* RSP2-specific */
        const char *ant_name;
        switch (config->antenna) {
            case PSDR_ANT_A:
                ch->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A;
                ch->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_2;
                ant_name = "A";
                break;
            case PSDR_ANT_B:
                ch->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_B;
                ch->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_2;
                ant_name = "B";
                break;
            case PSDR_ANT_HIZ:
                ch->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A; /* Required even for Hi-Z */
                ch->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_1;   /* AMPORT_1 = Hi-Z */
                ant_name = "Hi-Z";
                break;
            default:
                ant_name = "?";
                break;
        }

        ch->rsp2TunerParams.biasTEnable = config->bias_t ? 1 : 0;
        ch->rsp2TunerParams.rfNotchEnable = config->rf_notch ? 1 : 0;

        printf("psdr_configure: freq=%.0f Hz, SR=%.0f Hz, BW=%d kHz, gain=%d dB, ant=%s\n",
               config->freq_hz, config->sample_rate_hz,
               config->bandwidth, config->gain_reduction, ant_name);
    }

    return PSDR_OK;
}

/*============================================================================
 * Start / Stop Streaming
 *============================================================================*/

psdr_error_t psdr_start(psdr_context_t *ctx, const psdr_callbacks_t *callbacks) {
    sdrplay_api_ErrT err;

    if (!ctx) return PSDR_ERR_INVALID_ARG;
    if (!ctx->device_selected) return PSDR_ERR_NOT_INITIALIZED;
    if (ctx->streaming) return PSDR_ERR_ALREADY_STREAMING;

    /* Store user callbacks */
    if (callbacks) {
        memcpy(&ctx->user_callbacks, callbacks, sizeof(ctx->user_callbacks));
    } else {
        memset(&ctx->user_callbacks, 0, sizeof(ctx->user_callbacks));
    }

    /* Set up API callbacks */
    ctx->api_callbacks.StreamACbFn = stream_callback_a;
    ctx->api_callbacks.StreamBCbFn = stream_callback_b;
    ctx->api_callbacks.EventCbFn = event_callback;

    /* Store global context for callbacks */
    g_callback_ctx = ctx;

    /* Initialize streaming */
    err = sdrplay_api_Init(ctx->device.dev, &ctx->api_callbacks, ctx);
    if (err != sdrplay_api_Success) {
        fprintf(stderr, "psdr_start: sdrplay_api_Init failed: %s\n",
                sdrplay_api_GetErrorString(err));
        return PSDR_ERR_INIT;
    }

    ctx->streaming = true;
    printf("psdr_start: Streaming started\n");

    return PSDR_OK;
}

psdr_error_t psdr_stop(psdr_context_t *ctx) {
    sdrplay_api_ErrT err;

    if (!ctx) return PSDR_ERR_INVALID_ARG;
    if (!ctx->streaming) return PSDR_ERR_NOT_STREAMING;

    err = sdrplay_api_Uninit(ctx->device.dev);
    if (err != sdrplay_api_Success) {
        fprintf(stderr, "psdr_stop: sdrplay_api_Uninit failed: %s\n",
                sdrplay_api_GetErrorString(err));
        return PSDR_ERR_UNINIT;
    }

    ctx->streaming = false;
    g_callback_ctx = NULL;

    printf("psdr_stop: Streaming stopped\n");

    return PSDR_OK;
}

/*============================================================================
 * Runtime Updates
 *============================================================================*/

psdr_error_t psdr_update(psdr_context_t *ctx, const psdr_config_t *config) {
    sdrplay_api_ErrT err;
    sdrplay_api_ReasonForUpdateT reason = sdrplay_api_Update_None;

    if (!ctx || !config) return PSDR_ERR_INVALID_ARG;
    if (!ctx->streaming) return PSDR_ERR_NOT_STREAMING;

    sdrplay_api_RxChannelParamsT *ch = ctx->params->rxChannelA;
    if (!ch) return PSDR_ERR_DEVICE_PARAMS;

    /* Check what changed and update accordingly */

    /* Frequency */
    if (config->freq_hz != ctx->config.freq_hz) {
        ch->tunerParams.rfFreq.rfHz = config->freq_hz;
        reason |= sdrplay_api_Update_Tuner_Frf;
        ctx->config.freq_hz = config->freq_hz;
    }

    /* Gain */
    if (config->gain_reduction != ctx->config.gain_reduction ||
        config->lna_state != ctx->config.lna_state) {
        ch->tunerParams.gain.gRdB = config->gain_reduction;
        ch->tunerParams.gain.LNAstate = (unsigned char)config->lna_state;
        reason |= sdrplay_api_Update_Tuner_Gr;
        ctx->config.gain_reduction = config->gain_reduction;
        ctx->config.lna_state = config->lna_state;
    }

    /* AGC */
    if (config->agc_mode != ctx->config.agc_mode) {
        ch->ctrlParams.agc.enable = map_agc(config->agc_mode);
        reason |= sdrplay_api_Update_Ctrl_Agc;
        ctx->config.agc_mode = config->agc_mode;
    }

    if (reason == sdrplay_api_Update_None) {
        return PSDR_OK;  /* Nothing to update */
    }

    /* Apply updates */
    err = sdrplay_api_Update(ctx->device.dev, sdrplay_api_Tuner_A,
                             reason, sdrplay_api_Update_Ext1_None);
    if (err != sdrplay_api_Success) {
        fprintf(stderr, "psdr_update: sdrplay_api_Update failed: %s\n",
                sdrplay_api_GetErrorString(err));
        return PSDR_ERR_UPDATE;
    }

    return PSDR_OK;
}
