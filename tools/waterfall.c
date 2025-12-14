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

#define WATERFALL_WIDTH 1024    /* Left panel: waterfall display */
#define BUCKET_WIDTH    200     /* Right panel: bucket bars */
#define WINDOW_WIDTH    (WATERFALL_WIDTH + BUCKET_WIDTH)  /* Total width */
#define WINDOW_HEIGHT   800     /* Scrolling history */
#define FFT_SIZE        1024    /* FFT size (512 usable bins) */
#define SAMPLE_RATE     48000   /* Expected input sample rate */

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
static float g_bucket_energy[NUM_TICK_FREQS];  /* Current energy in each bucket */
static int g_selected_param = 0;      /* 0 = gain, 1-6 = tick thresholds */

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
    int16_t *pcm_buffer = (int16_t *)malloc(FFT_SIZE * sizeof(int16_t));
    kiss_fft_cpx *fft_in = (kiss_fft_cpx *)malloc(FFT_SIZE * sizeof(kiss_fft_cpx));
    kiss_fft_cpx *fft_out = (kiss_fft_cpx *)malloc(FFT_SIZE * sizeof(kiss_fft_cpx));
    uint8_t *pixels = (uint8_t *)malloc(WINDOW_WIDTH * WINDOW_HEIGHT * 3);
    float *magnitudes = (float *)malloc(WATERFALL_WIDTH * sizeof(float));

    if (!pcm_buffer || !fft_in || !fft_out || !pixels || !magnitudes) {
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
    printf("Window: %dx%d, FFT: %d bins (%.1f Hz/bin)\n", WINDOW_WIDTH, WINDOW_HEIGHT, FFT_SIZE / 2, (float)SAMPLE_RATE / FFT_SIZE);
    printf("Keys: 0=gain, 1-7=tick thresholds, +/- adjust, Q/Esc quit\n");
    printf("1:100Hz(±10) 2:440Hz(±5) 3:500Hz(±5) 4:600Hz(±5) 5:1000Hz(±100) 6:1200Hz(±100) 7:1500Hz(±20)\n");

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
                }
            }
        }

        /* Read PCM samples from stdin */
        size_t read_count = fread(pcm_buffer, sizeof(int16_t), FFT_SIZE, stdin);
        if (read_count < FFT_SIZE) {
            if (feof(stdin)) {
                printf("End of input\n");
                /* Keep window open but stop reading */
                SDL_Delay(100);
                continue;
            }
            /* Pad with zeros if partial read */
            memset(pcm_buffer + read_count, 0, (FFT_SIZE - read_count) * sizeof(int16_t));
        }

        /* Convert to complex and apply window */
        for (int i = 0; i < FFT_SIZE; i++) {
            fft_in[i].r = (pcm_buffer[i] / 32768.0f) * window_func[i];
            fft_in[i].i = 0.0f;
        }

        /* Run FFT */
        kiss_fft(fft_cfg, fft_in, fft_out);

        /* Calculate magnitudes with FFT shift (DC in center) */
        for (int i = 0; i < WATERFALL_WIDTH; i++) {
            int bin;
            if (i < WATERFALL_WIDTH / 2) {
                /* Left half: negative frequencies */
                bin = FFT_SIZE / 2 + i;
            } else {
                /* Right half: positive frequencies */
                bin = i - WATERFALL_WIDTH / 2;
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

        /* Auto-gain: track peak and floor */
        float frame_max = -200.0f;
        float frame_min = 200.0f;
        for (int i = 0; i < WATERFALL_WIDTH; i++) {
            float db = 20.0f * log10f(magnitudes[i] + 1e-10f);
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

        /* Draw new row at top (row 0) - WATERFALL ONLY */
        for (int x = 0; x < WATERFALL_WIDTH; x++) {
            uint8_t r, g, b;
            magnitude_to_rgb(magnitudes[x], g_peak_db, g_floor_db, &r, &g, &b);
            pixels[x * 3 + 0] = r;
            pixels[x * 3 + 1] = g;
            pixels[x * 3 + 2] = b;
        }

        /* Tick detection: check each frequency and mark with colored dot if above threshold */
        float hz_per_bin = (float)SAMPLE_RATE / FFT_SIZE;
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
            g_bucket_energy[f] = combined_energy;  /* Store for right panel display */

            /* If above threshold, draw marker dot at the frequency position */
            if (combined_energy > g_tick_thresholds[f]) {
                /* Calculate x position in FFT-shifted display */
                /* Positive freq: x = WATERFALL_WIDTH/2 + center_bin */
                int x_pos = WATERFALL_WIDTH / 2 + center_bin;
                int x_neg = WATERFALL_WIDTH / 2 - center_bin;

                /* Draw red dot at positive frequency */
                if (x_pos >= 0 && x_pos < WATERFALL_WIDTH) {
                    pixels[x_pos * 3 + 0] = 255;  /* R */
                    pixels[x_pos * 3 + 1] = 0;    /* G */
                    pixels[x_pos * 3 + 2] = 0;    /* B */
                }
                /* Draw red dot at negative frequency */
                if (x_neg >= 0 && x_neg < WATERFALL_WIDTH) {
                    pixels[x_neg * 3 + 0] = 255;  /* R */
                    pixels[x_neg * 3 + 1] = 0;    /* G */
                    pixels[x_neg * 3 + 2] = 0;    /* B */
                }
            }
        }

        /* Draw selection indicator at bottom of first row */
        /* Small colored tick at the selected parameter position */
        {
            int indicator_x = 10 + g_selected_param * 20;
            if (indicator_x < WATERFALL_WIDTH) {
                pixels[indicator_x * 3 + 0] = 255;  /* Cyan indicator */
                pixels[indicator_x * 3 + 1] = 255;
                pixels[indicator_x * 3 + 2] = 0;
            }
        }

        /* === RIGHT PANEL: Bucket energy bars === */
        {
            int bar_width = BUCKET_WIDTH / NUM_TICK_FREQS;  /* ~28 pixels per bar */
            int bar_gap = 2;  /* Gap between bars */
            
            /* Clear right panel (black background) */
            for (int y = 0; y < WINDOW_HEIGHT; y++) {
                for (int x = WATERFALL_WIDTH; x < WINDOW_WIDTH; x++) {
                    int idx = (y * WINDOW_WIDTH + x) * 3;
                    pixels[idx + 0] = 0;
                    pixels[idx + 1] = 0;
                    pixels[idx + 2] = 0;
                }
            }
            
            /* Draw each bucket bar */
            for (int f = 0; f < NUM_TICK_FREQS; f++) {
                int bar_x = WATERFALL_WIDTH + f * bar_width + bar_gap;
                int bar_w = bar_width - bar_gap * 2;
                
                /* Convert energy to height using log scale */
                float db = 20.0f * log10f(g_bucket_energy[f] + 1e-10f);
                float norm = (db - g_floor_db) / (g_peak_db - g_floor_db + 0.1f);
                if (norm < 0.0f) norm = 0.0f;
                if (norm > 1.0f) norm = 1.0f;
                
                int bar_height = (int)(norm * WINDOW_HEIGHT);
                
                /* Get color based on magnitude */
                uint8_t r, g, b;
                magnitude_to_rgb(g_bucket_energy[f], g_peak_db, g_floor_db, &r, &g, &b);
                
                /* Draw bar from bottom up */
                for (int y = WINDOW_HEIGHT - bar_height; y < WINDOW_HEIGHT; y++) {
                    for (int x = bar_x; x < bar_x + bar_w && x < WINDOW_WIDTH; x++) {
                        int idx = (y * WINDOW_WIDTH + x) * 3;
                        pixels[idx + 0] = r;
                        pixels[idx + 1] = g;
                        pixels[idx + 2] = b;
                    }
                }
            }
        }

        /* Update texture */
        SDL_UpdateTexture(texture, NULL, pixels, WINDOW_WIDTH * 3);

        /* Render */
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);

        SDL_RenderPresent(renderer);
    }

    /* Cleanup */
    free(window_func);
    free(magnitudes);
    free(pixels);
    free(fft_out);
    free(fft_in);
    free(pcm_buffer);
    kiss_fft_free(fft_cfg);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("Done.\n");
    return 0;
}
