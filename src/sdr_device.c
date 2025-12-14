/**
 * @file sdr_device.c
 * @brief SDRplay device management - enumeration, open, close
 */

#include "phoenix_sdr.h"
#include "sdrplay_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*============================================================================
 * Internal Context Structure
 *============================================================================*/

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

/*============================================================================
 * Error String Table
 *============================================================================*/

static const char *error_strings[] = {
    [PSDR_OK]                  = "Success",
    [PSDR_ERR_API_OPEN]        = "Failed to open SDRplay API",
    [PSDR_ERR_API_VERSION]     = "SDRplay API version mismatch",
    [PSDR_ERR_NO_DEVICES]      = "No SDRplay devices found",
    [PSDR_ERR_DEVICE_SELECT]   = "Failed to select device",
    [PSDR_ERR_DEVICE_PARAMS]   = "Failed to get device parameters",
    [PSDR_ERR_INIT]            = "Failed to initialize streaming",
    [PSDR_ERR_UNINIT]          = "Failed to stop streaming",
    [PSDR_ERR_UPDATE]          = "Failed to update parameters",
    [PSDR_ERR_INVALID_ARG]     = "Invalid argument",
    [PSDR_ERR_NOT_INITIALIZED] = "Device not initialized",
    [PSDR_ERR_ALREADY_STREAMING] = "Already streaming",
    [PSDR_ERR_NOT_STREAMING]   = "Not currently streaming",
    [PSDR_ERR_CALLBACK]        = "Callback error",
    [PSDR_ERR_UNKNOWN]         = "Unknown error"
};

const char* psdr_strerror(psdr_error_t err) {
    if (err < 0 || err > PSDR_ERR_UNKNOWN) {
        return "Invalid error code";
    }
    return error_strings[err];
}

/*============================================================================
 * Default Configuration
 *============================================================================*/

void psdr_config_defaults(psdr_config_t *config) {
    if (!config) return;

    memset(config, 0, sizeof(*config));

    config->freq_hz        = 7100000.0;     /* 40m band */
    config->sample_rate_hz = 2000000.0;     /* 2 MSPS - 14-bit mode */
    config->bandwidth      = PSDR_BW_200;   /* 200 kHz - narrowest */
    config->antenna        = PSDR_ANT_A;
    config->agc_mode       = PSDR_AGC_DISABLED;  /* Manual gain for modem work */
    config->gain_reduction = 40;            /* Moderate reduction */
    config->lna_state      = 4;             /* Mid-range LNA */
    config->decimation     = 1;             /* No hardware decimation */
    config->bias_t         = false;
    config->rf_notch       = false;

    /* Advanced settings */
    config->if_mode           = PSDR_IF_ZERO;   /* Zero-IF (signal at DC) */
    config->dc_offset_corr    = true;           /* DC offset correction ON */
    config->iq_imbalance_corr = true;           /* IQ imbalance correction ON */
    config->agc_setpoint_dbfs = -60;            /* AGC setpoint -60 dBFS */
}

/*============================================================================
 * Device Enumeration
 *============================================================================*/

psdr_error_t psdr_enumerate(
    psdr_device_info_t *devices,
    size_t max_devices,
    size_t *num_found
) {
    sdrplay_api_ErrT err;
    sdrplay_api_DeviceT dev_list[SDRPLAY_MAX_DEVICES];
    unsigned int ndev = 0;
    float api_ver = 0.0f;

    if (num_found) *num_found = 0;

    /* Open API */
    err = sdrplay_api_Open();
    if (err != sdrplay_api_Success) {
        fprintf(stderr, "psdr_enumerate: sdrplay_api_Open failed: %s\n",
                sdrplay_api_GetErrorString(err));
        return PSDR_ERR_API_OPEN;
    }

    /* Check API version */
    err = sdrplay_api_ApiVersion(&api_ver);
    if (err != sdrplay_api_Success) {
        sdrplay_api_Close();
        return PSDR_ERR_API_VERSION;
    }

    printf("psdr_enumerate: SDRplay API version %.2f\n", api_ver);

    /* Get device list */
    err = sdrplay_api_GetDevices(dev_list, &ndev, SDRPLAY_MAX_DEVICES);
    if (err != sdrplay_api_Success) {
        fprintf(stderr, "psdr_enumerate: GetDevices failed: %s\n",
                sdrplay_api_GetErrorString(err));
        sdrplay_api_Close();
        return PSDR_ERR_NO_DEVICES;
    }

    if (num_found) *num_found = ndev;

    /* Copy device info to user buffer */
    if (devices && max_devices > 0) {
        size_t copy_count = (ndev < max_devices) ? ndev : max_devices;

        for (size_t i = 0; i < copy_count; i++) {
            strncpy(devices[i].serial, dev_list[i].SerNo,
                    sizeof(devices[i].serial) - 1);
            devices[i].serial[sizeof(devices[i].serial) - 1] = '\0';
            devices[i].hw_version = dev_list[i].hwVer;
            devices[i].available = true;

            printf("  Device %zu: Serial=%s HW=%d\n",
                   i, devices[i].serial, devices[i].hw_version);
        }
    }

    sdrplay_api_Close();
    return PSDR_OK;
}

/*============================================================================
 * Open / Close
 *============================================================================*/

