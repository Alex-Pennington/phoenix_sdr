/**
 * @file main.c
 * @brief Phoenix SDR - I/Q capture with decimation to 48kHz
 * 
 * Records raw I/Q at 2 MSPS and also outputs decimated 48kHz complex
 * baseband suitable for modem input. Uses GPS PPS as primary time reference.
 * 
 * Usage: phoenix_sdr -f <freq_MHz> [-d <duration>] [-o <n>] [-g <gain>] [-p <COM>]
 */

#include "phoenix_sdr.h"
#include "iq_recorder.h"
#include "iqr_meta.h"
#include "decimator.h"
#include "gps_serial.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>  /* For PRIu64 portable format */
#include <time.h>

#ifdef _WIN32
#include <Windows.h>
#include <direct.h>
#define sleep_ms(ms) Sleep(ms)
#define mkdir_p(path) _mkdir(path)
#else
#include <unistd.h>
#include <sys/stat.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#define mkdir_p(path) mkdir(path, 0755)
#endif

/*============================================================================
 * Default Configuration
 *============================================================================*/

#define DEFAULT_DURATION_SEC    5
#define DEFAULT_GAIN_REDUCTION  40
#define DEFAULT_OUTPUT_PREFIX   "capture"
#define SAMPLE_RATE_HZ          2000000.0
#define BANDWIDTH_KHZ           200
#define LNA_STATE               0
#define DEFAULT_GPS_PORT        "COM6"

/* Frequency limits for RSP2 Pro */
#define MIN_FREQ_MHZ    0.001
#define MAX_FREQ_MHZ    2000.0

/* Timing sync check interval - aim for ~2 second intervals
 * SDRplay delivers ~1000 samples per callback at 2MSPS = ~2000 callbacks/sec
 * So 4000 callbacks = ~2 seconds */
#define SYNC_CHECK_INTERVAL     4000

/* Max GPS sync samples to record */
#define MAX_GPS_SYNC_SAMPLES    256

/*============================================================================
 * Runtime Configuration (from command line)
 *============================================================================*/

static double   g_center_freq_hz = 0.0;     /* Must be specified */
static int      g_duration_sec = DEFAULT_DURATION_SEC;
static int      g_gain_reduction = DEFAULT_GAIN_REDUCTION;
static char     g_output_prefix[256] = DEFAULT_OUTPUT_PREFIX;
static int      g_antenna = 0;              /* 0=A, 1=B, 2=Hi-Z */
static bool     g_query_only = false;       /* Query device params and exit */
static bool     g_gps_enabled = false;      /* GPS timing */
static char     g_gps_port[32] = "";        /* GPS COM port */
static bool     g_auto_gain = false;        /* Auto-reduce gain on overload */

/*============================================================================
 * Globals
 *============================================================================*/

static volatile bool g_running = true;
static uint64_t g_sample_count = 0;
static uint32_t g_callback_count = 0;
static iqr_recorder_t *g_raw_recorder = NULL;
static iqr_recorder_t *g_decim_recorder = NULL;
static decim_state_t *g_decimator = NULL;
static gps_context_t g_gps_ctx;
static double g_sample_rate = SAMPLE_RATE_HZ;
static psdr_context_t *g_sdr_ctx = NULL;    /* For auto-gain adjustments */
static psdr_config_t g_sdr_config;          /* Current SDR config */
static int g_overload_count = 0;            /* Count overloads for auto-gain */

/* Decimator output buffer */
static decim_complex_t g_decim_buffer[8192];
static uint64_t g_decim_sample_count = 0;

/* Output filenames (built from prefix) */
static char g_raw_filename[512];
static char g_decim_filename[512];
static char g_recordings_dir[512];

/* Metadata for recordings */
static iqr_meta_t g_raw_meta;
static iqr_meta_t g_decim_meta;

/* GPS timing - primary time reference */
static double g_gps_start_unix = 0.0;     /* GPS time at recording start */
static int g_gps_start_second = 0;        /* Second of minute at start */

/* GPS sync log during recording */
typedef struct {
    double elapsed_sec;
    double gps_unix;
    int hour, minute, second, millisecond;
    int satellites;
    bool valid;
} gps_sync_sample_t;

