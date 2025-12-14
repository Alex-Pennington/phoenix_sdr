/**
 * @file wormhole.c
 * @brief 3D I/Q constellation display for MIL-STD-188-110A PSK signals
 *
 * Reads 16-bit signed mono PCM from stdin (expected 9600 Hz, 110A audio),
 * converts to I/Q via Hilbert transform, displays as 3D constellation trajectory.
 * 
 * Usage: type signal.pcm | wormhole.exe
 *        Get-Content -Raw signal.pcm | wormhole.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include "version.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

#define WINDOW_WIDTH    800
#define WINDOW_HEIGHT   800
#define SAMPLE_RATE     48000       /* PCM sample rate from tx_pcm_out */
#define M110A_CENTER_HZ 1800        /* MIL-188-110A center frequency */
#define M110A_SYMBOL_RATE 2400      /* Symbols per second */
#define SAMPLES_PER_SYMBOL (SAMPLE_RATE / M110A_SYMBOL_RATE)  /* 20 at 48kHz */

/* Trail history */
#define TRAIL_LENGTH    2000        /* How many points to keep in the wormhole */
#define FADE_START      0.3f        /* Where fade begins (0=newest, 1=oldest) */

/* Display scaling */
#define IQ_SCALE        300.0f      /* Pixels per unit I/Q magnitude */
#define Z_DEPTH         400.0f      /* Perspective depth */
#define Z_SCALE         0.3f        /* How much Z affects position */

/*============================================================================
 * Hilbert Transform for I/Q extraction
 *============================================================================*/

#define HILBERT_TAPS    31
static float g_hilbert_coeffs[HILBERT_TAPS];
static float g_hilbert_delay[HILBERT_TAPS];
static float g_delay_line[HILBERT_TAPS];  /* For I channel (delayed real) */

static void hilbert_init(void) {
    memset(g_hilbert_delay, 0, sizeof(g_hilbert_delay));
    memset(g_delay_line, 0, sizeof(g_delay_line));
    
    /* Hilbert FIR coefficients (odd taps only, others are zero) */
    int center = HILBERT_TAPS / 2;
    for (int i = 0; i < HILBERT_TAPS; i++) {
        int n = i - center;
        if (n == 0) {
            g_hilbert_coeffs[i] = 0.0f;
        } else if (n % 2 != 0) {
            /* Odd taps: 2/(pi*n) */
            g_hilbert_coeffs[i] = 2.0f / (3.14159265f * n);
        } else {
            g_hilbert_coeffs[i] = 0.0f;
        }
        /* Apply Hamming window */
        float w = 0.54f - 0.46f * cosf(2.0f * 3.14159265f * i / (HILBERT_TAPS - 1));
        g_hilbert_coeffs[i] *= w;
    }
}

/* Process one sample, returns I and Q */
static void hilbert_process(float sample, float *out_i, float *out_q) {
    /* Shift delay lines */
    memmove(g_hilbert_delay + 1, g_hilbert_delay, (HILBERT_TAPS - 1) * sizeof(float));
    memmove(g_delay_line + 1, g_delay_line, (HILBERT_TAPS - 1) * sizeof(float));
    
    g_hilbert_delay[0] = sample;
    g_delay_line[0] = sample;
    
    /* Q = Hilbert transform output */
    float q = 0.0f;
    for (int i = 0; i < HILBERT_TAPS; i++) {
        q += g_hilbert_delay[i] * g_hilbert_coeffs[i];
    }
    
    /* I = delayed original (group delay compensation) */
    *out_i = g_delay_line[HILBERT_TAPS / 2];
    *out_q = q;
}

/*============================================================================
 * Mixer - shift to baseband (remove 1800 Hz carrier)
 *============================================================================*/

static float g_mix_phase = 0.0f;
static const float MIX_FREQ = (float)M110A_CENTER_HZ;

