/**
 * @file waterfall.c
 * @brief Simple waterfall display for audio PCM input
 *
 * Reads 16-bit signed mono PCM from stdin, displays FFT waterfall.
 * Usage: simple_am_receiver.exe -f 10 -i -o | waterfall.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include "kiss_fft.h"
#include "version.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

#define WINDOW_WIDTH    1024    /* Display width (2x for visibility) */
#define WINDOW_HEIGHT   800     /* Scrolling history (2x for visibility) */
#define FFT_SIZE        1024    /* FFT size (512 usable bins) */
#define SAMPLE_RATE     48000   /* Expected input sample rate */
#define HOP_SIZE        1024    /* TEMP: Full frame like yesterday */
#define DISPLAY_DECIMATION 1     /* TEMP: Display every frame */

/* Ring buffer for sliding window FFT */
static int16_t g_ring_buffer[FFT_SIZE];
static bool g_ring_initialized = false;

/* Frequency bins to monitor for WWV tick detection
 * Each has a center frequency and bandwidth based on signal characteristics:
 * - Pure tones (440, 500, 600 Hz): narrow bandwidth
 * - Short pulses (1000, 1200 Hz): wide bandwidth (energy spreads due to 5ms pulse)
 * - Subcarrier (100 Hz): tight to avoid DC noise
 * - Longer pulse (1500 Hz): moderate bandwidth (800ms pulse)
 */
#define NUM_TICK_FREQS  7
static const int   TICK_FREQS[NUM_TICK_FREQS] = { 100,   440,  500,  600,  1000, 1200, 1500 };
static const int   TICK_BW[NUM_TICK_FREQS]    = { 10,    5,    5,    5,    100,  100,  20   };  /* ± bandwidth in Hz */
static const char *TICK_NAMES[NUM_TICK_FREQS] = { "100Hz BCD", "440Hz Cal", "500Hz Min", "600Hz ID", "1000Hz Tick", "1200Hz WWVH", "1500Hz Tone" };

/*============================================================================
 * Color Mapping (magnitude to RGB) with auto-gain
 *============================================================================*/

/* Auto-gain state */
static float g_peak_db = -60.0f;      /* Tracked peak in dB */
static float g_floor_db = -60.0f;     /* Tracked noise floor in dB */
static float g_gain_offset = 0.0f;    /* Manual gain adjustment (+/- keys) */
#define AGC_ATTACK  0.1f              /* Fast attack for peaks */
#define AGC_DECAY   0.001f            /* Slow decay */

/* Tick detection state */
static float g_tick_thresholds[NUM_TICK_FREQS] = { 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f };
static int g_selected_param = 0;      /* 0 = gain, 1-7 = tick thresholds */

/*============================================================================
 * Tick Detector State Machine
 *============================================================================*/

/* Detection parameters */
#define TICK_MIN_DURATION_MS    2       /* Minimum tick duration (reject transients) */
#define TICK_MAX_DURATION_MS    50      /* Maximum tick duration (reject voice) */
#define TICK_COOLDOWN_MS        500     /* Debounce between ticks */
#define TICK_NOISE_ADAPT_RATE   0.001f  /* Slow adaptation of noise floor */
#define TICK_WARMUP_ADAPT_RATE  0.05f   /* Fast adaptation during warmup */
#define TICK_HYSTERESIS_RATIO   0.7f    /* threshold_low = threshold_high * this */
#define TICK_WARMUP_FRAMES      2000    /* ~1 second warmup (at 0.5ms/frame) */

/* Convert ms to frames (each frame advances HOP_SIZE samples) */
#define MS_TO_FRAMES(ms) ((int)((ms) * SAMPLE_RATE / 1000 / HOP_SIZE))

/* Convert samples to ms */
#define SAMPLES_TO_MS(samples) ((float)(samples) * 1000.0f / SAMPLE_RATE)

typedef enum {
    TICK_IDLE,          /* Waiting for energy rise */
    TICK_IN_TICK,       /* Energy above threshold */
    TICK_COOLDOWN       /* Debounce after detection */
} tick_state_t;

