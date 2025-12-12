/**
 * @file main.c
 * @brief Phoenix SDR - I/Q capture with decimation to 48kHz
 * 
 * Records raw I/Q at 2 MSPS and also outputs decimated 48kHz complex
 * baseband suitable for modem input.
 * 
 * Usage: phoenix_sdr -f <freq_MHz> [-d <duration>] [-o <output>] [-g <gain>]
 */

#include "phoenix_sdr.h"
#include "iq_recorder.h"
#include "decimator.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>

#ifdef _WIN32
#include <Windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

/*============================================================================
 * Default Configuration
 *============================================================================*/

#define DEFAULT_DURATION_SEC    5
#define DEFAULT_GAIN_REDUCTION  40
#define DEFAULT_OUTPUT_PREFIX   "capture"
#define SAMPLE_RATE_HZ          2000000.0
#define BANDWIDTH_KHZ           200
#define LNA_STATE               4

/* Frequency limits for RSP2 Pro */
#define MIN_FREQ_MHZ    0.001
#define MAX_FREQ_MHZ    2000.0

/*============================================================================
 * Runtime Configuration (from command line)
 *============================================================================*/

static double   g_center_freq_hz = 0.0;     /* Must be specified */
static int      g_duration_sec = DEFAULT_DURATION_SEC;
static int      g_gain_reduction = DEFAULT_GAIN_REDUCTION;
static char     g_output_prefix[256] = DEFAULT_OUTPUT_PREFIX;

/*============================================================================
 * Globals
 *============================================================================*/

static volatile bool g_running = true;
static uint64_t g_sample_count = 0;
static uint32_t g_callback_count = 0;
static iqr_recorder_t *g_raw_recorder = NULL;
static iqr_recorder_t *g_decim_recorder = NULL;
static decim_state_t *g_decimator = NULL;
static double g_sample_rate = SAMPLE_RATE_HZ;

/* Decimator output buffer */
static decim_complex_t g_decim_buffer[8192];
static uint64_t g_decim_sample_count = 0;

/* Output filenames (built from prefix) */
static char g_raw_filename[512];
static char g_decim_filename[512];

/*============================================================================
 * Usage / Help
 *============================================================================*/

static void print_usage(const char *progname) {
    printf("Phoenix SDR - I/Q Recording for MIL-STD-188-110A Testing\n");
    printf("Phoenix Nest LLC - https://github.com/Alex-Pennington/phoenix_sdr\n\n");
    printf("Usage: %s -f <freq_MHz> [options]\n\n", progname);
    printf("Required:\n");
    printf("  -f, --freq <MHz>      Center frequency in MHz (e.g., 7.102, 14.100)\n\n");
    printf("Optional:\n");
    printf("  -d, --duration <sec>  Recording duration in seconds (default: %d)\n", DEFAULT_DURATION_SEC);
    printf("  -o, --output <name>   Output filename prefix (default: \"%s\")\n", DEFAULT_OUTPUT_PREFIX);
    printf("  -g, --gain <dB>       Gain reduction 20-59 dB (default: %d)\n", DEFAULT_GAIN_REDUCTION);
    printf("  -h, --help            Show this help message\n\n");
    printf("Output Files:\n");
    printf("  <name>_raw.iqr        Full-rate I/Q at 2 MSPS\n");
    printf("  <name>_48k.iqr        Decimated I/Q at 48 kHz (modem-ready)\n\n");
    printf("Examples:\n");
    printf("  %s -f 7.074                    # Record 5 sec at 7.074 MHz (40m FT8)\n", progname);
    printf("  %s -f 14.074 -d 30             # Record 30 sec at 14.074 MHz (20m FT8)\n", progname);
    printf("  %s -f 7.074 -o ft8_capture     # Custom output filename\n", progname);
    printf("  %s -f 14.074 -g 30 -d 60       # Lower gain, 60 sec recording\n\n", progname);
    printf("Frequency Range: %.3f - %.1f MHz (SDRplay RSP2 Pro)\n", MIN_FREQ_MHZ, MAX_FREQ_MHZ);
}

/*============================================================================
 * Argument Parsing
 *============================================================================*/