static void mix_to_baseband(float i_in, float q_in, float *i_out, float *q_out) {
    float cos_p = cosf(g_mix_phase);
    float sin_p = sinf(g_mix_phase);
    
    /* Complex multiply by e^(-j*phase) to shift down */
    *i_out = i_in * cos_p + q_in * sin_p;
    *q_out = q_in * cos_p - i_in * sin_p;
    
    /* Advance phase */
    g_mix_phase += 2.0f * 3.14159265f * MIX_FREQ / SAMPLE_RATE;
    if (g_mix_phase > 2.0f * 3.14159265f) {
        g_mix_phase -= 2.0f * 3.14159265f;
    }
}

/*============================================================================
 * Low-pass filter after mixing
 *============================================================================*/

#define LPF_TAPS    21
static float g_lpf_coeffs[LPF_TAPS];
static float g_lpf_i[LPF_TAPS];
static float g_lpf_q[LPF_TAPS];

static void lpf_init(float cutoff_hz) {
    memset(g_lpf_i, 0, sizeof(g_lpf_i));
    memset(g_lpf_q, 0, sizeof(g_lpf_q));
    
    float fc = cutoff_hz / SAMPLE_RATE;
    int center = LPF_TAPS / 2;
    
    for (int i = 0; i < LPF_TAPS; i++) {
        int n = i - center;
        if (n == 0) {
            g_lpf_coeffs[i] = 2.0f * fc;
        } else {
            g_lpf_coeffs[i] = sinf(2.0f * 3.14159265f * fc * n) / (3.14159265f * n);
        }
        /* Hamming window */
        float w = 0.54f - 0.46f * cosf(2.0f * 3.14159265f * i / (LPF_TAPS - 1));
        g_lpf_coeffs[i] *= w;
    }
}

static void lpf_process(float i_in, float q_in, float *i_out, float *q_out) {
    memmove(g_lpf_i + 1, g_lpf_i, (LPF_TAPS - 1) * sizeof(float));
    memmove(g_lpf_q + 1, g_lpf_q, (LPF_TAPS - 1) * sizeof(float));
    g_lpf_i[0] = i_in;
    g_lpf_q[0] = q_in;
    
    float si = 0.0f, sq = 0.0f;
    for (int i = 0; i < LPF_TAPS; i++) {
        si += g_lpf_i[i] * g_lpf_coeffs[i];
        sq += g_lpf_q[i] * g_lpf_coeffs[i];
    }
    *i_out = si;
    *q_out = sq;
}

/*============================================================================
 * Trail buffer (the "wormhole")
 *============================================================================*/

typedef struct {
    float i, q;     /* Baseband I/Q */
    float mag;      /* Magnitude for coloring */
} trail_point_t;

static trail_point_t g_trail[TRAIL_LENGTH];
static int g_trail_head = 0;
static int g_trail_count = 0;

static void trail_add(float i, float q) {
    g_trail[g_trail_head].i = i;
    g_trail[g_trail_head].q = q;
    g_trail[g_trail_head].mag = sqrtf(i*i + q*q);
    
    g_trail_head = (g_trail_head + 1) % TRAIL_LENGTH;
    if (g_trail_count < TRAIL_LENGTH) g_trail_count++;
}

/*============================================================================
 * 3D Projection
 *============================================================================*/

static float g_rotation_y = 0.0f;       /* Y-axis rotation (horizontal spin) */
static float g_rotation_x = 0.3f;       /* X-axis tilt (look down into wormhole) */
static float g_zoom = 1.0f;
static float g_time_scale = 1.0f;       /* Time speed multiplier (A/Z keys) */
static float g_tilt_angle = 0.3f;       /* View tilt angle (S/X keys) */