static gps_sync_sample_t g_gps_sync_log[MAX_GPS_SYNC_SAMPLES];
static int g_gps_sync_count = 0;

/*============================================================================
 * Usage / Help
 *============================================================================*/

static void print_usage(const char *progname) {
    printf("Phoenix SDR - I/Q Recording for MIL-STD-188-110A Testing\n");
    printf("Phoenix Nest LLC - https://github.com/Alex-Pennington/phoenix_sdr\n\n");
    printf("Usage: %s -f <freq_MHz> [options]\n\n", progname);
    printf("Required:\n");
    printf("  -f, --freq <MHz>      Center frequency in MHz (e.g., 10.0 for WWV)\n\n");
    printf("Optional:\n");
    printf("  -d, --duration <sec>  Recording duration in seconds (default: %d)\n", DEFAULT_DURATION_SEC);
    printf("  -o, --output <n>      Output filename prefix (default: \"%s\")\n", DEFAULT_OUTPUT_PREFIX);
    printf("  -g, --gain <dB>       Gain reduction 20-59 dB (default: %d, higher=less gain)\n", DEFAULT_GAIN_REDUCTION);
    printf("  -a, --antenna <port>  Antenna: A, B, or Z (Hi-Z) (default: A)\n");
    printf("  -p, --gps <COM>       GPS timing port (default: %s)\n", DEFAULT_GPS_PORT);
    printf("  -A, --auto-gain       Auto-reduce gain on ADC overload\n");
    printf("  -q, --query           Query device parameters and exit (no recording)\n");
    printf("  -h, --help            Show this help message\n\n");
    printf("Output Files (in recordings/YYYY_MM_DD/):\n");
    printf("  <prefix>_raw.iqr   Full-rate I/Q at 2 MSPS\n");
    printf("  <prefix>_48k.iqr   Decimated I/Q at 48 kHz (modem-ready)\n");
    printf("  <prefix>_48k.meta  Metadata with GPS timing + sync log\n\n");
    printf("Examples:\n");
    printf("  %s -f 10.0 -a Z -A                 # WWV 10 MHz, Hi-Z, auto-gain\n", progname);
    printf("  %s -f 15.0 -d 180 -o wwv15        # WWV 15 MHz, 3 min\n", progname);
    printf("  %s -f 5.0 -g 40 -p COM7           # WWV 5 MHz, GPS on COM7\n", progname);
    printf("  %s -f 7.074 -d 30                 # 40m FT8, 30 sec recording\n\n", progname);
    printf("WWV/WWVH Frequencies: 2.5, 5.0, 10.0, 15.0, 20.0 MHz\n");
    printf("Frequency Range: %.3f - %.1f MHz (SDRplay RSP2 Pro)\n", MIN_FREQ_MHZ, MAX_FREQ_MHZ);
}

/*============================================================================
 * Argument Parsing
 *============================================================================*/

static bool parse_arguments(int argc, char *argv[]) {
    bool freq_specified = false;
    
    /* Default GPS port */
    strncpy(g_gps_port, DEFAULT_GPS_PORT, sizeof(g_gps_port) - 1);
    g_gps_enabled = true;  /* GPS enabled by default */
    
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
        else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--gps") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -p requires a COM port (e.g., COM6)\n");
                return false;
            }
            strncpy(g_gps_port, argv[++i], sizeof(g_gps_port) - 1);
            g_gps_port[sizeof(g_gps_port) - 1] = '\0';
        }
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--query") == 0) {
            g_query_only = true;
        }
        else if (strcmp(argv[i], "-A") == 0 || strcmp(argv[i], "--auto-gain") == 0) {
            g_auto_gain = true;
        }
        else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--antenna") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -a requires antenna port: A, B, or Z\n");
                return false;
            }
            char ant = argv[++i][0];
            if (ant == 'A' || ant == 'a') {
                g_antenna = 0;
            } else if (ant == 'B' || ant == 'b') {
                g_antenna = 1;
            } else if (ant == 'Z' || ant == 'z' || ant == 'H' || ant == 'h') {
                g_antenna = 2;
            } else {
                fprintf(stderr, "Error: Invalid antenna '%s'. Use A, B, or Z (Hi-Z)\n", argv[i]);
                return false;
            }
        }
        else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            fprintf(stderr, "Use -h for help\n");
            return false;
        }
    }
    
    if (!freq_specified && !g_query_only) {
        fprintf(stderr, "Error: Frequency is required. Use -f <MHz>\n");
        fprintf(stderr, "Use -h for help\n");
        return false;
    }
    
    return true;
}