psdr_error_t psdr_open(psdr_context_t **ctx, unsigned int device_idx) {
    sdrplay_api_ErrT err;
    sdrplay_api_DeviceT dev_list[SDRPLAY_MAX_DEVICES];
    unsigned int ndev = 0;

    if (!ctx) return PSDR_ERR_INVALID_ARG;

    /* Allocate context */
    psdr_context_t *c = calloc(1, sizeof(psdr_context_t));
    if (!c) return PSDR_ERR_UNKNOWN;

    /* Open API */
    err = sdrplay_api_Open();
    if (err != sdrplay_api_Success) {
        free(c);
        return PSDR_ERR_API_OPEN;
    }
    c->api_open = true;

    /* Lock API for device selection */
    sdrplay_api_LockDeviceApi();

    /* Get devices */
    err = sdrplay_api_GetDevices(dev_list, &ndev, SDRPLAY_MAX_DEVICES);
    if (err != sdrplay_api_Success || ndev == 0) {
        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_Close();
        free(c);
        return PSDR_ERR_NO_DEVICES;
    }

    if (device_idx >= ndev) {
        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_Close();
        free(c);
        return PSDR_ERR_INVALID_ARG;
    }

    /* Copy device info */
    memcpy(&c->device, &dev_list[device_idx], sizeof(c->device));

    /* Select device */
    err = sdrplay_api_SelectDevice(&c->device);
    if (err != sdrplay_api_Success) {
        fprintf(stderr, "psdr_open: SelectDevice failed: %s\n",
                sdrplay_api_GetErrorString(err));
        sdrplay_api_UnlockDeviceApi();
        sdrplay_api_Close();
        free(c);
        return PSDR_ERR_DEVICE_SELECT;
    }
    c->device_selected = true;

    /* Unlock API */
    sdrplay_api_UnlockDeviceApi();

    /* Get device parameters */
    err = sdrplay_api_GetDeviceParams(c->device.dev, &c->params);
    if (err != sdrplay_api_Success || !c->params) {
        fprintf(stderr, "psdr_open: GetDeviceParams failed: %s\n",
                sdrplay_api_GetErrorString(err));
        sdrplay_api_ReleaseDevice(&c->device);
        sdrplay_api_Close();
        free(c);
        return PSDR_ERR_DEVICE_PARAMS;
    }

    /* Initialize with defaults */
    psdr_config_defaults(&c->config);

    *ctx = c;
    printf("psdr_open: Device opened successfully (Serial: %s)\n", c->device.SerNo);

    return PSDR_OK;
}

void psdr_close(psdr_context_t *ctx) {
    if (!ctx) return;

    /* Stop streaming if active */
    if (ctx->streaming) {
        psdr_stop(ctx);
    }

    /* Release device */
    if (ctx->device_selected) {
        sdrplay_api_ReleaseDevice(&ctx->device);
    }

    /* Close API */
    if (ctx->api_open) {
        sdrplay_api_Close();
    }

    free(ctx);
    printf("psdr_close: Device closed\n");
}

/*============================================================================
 * Query Functions
 *============================================================================*/

double psdr_get_sample_rate(const psdr_context_t *ctx) {
    if (!ctx || !ctx->streaming) return 0.0;
    return ctx->actual_sample_rate;
}

bool psdr_is_streaming(const psdr_context_t *ctx) {
    if (!ctx) return false;
    return ctx->streaming;
}

void psdr_print_device_params(const psdr_context_t *ctx) {
    if (!ctx || !ctx->params) {
        printf("psdr_print_device_params: No device params available\n");
        return;
    }

    sdrplay_api_DeviceParamsT *p = ctx->params;
    sdrplay_api_RxChannelParamsT *ch = p->rxChannelA;

    printf("\n=== SDRplay Device Parameters (API Defaults) ===\n");

    /* Device-level params */
    if (p->devParams) {
        printf("Device:\n");
        printf("  Sample Rate:    %.0f Hz\n", p->devParams->fsFreq.fsHz);
        printf("  PPM Offset:     %.1f\n", p->devParams->ppm);
    }

    /* Tuner params */
    if (ch) {
        printf("Tuner:\n");
        printf("  RF Frequency:   %.0f Hz (%.6f MHz)\n",
               ch->tunerParams.rfFreq.rfHz,
               ch->tunerParams.rfFreq.rfHz / 1e6);
        printf("  Bandwidth:      %d kHz\n", ch->tunerParams.bwType);
        printf("  IF Type:        %d kHz\n", ch->tunerParams.ifType);
        printf("  LO Mode:        %d\n", ch->tunerParams.loMode);

        printf("Gain:\n");
        printf("  Gain Reduction: %d dB\n", ch->tunerParams.gain.gRdB);
        printf("  LNA State:      %d\n", ch->tunerParams.gain.LNAstate);
        printf("  Min GR Mode:    %d\n", ch->tunerParams.gain.minGr);

        printf("Control:\n");
        printf("  AGC Enable:     %d\n", ch->ctrlParams.agc.enable);
        printf("  AGC Setpoint:   %d dBfs\n", ch->ctrlParams.agc.setPoint_dBfs);
        printf("  DC Offset:      %d\n", ch->ctrlParams.dcOffset.DCenable);
        printf("  IQ Balance:     %d\n", ch->ctrlParams.dcOffset.IQenable);
        printf("  Decimation:     %d (enable=%d)\n",
               ch->ctrlParams.decimation.decimationFactor,
               ch->ctrlParams.decimation.enable);

        /* RSP2-specific params */
        printf("RSP2 Tuner:\n");
        printf("  Antenna:        %d (5=A, 6=B)\n", ch->rsp2TunerParams.antennaSel);
        printf("  AM Port:        %d (0=Port2, 1=Port1/HiZ)\n", ch->rsp2TunerParams.amPortSel);
        printf("  Bias-T:         %d\n", ch->rsp2TunerParams.biasTEnable);
        printf("  RF Notch:       %d\n", ch->rsp2TunerParams.rfNotchEnable);
    }

    printf("================================================\n\n");
}