static void project_3d(float i, float q, float z_norm, int *screen_x, int *screen_y) {
    /* z_norm: 0 = newest (front), 1 = oldest (back) */
    float z = z_norm * Z_DEPTH;
    
    /* Apply Y rotation (spin around vertical axis) */
    float cos_y = cosf(g_rotation_y);
    float sin_y = sinf(g_rotation_y);
    float x1 = i * cos_y - z * sin_y * Z_SCALE;
    float z1 = i * sin_y + z * cos_y * Z_SCALE;
    float y1 = q;
    
    /* Apply X rotation (tilt) */
    float cos_x = cosf(g_rotation_x);
    float sin_x = sinf(g_rotation_x);
    float y2 = y1 * cos_x - z1 * sin_x;
    float z2 = y1 * sin_x + z1 * cos_x;
    
    /* Perspective projection */
    float perspective = 1.0f + z2 * 0.001f;
    if (perspective < 0.1f) perspective = 0.1f;
    
    float scale = IQ_SCALE * g_zoom / perspective;
    
    *screen_x = WINDOW_WIDTH / 2 + (int)(x1 * scale);
    *screen_y = WINDOW_HEIGHT / 2 - (int)(y2 * scale);
}

/*============================================================================
 * Color mapping based on phase angle
 *============================================================================*/

static void phase_to_color(float i, float q, float age, uint8_t *r, uint8_t *g, uint8_t *b) {
    /* Phase angle -> hue */
    float phase = atan2f(q, i);  /* -pi to +pi */
    float hue = (phase + 3.14159265f) / (2.0f * 3.14159265f);  /* 0 to 1 */
    
    /* Magnitude -> saturation */
    float mag = sqrtf(i*i + q*q);
    float sat = mag * 5.0f;  /* Scale up */
    if (sat > 1.0f) sat = 1.0f;
    
    /* Age -> brightness fade */
    float val = 1.0f;
    if (age > FADE_START) {
        val = 1.0f - (age - FADE_START) / (1.0f - FADE_START);
    }
    val = val * 0.8f + 0.2f;  /* Don't fade to complete black */
    
    /* HSV to RGB */
    float h = hue * 6.0f;
    int hi = (int)h % 6;
    float f = h - (int)h;
    float p = val * (1.0f - sat);
    float t = val * (1.0f - sat * (1.0f - f));
    float s = val * (1.0f - sat * f);
    
    float rf, gf, bf;
    switch (hi) {
        case 0: rf = val; gf = t; bf = p; break;
        case 1: rf = s; gf = val; bf = p; break;
        case 2: rf = p; gf = val; bf = t; break;
        case 3: rf = p; gf = s; bf = val; break;
        case 4: rf = t; gf = p; bf = val; break;
        default: rf = val; gf = p; bf = s; break;
    }
    
    *r = (uint8_t)(rf * 255.0f);
    *g = (uint8_t)(gf * 255.0f);
    *b = (uint8_t)(bf * 255.0f);
}

/*============================================================================
 * Draw constellation reference
 *============================================================================*/