static bool parse_arguments(int argc, char *argv[]) {
    bool freq_specified = false;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
        else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--freq") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -f requires a frequency value in MHz\n");
                return false;
            }
            double freq_mhz = atof(argv[++i]);
            if (freq_mhz < MIN_FREQ_MHZ || freq_mhz > MAX_FREQ_MHZ) {
                fprintf(stderr, "Error: Frequency %.6f MHz out of range (%.3f - %.1f MHz)\n",
                        freq_mhz, MIN_FREQ_MHZ, MAX_FREQ_MHZ);
                return false;
            }
            g_center_freq_hz = freq_mhz * 1e6;
            freq_specified = true;
        }
        else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--duration") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -d requires a duration value in seconds\n");
                return false;
            }
            g_duration_sec = atoi(argv[++i]);
            if (g_duration_sec < 1 || g_duration_sec > 3600) {
                fprintf(stderr, "Error: Duration must be 1-3600 seconds\n");
                return false;
            }
        }
        else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -o requires an output filename prefix\n");
                return false;
            }
            strncpy(g_output_prefix, argv[++i], sizeof(g_output_prefix) - 1);
            g_output_prefix[sizeof(g_output_prefix) - 1] = '\0';
        }
        else if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--gain") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -g requires a gain reduction value in dB\n");
                return false;
            }
            g_gain_reduction = atoi(argv[++i]);
            if (g_gain_reduction < 20 || g_gain_reduction > 59) {
                fprintf(stderr, "Error: Gain reduction must be 20-59 dB\n");
                return false;
            }
        }
        else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            fprintf(stderr, "Use -h for help\n");
            return false;
        }
    }
    
    if (!freq_specified) {
        fprintf(stderr, "Error: Frequency is required. Use -f <MHz>\n");
        fprintf(stderr, "Use -h for help\n");
        return false;
    }
    
    /* Build output filenames from prefix */
    snprintf(g_raw_filename, sizeof(g_raw_filename), "%s_raw.iqr", g_output_prefix);
    snprintf(g_decim_filename, sizeof(g_decim_filename), "%s_48k.iqr", g_output_prefix);
    
    return true;
}

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    printf("\nInterrupt received, stopping...\n");
    g_running = false;
}

/*============================================================================
 * Callbacks
 *============================================================================*/

static void on_samples(
    const int16_t *xi,
    const int16_t *xq,
    uint32_t count,
    bool reset,
    void *user_ctx
) {
    (void)user_ctx;
    
    if (reset) {
        printf("*** Stream reset - flush buffers ***\n");
        if (g_decimator) {
            decim_reset(g_decimator);
        }
    }
    
    /* Record raw samples if recorder is active */
    if (g_raw_recorder && iqr_is_recording(g_raw_recorder)) {
        iqr_error_t err = iqr_write(g_raw_recorder, xi, xq, count);
        if (err != IQR_OK) {
            fprintf(stderr, "Raw recording error: %s\n", iqr_strerror(err));
        }
    }
    
    /* Decimate and record 48kHz output */
    if (g_decimator && g_decim_recorder && iqr_is_recording(g_decim_recorder)) {
        size_t out_count = 0;
        decim_error_t derr = decim_process_int16(
            g_decimator, xi, xq, count,
            g_decim_buffer, sizeof(g_decim_buffer)/sizeof(g_decim_buffer[0]),
            &out_count
        );
        
        if (derr == DECIM_OK && out_count > 0) {
            /* Convert decim_complex_t to separate I/Q arrays for recorder */
            /* Note: iqr_write expects int16, so we scale back */
            int16_t *dec_i = malloc(out_count * sizeof(int16_t));
            int16_t *dec_q = malloc(out_count * sizeof(int16_t));
            
            if (dec_i && dec_q) {
                for (size_t i = 0; i < out_count; i++) {
                    /* Clamp to int16 range */
                    float fi = g_decim_buffer[i].i * 32767.0f;
                    float fq = g_decim_buffer[i].q * 32767.0f;
                    if (fi > 32767.0f) fi = 32767.0f;
                    if (fi < -32768.0f) fi = -32768.0f;
                    if (fq > 32767.0f) fq = 32767.0f;
                    if (fq < -32768.0f) fq = -32768.0f;
                    dec_i[i] = (int16_t)fi;
                    dec_q[i] = (int16_t)fq;
                }
                
                iqr_write(g_decim_recorder, dec_i, dec_q, (uint32_t)out_count);
                g_decim_sample_count += out_count;
            }
            
            free(dec_i);
            free(dec_q);
        }
    }
    
    g_sample_count += count;
    g_callback_count++;
    
    /* Check if we've recorded enough */
    double duration = (double)g_sample_count / g_sample_rate;
    if (duration >= g_duration_sec) {
        g_running = false;
    }
    
    /* Print progress every ~0.5 seconds */
    if (g_callback_count % 50 == 0) {
        printf("Recording: %.1f / %d sec  (raw: %llu, decim: %llu samples)\r", 
               duration, g_duration_sec,
               (unsigned long long)g_sample_count,
               (unsigned long long)g_decim_sample_count);
        fflush(stdout);
    }
}