typedef struct {
    tick_state_t state;

    /* Adaptive thresholds */
    float noise_floor;          /* Tracked noise floor (slow adapt) */
    float threshold_high;       /* Trigger threshold = noise_floor * 2.0 */
    float threshold_low;        /* Exit threshold = threshold_high * 0.7 */

    /* Current tick measurement - sample-based for accuracy */
    uint64_t tick_start_sample; /* Sample when tick started */
    float tick_peak_energy;     /* Peak energy during tick */
    int tick_duration_samples;  /* Samples above threshold (accurate timing) */

    /* Statistics */
    int ticks_detected;         /* Total tick count */
    int ticks_rejected;         /* Rejected (wrong duration) */
    uint64_t last_tick_sample;  /* Sample of last valid tick */
    uint64_t start_frame;       /* Frame when detection started */
    uint64_t total_samples;     /* Total samples processed */
    int cooldown_frames;        /* Frames remaining in cooldown */
    bool warmup_complete;       /* Warmup period finished */

    /* Output */
    bool detection_enabled;     /* Toggle with 'D' key */
    FILE *csv_file;             /* Log file */
} tick_detector_t;

static tick_detector_t g_tick_detector;

static void tick_detector_init(tick_detector_t *td) {
    memset(td, 0, sizeof(*td));
    td->state = TICK_IDLE;
    td->noise_floor = 0.001f;   /* Start low, will adapt up during warmup */
    td->threshold_high = td->noise_floor * 2.0f;
    td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;
    td->detection_enabled = true;
    td->warmup_complete = false;
    td->total_samples = 0;
    td->csv_file = NULL;

    /* Open CSV log file */
    td->csv_file = fopen("wwv_ticks.csv", "w");
    if (td->csv_file) {
        fprintf(td->csv_file, "timestamp_ms,tick_num,energy_peak,duration_ms,interval_ms,noise_floor\n");
        fflush(td->csv_file);
    }
}

static void tick_detector_close(tick_detector_t *td) {
    if (td->csv_file) {
        fclose(td->csv_file);
        td->csv_file = NULL;
    }
}

/* Update detector with energy from 1000 Hz bucket. Returns true if tick just detected. */
static bool tick_detector_update(tick_detector_t *td, float energy, uint64_t frame_num) {
    if (!td->detection_enabled) return false;

    bool tick_detected = false;
    td->total_samples += HOP_SIZE;  /* Track total samples for accurate timing */

    /* Warmup period: fast noise floor adaptation, no detection */
    if (!td->warmup_complete) {
        td->noise_floor += TICK_WARMUP_ADAPT_RATE * (energy - td->noise_floor);
        if (td->noise_floor < 0.0001f) td->noise_floor = 0.0001f;
        td->threshold_high = td->noise_floor * 2.0f;
        td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;

        if (frame_num >= td->start_frame + TICK_WARMUP_FRAMES) {
            td->warmup_complete = true;
            printf("[WARMUP] Complete. Noise floor=%.6f, Threshold=%.6f\n",
                   td->noise_floor, td->threshold_high);
        }
        return false;
    }

    /* Normal operation: slow noise adaptation during idle */
    if (td->state == TICK_IDLE && energy < td->threshold_high) {
        td->noise_floor += TICK_NOISE_ADAPT_RATE * (energy - td->noise_floor);
        if (td->noise_floor < 0.0001f) td->noise_floor = 0.0001f;
        td->threshold_high = td->noise_floor * 2.0f;
        td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;
    }

    switch (td->state) {
        case TICK_IDLE:
            if (energy > td->threshold_high) {
                /* Rising edge - potential tick start */
                td->state = TICK_IN_TICK;
                td->tick_start_sample = td->total_samples;
                td->tick_peak_energy = energy;
                td->tick_duration_samples = HOP_SIZE;  /* First frame's worth */
            }
            break;

        case TICK_IN_TICK:
            td->tick_duration_samples += HOP_SIZE;  /* Add samples from this frame */
            if (energy > td->tick_peak_energy) {
                td->tick_peak_energy = energy;
            }

            if (energy < td->threshold_low) {
                /* Falling edge - tick ended, validate duration */
                float duration_ms = SAMPLES_TO_MS(td->tick_duration_samples);

                if (duration_ms >= TICK_MIN_DURATION_MS && duration_ms <= TICK_MAX_DURATION_MS) {
                    /* Valid tick! */
                    td->ticks_detected++;
                    tick_detected = true;

                    float timestamp_ms = SAMPLES_TO_MS(td->total_samples);
                    float interval_ms = 0.0f;
                    if (td->last_tick_sample > 0) {
                        interval_ms = SAMPLES_TO_MS(td->tick_start_sample - td->last_tick_sample);
                    }

                    /* Console output */
                    char indicator = (interval_ms > 950.0f && interval_ms < 1050.0f) ? ' ' : '!';
                    printf("[%7.1fs] TICK #%-4d  peak=%.4f  dur=%4.1fms  interval=%6.0fms %c\n",
                           timestamp_ms / 1000.0f, td->ticks_detected,
                           td->tick_peak_energy, duration_ms, interval_ms, indicator);

                    /* CSV output */
                    if (td->csv_file) {
                        fprintf(td->csv_file, "%.1f,%d,%.6f,%.1f,%.0f,%.6f\n",
                                timestamp_ms, td->ticks_detected,
                                td->tick_peak_energy, duration_ms, interval_ms, td->noise_floor);
                        fflush(td->csv_file);
                    }

                    td->last_tick_sample = td->tick_start_sample;
                } else {
                    /* Invalid duration - reject */
                    td->ticks_rejected++;
                }

                td->state = TICK_COOLDOWN;
                td->cooldown_frames = MS_TO_FRAMES(TICK_COOLDOWN_MS);
            }
            else if (SAMPLES_TO_MS(td->tick_duration_samples) > TICK_MAX_DURATION_MS) {
                /* Exceeded max duration - abort (probably voice) */
                td->ticks_rejected++;
                td->state = TICK_COOLDOWN;
                td->cooldown_frames = MS_TO_FRAMES(TICK_COOLDOWN_MS);
            }
            break;

        case TICK_COOLDOWN:
            td->cooldown_frames--;
            if (td->cooldown_frames <= 0) {
                td->state = TICK_IDLE;
            }
            break;
    }

    return tick_detected;
}