static void draw_constellation_grid(uint8_t *pixels) {
    int cx = WINDOW_WIDTH / 2;
    int cy = WINDOW_HEIGHT / 2;
    
    /* Draw axes (dim) */
    for (int x = 0; x < WINDOW_WIDTH; x++) {
        int idx = (cy * WINDOW_WIDTH + x) * 3;
        pixels[idx] = 40; pixels[idx+1] = 40; pixels[idx+2] = 40;
    }
    for (int y = 0; y < WINDOW_HEIGHT; y++) {
        int idx = (y * WINDOW_WIDTH + cx) * 3;
        pixels[idx] = 40; pixels[idx+1] = 40; pixels[idx+2] = 40;
    }
    
    /* Draw unit circle (where ideal PSK points live) */
    float radius = IQ_SCALE * g_zoom * 0.15f;  /* Normalized signal level */
    for (int angle = 0; angle < 360; angle++) {
        float rad = angle * 3.14159265f / 180.0f;
        int x = cx + (int)(radius * cosf(rad));
        int y = cy - (int)(radius * sinf(rad));
        if (x >= 0 && x < WINDOW_WIDTH && y >= 0 && y < WINDOW_HEIGHT) {
            int idx = (y * WINDOW_WIDTH + x) * 3;
            pixels[idx] = 60; pixels[idx+1] = 60; pixels[idx+2] = 60;
        }
    }
    
    /* Draw 8-PSK constellation points */
    for (int n = 0; n < 8; n++) {
        float angle = n * 3.14159265f / 4.0f;
        int x = cx + (int)(radius * cosf(angle));
        int y = cy - (int)(radius * sinf(angle));
        /* Draw small cross at each point */
        for (int d = -3; d <= 3; d++) {
            if (x+d >= 0 && x+d < WINDOW_WIDTH) {
                int idx = (y * WINDOW_WIDTH + x + d) * 3;
                pixels[idx] = 80; pixels[idx+1] = 80; pixels[idx+2] = 80;
            }
            if (y+d >= 0 && y+d < WINDOW_HEIGHT) {
                int idx = ((y + d) * WINDOW_WIDTH + x) * 3;
                pixels[idx] = 80; pixels[idx+1] = 80; pixels[idx+2] = 80;
            }
        }
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    print_version("Phoenix SDR - Wormhole (M110A Constellation)");

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    /* Initialize DSP */
    hilbert_init();
    lpf_init(1500.0f);  /* LPF cutoff - captures 110A bandwidth */

    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Wormhole - MIL-STD-188-110A Constellation",
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

    /* Allocate pixel buffer */
    uint8_t *pixels = (uint8_t *)malloc(WINDOW_WIDTH * WINDOW_HEIGHT * 3);
    if (!pixels) {
        fprintf(stderr, "Memory allocation failed\n");
        return 1;
    }

    printf("Wormhole display ready. Reading PCM from stdin...\n");
    printf("Expected: 16-bit signed mono @ %d Hz\n", SAMPLE_RATE);
    printf("Center frequency: %d Hz (MIL-STD-188-110A)\n", M110A_CENTER_HZ);
    printf("\nControls:\n");
    printf("  Arrow keys: Rotate view\n");
    printf("  A/Z: Slow down / speed up time\n");
    printf("  S/X: Adjust tilt angle\n");
    printf("  +/-: Zoom in/out\n");
    printf("  Space: Toggle auto-rotate\n");
    printf("  R: Reset view\n");
    printf("  Q/Esc: Quit\n\n");

    bool running = true;
    bool auto_rotate = true;
    int samples_processed = 0;
    int decimation_counter = 0;
    const int DECIMATION = 4;  /* Don't need every sample for display */

    while (running) {
        /* Handle SDL events */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                    case SDLK_ESCAPE:
                    case SDLK_q:
                        running = false;
                        break;
                    case SDLK_LEFT:
                        g_rotation_y -= 0.1f;
                        auto_rotate = false;
                        break;
                    case SDLK_RIGHT:
                        g_rotation_y += 0.1f;
                        auto_rotate = false;
                        break;
                    case SDLK_UP:
                        g_rotation_x -= 0.1f;
                        auto_rotate = false;
                        break;
                    case SDLK_DOWN:
                        g_rotation_x += 0.1f;
                        auto_rotate = false;
                        break;
                    case SDLK_PLUS:
                    case SDLK_EQUALS:
                    case SDLK_KP_PLUS:
                        g_zoom *= 1.2f;
                        break;
                    case SDLK_MINUS:
                    case SDLK_KP_MINUS:
                        g_zoom /= 1.2f;
                        break;
                    case SDLK_r:
                        g_rotation_y = 0.0f;
                        g_rotation_x = 0.3f;
                        g_zoom = 1.0f;
                        auto_rotate = true;
                        break;
                    case SDLK_SPACE:
                        auto_rotate = !auto_rotate;
                        break;
                    case SDLK_a:
                        g_time_scale *= 0.5f;
                        if (g_time_scale < 0.01f) g_time_scale = 0.01f;
                        printf("Time scale: %.2fx\n", g_time_scale);
                        break;
                    case SDLK_z:
                        g_time_scale *= 2.0f;
                        if (g_time_scale > 10.0f) g_time_scale = 10.0f;
                        printf("Time scale: %.2fx\n", g_time_scale);
                        break;
                    case SDLK_s:
                        g_tilt_angle += 0.1f;
                        g_rotation_x = g_tilt_angle;
                        printf("Tilt angle: %.1f deg\n", g_tilt_angle * 180.0f / 3.14159265f);
                        break;
                    case SDLK_x:
                        g_tilt_angle -= 0.1f;
                        g_rotation_x = g_tilt_angle;
                        printf("Tilt angle: %.1f deg\n", g_tilt_angle * 180.0f / 3.14159265f);
                        break;
                }
            }
        }

        /* Read a block of samples */
        int16_t pcm_block[256];
        size_t samples_read = fread(pcm_block, sizeof(int16_t), 256, stdin);
        
        if (samples_read == 0) {
            /* No more data - keep displaying but slower update */
            SDL_Delay(50);
        } else {
            /* Apply time scale - delay to slow things down */
            if (g_time_scale < 1.0f) {
                SDL_Delay((Uint32)((1.0f / g_time_scale - 1.0f) * 5.0f));
            }
            /* Process samples */
            for (size_t s = 0; s < samples_read; s++) {
                float sample = pcm_block[s] / 32768.0f;
                
                /* Convert to analytic signal (I/Q) via Hilbert */
                float i_raw, q_raw;
                hilbert_process(sample, &i_raw, &q_raw);
                
                /* Mix down to baseband */
                float i_mixed, q_mixed;
                mix_to_baseband(i_raw, q_raw, &i_mixed, &q_mixed);
                
                /* Low-pass filter */
                float i_filt, q_filt;
                lpf_process(i_mixed, q_mixed, &i_filt, &q_filt);
                
                /* Decimate for display */
                decimation_counter++;
                if (decimation_counter >= DECIMATION) {
                    decimation_counter = 0;
                    trail_add(i_filt, q_filt);
                }
                
                samples_processed++;
            }
        }

        /* Auto-rotate slowly */
        if (auto_rotate) {
            g_rotation_y += 0.002f;
        }

        /* Clear screen (dark background) */
        memset(pixels, 10, WINDOW_WIDTH * WINDOW_HEIGHT * 3);
        
        /* Draw constellation reference grid */
        draw_constellation_grid(pixels);
        
        /* Draw the wormhole trail */
        for (int t = 0; t < g_trail_count; t++) {
            /* Calculate age (0 = newest, 1 = oldest) */
            int idx = (g_trail_head - 1 - t + TRAIL_LENGTH) % TRAIL_LENGTH;
            float age = (float)t / (float)TRAIL_LENGTH;
            
            trail_point_t *pt = &g_trail[idx];
            
            /* Project to screen */
            int sx, sy;
            project_3d(pt->i, pt->q, age, &sx, &sy);
            
            /* Get color based on phase */
            uint8_t r, g, b;
            phase_to_color(pt->i, pt->q, age, &r, &g, &b);
            
            /* Draw point (2x2 for visibility) */
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int px = sx + dx;
                    int py = sy + dy;
                    if (px >= 0 && px < WINDOW_WIDTH && py >= 0 && py < WINDOW_HEIGHT) {
                        int pidx = (py * WINDOW_WIDTH + px) * 3;
                        /* Blend with existing (additive for glow effect) */
                        int nr = pixels[pidx] + r;
                        int ng = pixels[pidx+1] + g;
                        int nb = pixels[pidx+2] + b;
                        pixels[pidx] = (nr > 255) ? 255 : nr;
                        pixels[pidx+1] = (ng > 255) ? 255 : ng;
                        pixels[pidx+2] = (nb > 255) ? 255 : nb;
                    }
                }
            }
        }

        /* Update display */
        SDL_UpdateTexture(texture, NULL, pixels, WINDOW_WIDTH * 3);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        /* Status every 48000 samples (~1 second) */
        if (samples_processed % 48000 < 256) {
            printf("\rSamples: %d  Trail: %d  Rotation: %.1f°  ", 
                   samples_processed, g_trail_count, g_rotation_y * 180.0f / 3.14159265f);
            fflush(stdout);
        }
    }

    printf("\n\nShutting down...\n");

    free(pixels);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