/*============================================================================
 * Directory Creation
 *============================================================================*/

static int create_recordings_dir(int year, int month, int day) {
    /* Create base recordings directory */
    mkdir_p("recordings");
    
    /* Create date subdirectory: recordings/YYYY_MM_DD */
    snprintf(g_recordings_dir, sizeof(g_recordings_dir), 
             "recordings/%04d_%02d_%02d", year, month, day);
    
    int ret = mkdir_p(g_recordings_dir);
    if (ret != 0) {
        /* Directory may already exist, that's OK */
        #ifdef _WIN32
        DWORD attr = GetFileAttributesA(g_recordings_dir);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            fprintf(stderr, "Failed to create directory: %s\n", g_recordings_dir);
            return -1;
        }
        #endif
    }
    
    /* Build output filenames */
    snprintf(g_raw_filename, sizeof(g_raw_filename), 
             "%s/%s_raw.iqr", g_recordings_dir, g_output_prefix);
    snprintf(g_decim_filename, sizeof(g_decim_filename), 
             "%s/%s_48k.iqr", g_recordings_dir, g_output_prefix);
    
    return 0;
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
 * GPS Time Display During Recording
 *============================================================================*/

static void record_gps_sync(double elapsed_sec) {
    if (!g_gps_enabled || !gps_is_connected(&g_gps_ctx)) {
        printf("  %5.1fs | GPS: NOT CONNECTED\n", elapsed_sec);
        return;
    }
    
    gps_reading_t gps;
    if (gps_read_time(&g_gps_ctx, &gps, 500) == 0 && gps.valid) {
        /* Log to array for metadata */
        if (g_gps_sync_count < MAX_GPS_SYNC_SAMPLES) {
            g_gps_sync_log[g_gps_sync_count].elapsed_sec = elapsed_sec;
            g_gps_sync_log[g_gps_sync_count].gps_unix = gps.unix_time;
            g_gps_sync_log[g_gps_sync_count].hour = gps.hour;
            g_gps_sync_log[g_gps_sync_count].minute = gps.minute;
            g_gps_sync_log[g_gps_sync_count].second = gps.second;
            g_gps_sync_log[g_gps_sync_count].millisecond = gps.millisecond;
            g_gps_sync_log[g_gps_sync_count].satellites = gps.satellites;
            g_gps_sync_log[g_gps_sync_count].valid = true;
            g_gps_sync_count++;
        }
        
        /* Calculate offset to next minute marker */
        int sec_in_minute = gps.second;
        double offset_to_minute = (60 - sec_in_minute) - (gps.millisecond / 1000.0);
        
        printf("  %5.1fs | GPS: %02d:%02d:%02d.%03d UTC | SAT:%2d | next min: %.1fs\n",
               elapsed_sec,
               gps.hour, gps.minute, gps.second, gps.millisecond,
               gps.satellites, offset_to_minute);
    } else {
        /* Log failed reading */
        if (g_gps_sync_count < MAX_GPS_SYNC_SAMPLES) {
            g_gps_sync_log[g_gps_sync_count].elapsed_sec = elapsed_sec;
            g_gps_sync_log[g_gps_sync_count].valid = false;
            g_gps_sync_count++;
        }
        printf("  %5.1fs | GPS: WAITING FOR FIX\n", elapsed_sec);
    }
    fflush(stdout);
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
    
    /* Decimate to 48kHz */
    if (g_decimator) {
        size_t out_count = 0;
        decim_error_t derr = decim_process_int16(
            g_decimator, xi, xq, count,
            g_decim_buffer, sizeof(g_decim_buffer)/sizeof(g_decim_buffer[0]),
            &out_count
        );
        
        if (derr == DECIM_OK && out_count > 0) {
            /* Convert decim_complex_t to separate I/Q arrays */
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
                
                /* Record decimated samples */
                if (g_decim_recorder && iqr_is_recording(g_decim_recorder)) {
                    iqr_write(g_decim_recorder, dec_i, dec_q, (uint32_t)out_count);
                }
                
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
    
    /* Print GPS sync every ~2 seconds */
    if (g_callback_count % SYNC_CHECK_INTERVAL == 0) {
        record_gps_sync(duration);
    }
}

static void on_gain_change(double gain_db, int lna_db, void *user_ctx) {
    (void)user_ctx;
    printf("Gain changed: %.2f dB (LNA: %d dB)\n", gain_db, lna_db);
}

static void on_overload(bool overloaded, void *user_ctx) {
    (void)user_ctx;
    static int max_overload_msg_count = 0;  /* Rate limit "at max" messages */
    
    if (overloaded) {
        g_overload_count++;
        
        if (g_auto_gain && g_sdr_ctx) {
            bool adjusted = false;
            
            /* First try increasing gain reduction (3dB steps) */
            if (g_sdr_config.gain_reduction < 59) {
                int new_gr = g_sdr_config.gain_reduction + 3;
                if (new_gr > 59) new_gr = 59;
                g_sdr_config.gain_reduction = new_gr;
                adjusted = true;
                printf("[AUTO-GAIN] Overload #%d - gain reduction: %d dB\n", 
                       g_overload_count, new_gr);
            }
            /* If gain reduction maxed, try increasing LNA state (more attenuation) */
            else if (g_sdr_config.lna_state < 8) {
                g_sdr_config.lna_state++;
                adjusted = true;
                printf("[AUTO-GAIN] Overload #%d - LNA state: %d (more attenuation)\n", 
                       g_overload_count, g_sdr_config.lna_state);
            }
            
            if (adjusted) {
                psdr_error_t err = psdr_update(g_sdr_ctx, &g_sdr_config);
                if (err != PSDR_OK) {
                    printf("[AUTO-GAIN] Failed to update: %s\n", psdr_strerror(err));
                }
                max_overload_msg_count = 0;
            } else {
                /* At absolute max - rate limit messages */
                max_overload_msg_count++;
                if (max_overload_msg_count <= 3) {
                    printf("!!! ADC OVERLOAD - at max settings (GR=59dB, LNA=8) - need external attenuator !!!\n");
                } else if (max_overload_msg_count == 4) {
                    printf("!!! (suppressing further overload messages) !!!\n");
                }
            }
        } else {
            printf("!!! ADC OVERLOAD - use -A for auto-gain or reduce gain with -g !!!\n");
        }
    }
}

/*============================================================================
 * Write GPS Sync Log to Meta File
 *============================================================================*/

static void write_gps_sync_log(const char *iqr_filename) {
    if (g_gps_sync_count == 0) return;
    
    /* Get meta filename */
    char meta_filename[512];
    strncpy(meta_filename, iqr_filename, sizeof(meta_filename) - 6);
    char *ext = strrchr(meta_filename, '.');
    if (ext) strcpy(ext, ".meta");
    
    /* Append sync log to existing meta file */
    FILE *f = fopen(meta_filename, "a");
    if (!f) return;
    
    fprintf(f, "\n[gps_sync_log]\n");
    fprintf(f, "# GPS time samples during recording\n");
    fprintf(f, "# elapsed_sec, gps_unix, hour, minute, second, ms, satellites\n");
    fprintf(f, "sample_count = %d\n", g_gps_sync_count);
    
    int valid_count = 0;
    for (int i = 0; i < g_gps_sync_count; i++) {
        if (g_gps_sync_log[i].valid) valid_count++;
    }
    fprintf(f, "valid_samples = %d\n", valid_count);
    
    /* Write individual samples */
    fprintf(f, "\n# Sample data\n");
    for (int i = 0; i < g_gps_sync_count; i++) {
        gps_sync_sample_t *s = &g_gps_sync_log[i];
        if (s->valid) {
            fprintf(f, "sync_%03d = %.1f, %.3f, %02d:%02d:%02d.%03d, %d\n",
                    i, s->elapsed_sec, s->gps_unix,
                    s->hour, s->minute, s->second, s->millisecond,
                    s->satellites);
        } else {
            fprintf(f, "sync_%03d = %.1f, 0, NO_FIX, 0\n", i, s->elapsed_sec);
        }
    }
    
    fclose(f);
    printf("[META] Appended GPS sync log (%d samples) to %s\n", 
            g_gps_sync_count, meta_filename);
}

/*============================================================================
 * Initialize Metadata from GPS
 *============================================================================*/

static int init_meta_from_gps(iqr_meta_t *meta, gps_reading_t *gps) {
    memset(meta, 0, sizeof(iqr_meta_t));
    
    /* Copy GPS time as primary reference */
    meta->gps_valid = true;
    strncpy(meta->gps_time_iso, gps->iso_string, sizeof(meta->gps_time_iso) - 1);
    meta->gps_time_us = (int64_t)(gps->unix_time * 1000000.0);
    meta->gps_satellites = gps->satellites;
    meta->gps_pc_offset_ms = gps->pc_offset_ms;
    strncpy(meta->gps_port, g_gps_port, sizeof(meta->gps_port) - 1);
    meta->gps_latency_ms = gps_get_latency(&g_gps_ctx);
    
    /* Also set start_time fields to GPS time (for compatibility) */
    strncpy(meta->start_time_iso, gps->iso_string, sizeof(meta->start_time_iso) - 1);
    meta->start_time_us = meta->gps_time_us;
    meta->start_second = gps->second;
    meta->offset_to_next_minute = (60 - gps->second) - (gps->millisecond / 1000.0);
    
    return 0;
}

/*============================================================================
 * Initialize Metadata from System Time (fallback)
 *============================================================================*/

static int init_meta_from_system(iqr_meta_t *meta) {
    memset(meta, 0, sizeof(iqr_meta_t));
    
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    
    snprintf(meta->start_time_iso, sizeof(meta->start_time_iso),
             "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
             utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
             utc->tm_hour, utc->tm_min, utc->tm_sec);
    
    meta->start_time_us = (int64_t)now * 1000000LL;
    meta->start_second = utc->tm_sec;
    meta->offset_to_next_minute = 60 - utc->tm_sec;
    meta->gps_valid = false;
    
    return 0;
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
    gps_reading_t gps_start;
    bool have_gps_time = false;
    
    /* Parse command line */
    if (!parse_arguments(argc, argv)) {
        return 1;
    }
    
    printf("===========================================\n");
    printf("Phoenix SDR - GPS-Timed I/Q Recording\n");
    printf("===========================================\n\n");
    
    /* Set up signal handler */
    signal(SIGINT, signal_handler);
    
    /* Initialize GPS context */
    memset(&g_gps_ctx, 0, sizeof(g_gps_ctx));
    memset(&gps_start, 0, sizeof(gps_start));
    
    /* Skip setup in query-only mode */
    if (!g_query_only) {
        /* Create decimator */
        printf("Initializing decimator (2 MSPS -> 48 kHz)...\n");
        decim_err = decim_create(&g_decimator, SAMPLE_RATE_HZ, 48000.0);
        if (decim_err != DECIM_OK) {
            fprintf(stderr, "Failed to create decimator: %s\n", decim_strerror(decim_err));
            return 1;
        }
        printf("Decimation ratio: %.2f\n\n", decim_get_ratio(g_decimator));
        
        /* Connect to GPS - primary time source */
        printf("Connecting to GPS on %s...\n", g_gps_port);
        if (gps_open(&g_gps_ctx, g_gps_port, 115200) == 0) {
            printf("GPS connected. Waiting for fix...\n");
            
            /* Wait up to 10 seconds for GPS fix */
            for (int attempt = 0; attempt < 10; attempt++) {
                if (gps_read_time(&g_gps_ctx, &gps_start, 1500) == 0 && gps_start.valid) {
                    have_gps_time = true;
                    printf("GPS fix: %s (SAT:%d)\n", 
                           gps_start.iso_string, gps_start.satellites);
                    break;
                }
                printf("  Waiting for satellites... (%d/10)\n", attempt + 1);
            }
            
            if (!have_gps_time) {
                fprintf(stderr, "Warning: GPS connected but no fix. Using system time.\n");
            }
        } else {
            fprintf(stderr, "Warning: Failed to connect to GPS on %s\n", g_gps_port);
            fprintf(stderr, "Using system time instead.\n");
            g_gps_enabled = false;
        }
        printf("\n");
    }
    
    /* Enumerate devices */
    printf("Enumerating SDRplay devices...\n");
    err = psdr_enumerate(devices, 8, &num_devices);
    if (err != PSDR_OK) {
        fprintf(stderr, "Enumeration failed: %s\n", psdr_strerror(err));
        decim_destroy(g_decimator);
        if (g_gps_enabled) gps_close(&g_gps_ctx);
        return 1;
    }
    
    printf("Found %zu device(s)\n\n", num_devices);
    
    if (num_devices == 0) {
        fprintf(stderr, "No devices found. Is an RSP connected?\n");
        decim_destroy(g_decimator);
        if (g_gps_enabled) gps_close(&g_gps_ctx);
        return 1;
    }
    
    /* Open first device */
    printf("Opening device 0...\n");
    err = psdr_open(&ctx, 0);
    if (err != PSDR_OK) {
        fprintf(stderr, "Open failed: %s\n", psdr_strerror(err));
        decim_destroy(g_decimator);
        if (g_gps_enabled) gps_close(&g_gps_ctx);
        return 1;
    }
    
    /* Print device parameters (API defaults) before we configure */
    psdr_print_device_params(ctx);
    
    /* If query-only mode, we're done */
    if (g_query_only) {
        printf("Query complete. Exiting without recording.\n");
        psdr_close(ctx);
        return 0;
    }
    
    /* Configure for HF narrowband */
    psdr_config_defaults(&config);
    config.freq_hz = g_center_freq_hz;
    config.sample_rate_hz = SAMPLE_RATE_HZ;
    config.bandwidth = BANDWIDTH_KHZ;
    config.agc_mode = PSDR_AGC_DISABLED;
    config.gain_reduction = g_gain_reduction;
    config.lna_state = LNA_STATE;
    config.antenna = (psdr_antenna_t)g_antenna;
    
    g_sample_rate = config.sample_rate_hz;
    
    const char *ant_names[] = {"A", "B", "Hi-Z"};
    printf("Configuration:\n");
    printf("  Frequency:    %.6f MHz\n", config.freq_hz / 1e6);
    printf("  Sample Rate:  %.3f MSPS (raw)\n", config.sample_rate_hz / 1e6);
    printf("  Output Rate:  48.000 kHz (decimated)\n");
    printf("  Bandwidth:    %d kHz\n", config.bandwidth);
    printf("  Antenna:      %s\n", ant_names[g_antenna]);
    printf("  Gain Red:     %d dB\n", config.gain_reduction);
    printf("  LNA State:    %d\n", config.lna_state);
    printf("  Duration:     %d seconds\n", g_duration_sec);
    printf("  Output:       %s_*.iqr\n", g_output_prefix);
    printf("  Time Source:  %s\n", have_gps_time ? "GPS PPS" : "System Clock");
    printf("  Auto-Gain:    %s\n", g_auto_gain ? "ENABLED" : "disabled");
    printf("\n");
    
    err = psdr_configure(ctx, &config);
    if (err != PSDR_OK) {
        fprintf(stderr, "Configure failed: %s\n", psdr_strerror(err));
        psdr_close(ctx);
        decim_destroy(g_decimator);
        if (g_gps_enabled) gps_close(&g_gps_ctx);
        return 1;
    }
    
    /* Store context and config for auto-gain callback */
    g_sdr_ctx = ctx;
    memcpy(&g_sdr_config, &config, sizeof(psdr_config_t));
    
    /* Create recorders */
    iqr_err = iqr_create(&g_raw_recorder, 0);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to create raw recorder: %s\n", iqr_strerror(iqr_err));
        psdr_close(ctx);
        decim_destroy(g_decimator);
        if (g_gps_enabled) gps_close(&g_gps_ctx);
        return 1;
    }
    
    iqr_err = iqr_create(&g_decim_recorder, 0);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to create decim recorder: %s\n", iqr_strerror(iqr_err));
        iqr_destroy(g_raw_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        if (g_gps_enabled) gps_close(&g_gps_ctx);
        return 1;
    }
    
    /* Get fresh GPS time right before recording starts */
    if (have_gps_time && gps_is_connected(&g_gps_ctx)) {
        printf("Synchronizing to GPS...\n");
        if (gps_read_time(&g_gps_ctx, &gps_start, 2000) == 0 && gps_start.valid) {
            init_meta_from_gps(&g_raw_meta, &gps_start);
            g_gps_start_unix = gps_start.unix_time;
            g_gps_start_second = gps_start.second;
            printf("Recording starts at: %s (SAT:%d)\n", 
                   gps_start.iso_string, gps_start.satellites);
            printf("Next minute marker at: %.1f sec into recording\n",
                   g_raw_meta.offset_to_next_minute);
        } else {
            printf("Warning: Lost GPS fix, using system time\n");
            init_meta_from_system(&g_raw_meta);
            have_gps_time = false;
        }
    } else {
        init_meta_from_system(&g_raw_meta);
        printf("Using system time: %s\n", g_raw_meta.start_time_iso);
    }
    
    /* Create recordings directory based on date */
    {
        int year, month, day;
        if (sscanf(g_raw_meta.start_time_iso, "%d-%d-%d", &year, &month, &day) == 3) {
            if (create_recordings_dir(year, month, day) == 0) {
                printf("Recording to: %s/\n", g_recordings_dir);
            }
        } else {
            snprintf(g_raw_filename, sizeof(g_raw_filename), "%s_raw.iqr", g_output_prefix);
            snprintf(g_decim_filename, sizeof(g_decim_filename), "%s_48k.iqr", g_output_prefix);
            printf("Warning: Could not parse date, recording to current directory\n");
        }
    }
    printf("\n");
    
    /* Copy metadata to decimated */
    memcpy(&g_decim_meta, &g_raw_meta, sizeof(iqr_meta_t));
    
    /* Fill in recording parameters */
    g_raw_meta.sample_rate_hz = config.sample_rate_hz;
    g_raw_meta.center_freq_hz = config.freq_hz;
    g_raw_meta.bandwidth_khz = config.bandwidth;
    g_raw_meta.gain_reduction = config.gain_reduction;
    g_raw_meta.lna_state = config.lna_state;
    
    g_decim_meta.sample_rate_hz = 48000.0;
    g_decim_meta.center_freq_hz = config.freq_hz;
    g_decim_meta.bandwidth_khz = config.bandwidth;
    g_decim_meta.gain_reduction = config.gain_reduction;
    g_decim_meta.lna_state = config.lna_state;
    
    /* Start recordings */
    iqr_err = iqr_start(g_raw_recorder, g_raw_filename,
                        config.sample_rate_hz, config.freq_hz,
                        config.bandwidth, config.gain_reduction, config.lna_state);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to start raw recording: %s\n", iqr_strerror(iqr_err));
        iqr_destroy(g_raw_recorder);
        iqr_destroy(g_decim_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        if (g_gps_enabled) gps_close(&g_gps_ctx);
        return 1;
    }
    iqr_meta_write_start(g_raw_filename, &g_raw_meta);
    
    iqr_err = iqr_start(g_decim_recorder, g_decim_filename,
                        48000.0, config.freq_hz,
                        config.bandwidth, config.gain_reduction, config.lna_state);
    if (iqr_err != IQR_OK) {
        fprintf(stderr, "Failed to start decim recording: %s\n", iqr_strerror(iqr_err));
        iqr_stop(g_raw_recorder);
        iqr_destroy(g_raw_recorder);
        iqr_destroy(g_decim_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        if (g_gps_enabled) gps_close(&g_gps_ctx);
        return 1;
    }
    iqr_meta_write_start(g_decim_filename, &g_decim_meta);
    
    /* Set up callbacks */
    psdr_callbacks_t callbacks = {
        .on_samples = on_samples,
        .on_gain_change = on_gain_change,
        .on_overload = on_overload,
        .user_ctx = NULL
    };
    
    /* Start streaming */
    printf("Recording %d seconds of I/Q data...\n", g_duration_sec);
    if (g_gps_enabled) {
        printf("  Elapsed |        GPS Time        | Sats | Next Minute\n");
        printf("----------|------------------------|------|------------\n");
    }
    
    err = psdr_start(ctx, &callbacks);
    if (err != PSDR_OK) {
        fprintf(stderr, "Start failed: %s\n", psdr_strerror(err));
        iqr_stop(g_raw_recorder);
        iqr_stop(g_decim_recorder);
        iqr_destroy(g_raw_recorder);
        iqr_destroy(g_decim_recorder);
        psdr_close(ctx);
        decim_destroy(g_decimator);
        if (g_gps_enabled) gps_close(&g_gps_ctx);
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
    
    /* Update metadata with final stats */
    g_raw_meta.sample_count = g_sample_count;
    g_raw_meta.duration_sec = (double)g_sample_count / g_sample_rate;
    g_raw_meta.recording_complete = true;
    g_raw_meta.end_time_us = g_raw_meta.start_time_us + 
                             (int64_t)(g_raw_meta.duration_sec * 1000000.0);
    iqr_meta_write_end(g_raw_filename, &g_raw_meta);
    
    g_decim_meta.sample_count = g_decim_sample_count;
    g_decim_meta.duration_sec = (double)g_decim_sample_count / 48000.0;
    g_decim_meta.recording_complete = true;
    g_decim_meta.end_time_us = g_decim_meta.start_time_us + 
                               (int64_t)(g_decim_meta.duration_sec * 1000000.0);
    iqr_meta_write_end(g_decim_filename, &g_decim_meta);
    
    /* Write GPS sync log */
    if (g_gps_sync_count > 0) {
        write_gps_sync_log(g_raw_filename);
        write_gps_sync_log(g_decim_filename);
    }
    
    /* Print final stats */
    printf("\n===========================================\n");
    printf("Recording Complete\n");
    printf("===========================================\n");
    printf("\nRaw recording (2 MSPS):\n");
    printf("  File:     %s\n", g_raw_filename);
    printf("  Samples:  %" PRIu64 "\n", g_sample_count);
    printf("  Duration: %.2f seconds\n", (double)g_sample_count / g_sample_rate);
    printf("  Size:     %.2f MB\n", (64.0 + g_sample_count * 4.0) / (1024.0 * 1024.0));
    
    printf("\nDecimated recording (48 kHz):\n");
    printf("  File:     %s\n", g_decim_filename);
    printf("  Samples:  %" PRIu64 "\n", g_decim_sample_count);
    printf("  Duration: %.2f seconds\n", (double)g_decim_sample_count / 48000.0);
    printf("  Size:     %.2f MB\n", (64.0 + g_decim_sample_count * 4.0) / (1024.0 * 1024.0));
    
    printf("\nTiming (%s):\n", g_raw_meta.gps_valid ? "GPS" : "System");
    printf("  Start:    %s\n", g_raw_meta.start_time_iso);
    if (g_raw_meta.gps_valid) {
        printf("  Sats:     %d\n", g_raw_meta.gps_satellites);
    }
    printf("  Sync log: %d samples\n", g_gps_sync_count);
    
    if (g_overload_count > 0) {
        printf("\nOverload Events: %d\n", g_overload_count);
        if (g_auto_gain) {
            printf("  Final gain reduction: %d dB (started at %d dB)\n", 
                   g_sdr_config.gain_reduction, g_gain_reduction);
            printf("  Final LNA state: %d (started at %d)\n",
                   g_sdr_config.lna_state, LNA_STATE);
        }
    }
    
    /* Cleanup */
    iqr_destroy(g_raw_recorder);
    iqr_destroy(g_decim_recorder);
    decim_destroy(g_decimator);
    if (g_gps_enabled) gps_close(&g_gps_ctx);
    psdr_close(ctx);
    
    printf("\nDone.\n");
    return 0;
}