static void tick_detector_print_stats(tick_detector_t *td, uint64_t current_frame) {
    (void)current_frame;  /* Use sample-based timing instead */
    float elapsed_sec = SAMPLES_TO_MS(td->total_samples) / 1000.0f;
    float detection_sec = td->warmup_complete ?
        (elapsed_sec - (TICK_WARMUP_FRAMES * HOP_SIZE * 1000.0f / SAMPLE_RATE / 1000.0f)) : 0.0f;
    int expected = (int)detection_sec;
    float hit_rate = (expected > 0) ? (100.0f * td->ticks_detected / expected) : 0.0f;

    printf("\n=== TICK DETECTION STATS ===\n");
    printf("Elapsed:   %.1f sec (%.1f sec detecting)\n", elapsed_sec, detection_sec);
    printf("Detected:  %d ticks\n", td->ticks_detected);
    printf("Expected:  ~%d ticks (1/sec)\n", expected);
    printf("Hit rate:  %.1f%%\n", hit_rate);
    printf("Rejected:  %d (wrong duration)\n", td->ticks_rejected);
    printf("Noise flr: %.6f\n", td->noise_floor);
    printf("Threshold: %.6f (high), %.6f (low)\n", td->threshold_high, td->threshold_low);
    printf("Warmup:    %s\n", td->warmup_complete ? "complete" : "in progress");
    printf("============================\n\n");
}