static void on_gain_change(double gain_db, int lna_db, void *user_ctx) {
    (void)user_ctx;
    printf("Gain changed: %.2f dB (LNA: %d dB)\n", gain_db, lna_db);
}

static void on_overload(bool overloaded, void *user_ctx) {
    (void)user_ctx;
    if (overloaded) {
        printf("!!! ADC OVERLOAD - reduce gain !!!\n");
    } else {
        printf("ADC overload cleared\n");
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    psdr_error_t err;
    iqr_error_t iqr_err;
    decim_error_t decim_err;
    psdr_device_info_t devices[8];
    size_t num_devices = 0;
    psdr_context_t *ctx = NULL;
    psdr_config_t config;
    
    /* Parse command line */
    if (!parse_arguments(argc, argv)) {
        return 1;
    }
    
    printf("===========================================\n");
    printf("Phoenix SDR - I/Q Recording with Decimation\n");
    printf("===========================================\n\n");
    
    /* Set up signal handler */
    signal(SIGINT, signal_handler);
    
    /* Create decimator */
    printf("Initializing decimator (2 MSPS -> 48 kHz)...\n");
    decim_err = decim_create(&g_decimator, SAMPLE_RATE_HZ, 48000.0);
    if (decim_err != DECIM_OK) {
        fprintf(stderr, "Failed to create decimator: %s\n", decim_strerror(decim_err));
        return 1;
    }
    printf("Decimation ratio: %.2f\n\n", decim_get_ratio(g_decimator));
    
    /* Enumerate devices */
    printf("Enumerating SDRplay devices...\n");
    err = psdr_enumerate(devices, 8, &num_devices);
    if (err != PSDR_OK) {
        fprintf(stderr, "Enumeration failed: %s\n", psdr_strerror(err));
        decim_destroy(g_decimator);
        return 1;
    }
    
    printf("Found %zu device(s)\n\n", num_devices);
    
    if (num_devices == 0) {
        fprintf(stderr, "No devices found. Is an RSP connected?\n");
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Open first device */
    printf("Opening device 0...\n");
    err = psdr_open(&ctx, 0);
    if (err != PSDR_OK) {
        fprintf(stderr, "Open failed: %s\n", psdr_strerror(err));
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Configure for HF narrowband */
    psdr_config_defaults(&config);
    config.freq_hz = g_center_freq_hz;
    config.sample_rate_hz = SAMPLE_RATE_HZ;
    config.bandwidth = BANDWIDTH_KHZ;
    config.agc_mode = PSDR_AGC_DISABLED;
    config.gain_reduction = g_gain_reduction;
    config.lna_state = LNA_STATE;
    
    g_sample_rate = config.sample_rate_hz;
    
    printf("Configuration:\n");
    printf("  Frequency:    %.6f MHz\n", config.freq_hz / 1e6);
    printf("  Sample Rate:  %.3f MSPS (raw)\n", config.sample_rate_hz / 1e6);
    printf("  Output Rate:  48.000 kHz (decimated)\n");
    printf("  Bandwidth:    %d kHz\n", config.bandwidth);
    printf("  Gain Red:     %d dB\n", config.gain_reduction);
    printf("  LNA State:    %d\n", config.lna_state);
    printf("  Duration:     %d seconds\n", g_duration_sec);
    printf("  Raw output:   %s\n", g_raw_filename);
    printf("  Decim output: %s\n", g_decim_filename);
    printf("\n");
    
    err = psdr_configure(ctx, &config);
    if (err != PSDR_OK) {
        fprintf(stderr, "Configure failed: %s\n", psdr_strerror(err));
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Create raw I/Q recorder */
    iqr_err = iqr_create(&g_raw_recorder, 0);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to create raw recorder: %s\n", iqr_strerror(iqr_err));
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Create decimated I/Q recorder */
    iqr_err = iqr_create(&g_decim_recorder, 0);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to create decim recorder: %s\n", iqr_strerror(iqr_err));
        iqr_destroy(g_raw_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Start raw recording */
    iqr_err = iqr_start(g_raw_recorder, 
                        g_raw_filename,
                        config.sample_rate_hz,
                        config.freq_hz,
                        config.bandwidth,
                        config.gain_reduction,
                        config.lna_state);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to start raw recording: %s\n", iqr_strerror(iqr_err));
        iqr_destroy(g_raw_recorder);
        iqr_destroy(g_decim_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Start decimated recording */
    iqr_err = iqr_start(g_decim_recorder, 
                        g_decim_filename,
                        48000.0,              /* Decimated rate */
                        config.freq_hz,
                        config.bandwidth,
                        config.gain_reduction,
                        config.lna_state);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to start decim recording: %s\n", iqr_strerror(iqr_err));
        iqr_stop(g_raw_recorder);
        iqr_destroy(g_raw_recorder);
        iqr_destroy(g_decim_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Set up callbacks */
    psdr_callbacks_t callbacks = {
        .on_samples = on_samples,
        .on_gain_change = on_gain_change,
        .on_overload = on_overload,
        .user_ctx = NULL
    };
    
    /* Start streaming */
    printf("Recording %d seconds of I/Q data...\n\n", g_duration_sec);
    err = psdr_start(ctx, &callbacks);
    if (err != PSDR_OK) {
        fprintf(stderr, "Start failed: %s\n", psdr_strerror(err));
        iqr_stop(g_raw_recorder);
        iqr_stop(g_decim_recorder);
        iqr_destroy(g_raw_recorder);
        iqr_destroy(g_decim_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        return 1;
    }
    
    /* Run until duration reached or interrupted */
    while (g_running && psdr_is_streaming(ctx)) {
        sleep_ms(100);
    }
    
    /* Stop streaming */
    printf("\n\nStopping stream...\n");
    psdr_stop(ctx);
    
    /* Stop recordings */
    printf("Finalizing recordings...\n");
    iqr_stop(g_raw_recorder);
    iqr_stop(g_decim_recorder);
    
    /* Print final stats */
    printf("\n===========================================\n");
    printf("Recording Complete\n");
    printf("===========================================\n");
    printf("\nRaw recording (2 MSPS):\n");
    printf("  File:          %s\n", g_raw_filename);
    printf("  Samples:       %llu\n", (unsigned long long)g_sample_count);
    printf("  Duration:      %.2f seconds\n", (double)g_sample_count / g_sample_rate);
    printf("  Size:          %.2f MB\n", 
           (64.0 + g_sample_count * 4.0) / (1024.0 * 1024.0));
    
    printf("\nDecimated recording (48 kHz):\n");
    printf("  File:          %s\n", g_decim_filename);
    printf("  Samples:       %llu\n", (unsigned long long)g_decim_sample_count);
    printf("  Duration:      %.2f seconds\n", (double)g_decim_sample_count / 48000.0);
    printf("  Size:          %.2f MB\n", 
           (64.0 + g_decim_sample_count * 4.0) / (1024.0 * 1024.0));
    
    printf("\nDecimation ratio achieved: %.2f:1\n",
           (double)g_sample_count / (double)g_decim_sample_count);
    
    /* Cleanup */
    iqr_destroy(g_raw_recorder);
    iqr_destroy(g_decim_recorder);
    decim_destroy(g_decimator);
    psdr_close(ctx);
    
    printf("\nDone.\n");
    return 0;
}