static void magnitude_to_rgb(float mag, float peak_db, float floor_db, uint8_t *r, uint8_t *g, uint8_t *b) {
    /* Log scale for better visibility */
    float db = 20.0f * log10f(mag + 1e-10f);

    /* Apply manual gain offset */
    db += g_gain_offset;

    /* Map dB to 0-1 range using tracked peak and floor */
    float range = peak_db - floor_db;
    if (range < 20.0f) range = 20.0f;  /* Minimum 20 dB range */

    float norm = (db - floor_db) / range;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;

    /* Blue -> Cyan -> Green -> Yellow -> Red */
    if (norm < 0.25f) {
        /* Black to Blue */
        *r = 0;
        *g = 0;
        *b = (uint8_t)(norm * 4.0f * 255.0f);
    } else if (norm < 0.5f) {
        /* Blue to Cyan */
        *r = 0;
        *g = (uint8_t)((norm - 0.25f) * 4.0f * 255.0f);
        *b = 255;
    } else if (norm < 0.75f) {
        /* Cyan to Yellow */
        *r = (uint8_t)((norm - 0.5f) * 4.0f * 255.0f);
        *g = 255;
        *b = (uint8_t)((0.75f - norm) * 4.0f * 255.0f);
    } else {
        /* Yellow to Red */
        *r = 255;
        *g = (uint8_t)((1.0f - norm) * 4.0f * 255.0f);
        *b = 0;
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    print_version("Phoenix SDR - Waterfall");

#ifdef _WIN32
    /* Set stdin to binary mode */
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Waterfall",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_SHOWN
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Texture *texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        WINDOW_WIDTH, WINDOW_HEIGHT
    );
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Initialize KissFFT */
    kiss_fft_cfg fft_cfg = kiss_fft_alloc(FFT_SIZE, 0, NULL, NULL);
    if (!fft_cfg) {
        fprintf(stderr, "kiss_fft_alloc failed\n");
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Allocate buffers */
    kiss_fft_cpx *fft_in = (kiss_fft_cpx *)malloc(FFT_SIZE * sizeof(kiss_fft_cpx));
    kiss_fft_cpx *fft_out = (kiss_fft_cpx *)malloc(FFT_SIZE * sizeof(kiss_fft_cpx));
    uint8_t *pixels = (uint8_t *)malloc(WINDOW_WIDTH * WINDOW_HEIGHT * 3);
    float *magnitudes = (float *)malloc(WINDOW_WIDTH * sizeof(float));

    if (!fft_in || !fft_out || !pixels || !magnitudes) {
        fprintf(stderr, "Memory allocation failed\n");
        return 1;
    }

    /* Clear pixel buffer */
    memset(pixels, 0, WINDOW_WIDTH * WINDOW_HEIGHT * 3);

    /* Hanning window */
    float *window_func = (float *)malloc(FFT_SIZE * sizeof(float));
    for (int i = 0; i < FFT_SIZE; i++) {
        window_func[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (FFT_SIZE - 1)));
    }

    printf("Waterfall display ready. Reading from stdin...\n");
    printf("Window: %dx%d, FFT: %d bins (%.1f Hz/bin)\n",
           WINDOW_WIDTH, WINDOW_HEIGHT, FFT_SIZE / 2, (float)SAMPLE_RATE / FFT_SIZE);
    printf("Timing: Hop=%d (%.2fms), Display=1:%d (~%d fps)\n",
           HOP_SIZE, (float)HOP_SIZE * 1000.0f / SAMPLE_RATE,
           DISPLAY_DECIMATION, (int)(SAMPLE_RATE / HOP_SIZE / DISPLAY_DECIMATION));
    printf("Keys: 0=gain, 1-7=tick thresholds, +/- adjust, D=detect, S=stats, Q/Esc quit\n");
    printf("Buckets: 1:100Hz 2:440Hz 3:500Hz 4:600Hz 5:1000Hz 6:1200Hz 7:1500Hz\n");

    /* Initialize tick detector */
    tick_detector_init(&g_tick_detector);
    uint64_t frame_num = 0;
    int display_frame_counter = 0;
    g_tick_detector.start_frame = 0;
    printf("\nTick detection ENABLED - watching 1000 Hz bucket\n");
    printf("Logging to wwv_ticks.csv\n\n");

    bool running = true;

    while (running) {
        /* Handle SDL events */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    running = false;
                } else if (event.key.keysym.sym == SDLK_PLUS ||
                           event.key.keysym.sym == SDLK_EQUALS ||
                           event.key.keysym.sym == SDLK_KP_PLUS) {
                    if (g_selected_param == 0) {
                        g_gain_offset += 3.0f;
                        printf("Gain: %+.0f dB\n", g_gain_offset);
                    } else {
                        int idx = g_selected_param - 1;
                        g_tick_thresholds[idx] *= 1.5f;
                        printf("%s threshold: %.4f\n", TICK_NAMES[idx], g_tick_thresholds[idx]);
                    }
                } else if (event.key.keysym.sym == SDLK_MINUS ||
                           event.key.keysym.sym == SDLK_KP_MINUS) {
                    if (g_selected_param == 0) {
                        g_gain_offset -= 3.0f;
                        printf("Gain: %+.0f dB\n", g_gain_offset);
                    } else {
                        int idx = g_selected_param - 1;
                        g_tick_thresholds[idx] /= 1.5f;
                        if (g_tick_thresholds[idx] < 0.0001f) g_tick_thresholds[idx] = 0.0001f;
                        printf("%s threshold: %.4f\n", TICK_NAMES[idx], g_tick_thresholds[idx]);
                    }
                } else if (event.key.keysym.sym == SDLK_0 || event.key.keysym.sym == SDLK_KP_0) {
                    g_selected_param = 0;
                    printf("Selected: Gain (%+.0f dB)\n", g_gain_offset);
                } else if (event.key.keysym.sym == SDLK_1 || event.key.keysym.sym == SDLK_KP_1) {
                    g_selected_param = 1;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[0], g_tick_thresholds[0]);
                } else if (event.key.keysym.sym == SDLK_2 || event.key.keysym.sym == SDLK_KP_2) {
                    g_selected_param = 2;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[1], g_tick_thresholds[1]);
                } else if (event.key.keysym.sym == SDLK_3 || event.key.keysym.sym == SDLK_KP_3) {
                    g_selected_param = 3;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[2], g_tick_thresholds[2]);
                } else if (event.key.keysym.sym == SDLK_4 || event.key.keysym.sym == SDLK_KP_4) {
                    g_selected_param = 4;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[3], g_tick_thresholds[3]);
                } else if (event.key.keysym.sym == SDLK_5 || event.key.keysym.sym == SDLK_KP_5) {
                    g_selected_param = 5;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[4], g_tick_thresholds[4]);
                } else if (event.key.keysym.sym == SDLK_6 || event.key.keysym.sym == SDLK_KP_6) {
                    g_selected_param = 6;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[5], g_tick_thresholds[5]);
                } else if (event.key.keysym.sym == SDLK_7 || event.key.keysym.sym == SDLK_KP_7) {
                    g_selected_param = 7;
                    printf("Selected: %s (threshold: %.4f)\n", TICK_NAMES[6], g_tick_thresholds[6]);
                } else if (event.key.keysym.sym == SDLK_d) {
                    g_tick_detector.detection_enabled = !g_tick_detector.detection_enabled;
                    printf("Tick detection: %s\n", g_tick_detector.detection_enabled ? "ENABLED" : "DISABLED");
                } else if (event.key.keysym.sym == SDLK_s) {
                    tick_detector_print_stats(&g_tick_detector, frame_num);
                }
            }
        }

        /* Sliding window: read HOP_SIZE new samples, slide ring buffer */
        if (!g_ring_initialized) {
            /* First time: fill the entire ring buffer */
            size_t read_count = fread(g_ring_buffer, sizeof(int16_t), FFT_SIZE, stdin);
            if (read_count < FFT_SIZE) {
                if (feof(stdin)) {
                    printf("End of input (initial fill)\n");
                    SDL_Delay(100);
                    continue;
                }
                memset(g_ring_buffer + read_count, 0, (FFT_SIZE - read_count) * sizeof(int16_t));
            }
            g_ring_initialized = true;
        } else {
            /* Subsequent: slide window by HOP_SIZE */
            int16_t new_samples[HOP_SIZE];
            size_t read_count = fread(new_samples, sizeof(int16_t), HOP_SIZE, stdin);
            if (read_count < HOP_SIZE) {
                if (feof(stdin)) {
                    printf("End of input\n");
                    /* Keep window open but stop reading */
                    SDL_Delay(100);
                    continue;
                }
                /* Pad with zeros if partial read */
                memset(new_samples + read_count, 0, (HOP_SIZE - read_count) * sizeof(int16_t));
            }
            /* Slide the buffer: move old data left, append new samples at end */
            memmove(g_ring_buffer, g_ring_buffer + HOP_SIZE, (FFT_SIZE - HOP_SIZE) * sizeof(int16_t));
            memcpy(g_ring_buffer + FFT_SIZE - HOP_SIZE, new_samples, HOP_SIZE * sizeof(int16_t));
        }

        /* Convert to complex and apply window */
        for (int i = 0; i < FFT_SIZE; i++) {
            fft_in[i].r = (g_ring_buffer[i] / 32768.0f) * window_func[i];
            fft_in[i].i = 0.0f;
        }

        /* Run FFT */
        kiss_fft(fft_cfg, fft_in, fft_out);

        /* Calculate magnitudes with FFT shift (DC in center) */
        for (int i = 0; i < WINDOW_WIDTH; i++) {
            int bin;
            if (i < WINDOW_WIDTH / 2) {
                /* Left half: negative frequencies */
                bin = FFT_SIZE / 2 + i;
            } else {
                /* Right half: positive frequencies */
                bin = i - WINDOW_WIDTH / 2;
            }
            /* Wrap around */
            if (bin < 0) bin += FFT_SIZE;
            if (bin >= FFT_SIZE) bin -= FFT_SIZE;

            /* Bounds check */
            if (bin >= 0 && bin < FFT_SIZE) {
                float re = fft_out[bin].r;
                float im = fft_out[bin].i;
                magnitudes[i] = sqrtf(re * re + im * im) / FFT_SIZE;
            } else {
                magnitudes[i] = 0.0f;
            }
        }

        /* === TICK DETECTION (runs at full rate - every 0.5ms) === */
        float hz_per_bin = (float)SAMPLE_RATE / FFT_SIZE;
        float tick_energy_1000hz = 0.0f;  /* Store for display marker */
        
        for (int f = 0; f < NUM_TICK_FREQS; f++) {
            int freq = TICK_FREQS[f];
            int bandwidth = TICK_BW[f];

            /* Calculate center bin and how many bins to sum based on bandwidth */
            int center_bin = (int)(freq / hz_per_bin + 0.5f);
            int bin_span = (int)(bandwidth / hz_per_bin + 0.5f);
            if (bin_span < 1) bin_span = 1;

            /* Sum energy across bandwidth for both sidebands */
            float pos_energy = 0.0f, neg_energy = 0.0f;
            for (int b = -bin_span; b <= bin_span; b++) {
                int pos_bin = center_bin + b;
                int neg_bin = FFT_SIZE - center_bin + b;

                if (pos_bin >= 0 && pos_bin < FFT_SIZE) {
                    float re = fft_out[pos_bin].r;
                    float im = fft_out[pos_bin].i;
                    pos_energy += sqrtf(re * re + im * im) / FFT_SIZE;
                }
                if (neg_bin >= 0 && neg_bin < FFT_SIZE) {
                    float re = fft_out[neg_bin].r;
                    float im = fft_out[neg_bin].i;
                    neg_energy += sqrtf(re * re + im * im) / FFT_SIZE;
                }
            }

            float combined_energy = pos_energy + neg_energy;

            /* Feed 1000 Hz bucket (index 4) to tick detector - ALWAYS */
            if (f == 4) {
                tick_detector_update(&g_tick_detector, combined_energy, frame_num);
                tick_energy_1000hz = combined_energy;
            }
        }

        /* === DISPLAY UPDATE (decimated - every DISPLAY_DECIMATION frames) === */
        display_frame_counter++;
        if (display_frame_counter >= DISPLAY_DECIMATION) {
            display_frame_counter = 0;

            /* Auto-gain: track peak and floor (runs at display rate, not detection rate) */
            /* Include g_gain_offset so AGC tracks what magnitude_to_rgb actually sees */
            float frame_max = -200.0f;
            float frame_min = 200.0f;
            for (int i = 0; i < WINDOW_WIDTH; i++) {
                float db = 20.0f * log10f(magnitudes[i] + 1e-10f) + g_gain_offset;
                if (db > frame_max) frame_max = db;
                if (db < frame_min) frame_min = db;
            }
            /* Update tracked values with attack/decay */
            if (frame_max > g_peak_db) {
                g_peak_db = g_peak_db + AGC_ATTACK * (frame_max - g_peak_db);
            } else {
                g_peak_db = g_peak_db + AGC_DECAY * (frame_max - g_peak_db);
            }
            if (frame_min < g_floor_db) {
                g_floor_db = g_floor_db + AGC_ATTACK * (frame_min - g_floor_db);
            } else {
                g_floor_db = g_floor_db + AGC_DECAY * (frame_min - g_floor_db);
            }

            /* Scroll pixels down by 1 row */
            memmove(pixels + WINDOW_WIDTH * 3,  /* dest: row 1 */
                    pixels,                      /* src: row 0 */
                    WINDOW_WIDTH * (WINDOW_HEIGHT - 1) * 3);

            /* Draw new row at top (row 0) */
            for (int x = 0; x < WINDOW_WIDTH; x++) {
                uint8_t r, g, b;
                magnitude_to_rgb(magnitudes[x], g_peak_db, g_floor_db, &r, &g, &b);
                pixels[x * 3 + 0] = r;
                pixels[x * 3 + 1] = g;
                pixels[x * 3 + 2] = b;
            }

            /* Draw tick threshold markers for all frequencies */
            for (int f = 0; f < NUM_TICK_FREQS; f++) {
                int freq = TICK_FREQS[f];
                int center_bin = (int)(freq / hz_per_bin + 0.5f);
                int bin_span = (int)(TICK_BW[f] / hz_per_bin + 0.5f);
                if (bin_span < 1) bin_span = 1;

                /* Recalculate energy for display (or use cached for 1000Hz) */
                float combined_energy = 0.0f;
                if (f == 4) {
                    combined_energy = tick_energy_1000hz;
                } else {
                    for (int b = -bin_span; b <= bin_span; b++) {
                        int pos_bin = center_bin + b;
                        int neg_bin = FFT_SIZE - center_bin + b;
                        if (pos_bin >= 0 && pos_bin < FFT_SIZE) {
                            float re = fft_out[pos_bin].r;
                            float im = fft_out[pos_bin].i;
                            combined_energy += sqrtf(re * re + im * im) / FFT_SIZE;
                        }
                        if (neg_bin >= 0 && neg_bin < FFT_SIZE) {
                            float re = fft_out[neg_bin].r;
                            float im = fft_out[neg_bin].i;
                            combined_energy += sqrtf(re * re + im * im) / FFT_SIZE;
                        }
                    }
                }

                /* If above threshold, draw marker dot at the frequency position */
                if (combined_energy > g_tick_thresholds[f]) {
                    int x_pos = WINDOW_WIDTH / 2 + center_bin;
                    int x_neg = WINDOW_WIDTH / 2 - center_bin;

                    if (x_pos >= 0 && x_pos < WINDOW_WIDTH) {
                        pixels[x_pos * 3 + 0] = 255;  /* R */
                        pixels[x_pos * 3 + 1] = 0;    /* G */
                        pixels[x_pos * 3 + 2] = 0;    /* B */
                    }
                    if (x_neg >= 0 && x_neg < WINDOW_WIDTH) {
                        pixels[x_neg * 3 + 0] = 255;  /* R */
                        pixels[x_neg * 3 + 1] = 0;    /* G */
                        pixels[x_neg * 3 + 2] = 0;    /* B */
                    }
                }
            }

            /* Draw selection indicator */
            {
                int indicator_x = 10 + g_selected_param * 20;
                if (indicator_x < WINDOW_WIDTH) {
                    pixels[indicator_x * 3 + 0] = 255;  /* Cyan indicator */
                    pixels[indicator_x * 3 + 1] = 255;
                    pixels[indicator_x * 3 + 2] = 0;
                }
            }

            /* Update texture and render */
            SDL_UpdateTexture(texture, NULL, pixels, WINDOW_WIDTH * 3);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);
        }

        frame_num++;
    }

    /* Print final stats */
    printf("\n");
    tick_detector_print_stats(&g_tick_detector, frame_num);
    tick_detector_close(&g_tick_detector);

    /* Cleanup */
    free(window_func);
    free(magnitudes);
    free(pixels);
    free(fft_out);
    free(fft_in);
    kiss_fft_free(fft_cfg);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("Done.\n");
    return 0;
}
