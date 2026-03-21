/*
 * platform.c — SDL2 platform layer implementation.
 *
 * Handles window creation, input polling, audio output, and frame timing.
 * This is the only file in the runtime that depends on SDL2.
 *
 * Default key mapping (configurable in the future):
 *   Player 1:
 *     Arrow keys: Up/Down/Left/Right
 *     Z: A   X: B   C: C   V: D
 *     1: Start   3: Select   5: Coin
 *   Player 2:
 *     TFGH: Up/Down/Left/Right
 *     I: A   O: B   P: C   [: D
 *     2: Start   4: Select   6: Coin
 *   System:
 *     F11: Toggle fullscreen
 *     Escape: Quit
 */

#include <neogeorecomp/platform.h>
#include <neogeorecomp/video.h>
#include <neogeorecomp/io.h>

#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* ----- Internal State ----- */

static SDL_Window *s_window = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture *s_texture = NULL;
static SDL_AudioDeviceID s_audio_dev = 0;

static bool s_fullscreen = false;
static uint64_t s_frame_start = 0;

/* Target frame time in microseconds (1,000,000 / 59.185606) */
#define FRAME_TIME_US 16896

/* ----- Initialization ----- */

int platform_init(int window_scale, bool fullscreen, bool vsync) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[platform] SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    int width = NEOGEO_SCREEN_WIDTH * window_scale;
    int height = NEOGEO_SCREEN_HEIGHT * window_scale;

    uint32_t flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    s_window = SDL_CreateWindow(
        "neogeorecomp",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height, flags
    );
    if (!s_window) {
        fprintf(stderr, "[platform] Window creation failed: %s\n", SDL_GetError());
        return -1;
    }

    uint32_t render_flags = SDL_RENDERER_ACCELERATED;
    if (vsync) render_flags |= SDL_RENDERER_PRESENTVSYNC;

    s_renderer = SDL_CreateRenderer(s_window, -1, render_flags);
    if (!s_renderer) {
        fprintf(stderr, "[platform] Renderer creation failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Set integer scaling for sharp pixels */
    SDL_RenderSetLogicalSize(s_renderer, NEOGEO_SCREEN_WIDTH, NEOGEO_SCREEN_HEIGHT);
    SDL_RenderSetIntegerScale(s_renderer, SDL_TRUE);

    s_texture = SDL_CreateTexture(
        s_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        NEOGEO_SCREEN_WIDTH, NEOGEO_SCREEN_HEIGHT
    );
    if (!s_texture) {
        fprintf(stderr, "[platform] Texture creation failed: %s\n", SDL_GetError());
        return -1;
    }

    s_fullscreen = fullscreen;
    s_frame_start = SDL_GetPerformanceCounter();

    printf("[platform] Window: %dx%d (scale %d), VSync: %s\n",
           width, height, window_scale, vsync ? "on" : "off");
    return 0;
}

void platform_shutdown(void) {
    if (s_audio_dev) SDL_CloseAudioDevice(s_audio_dev);
    if (s_texture)  SDL_DestroyTexture(s_texture);
    if (s_renderer) SDL_DestroyRenderer(s_renderer);
    if (s_window)   SDL_DestroyWindow(s_window);
    SDL_Quit();
}

/* ----- Display ----- */

void platform_present(const uint32_t *framebuffer) {
    SDL_UpdateTexture(s_texture, NULL, framebuffer,
                      NEOGEO_SCREEN_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

void platform_toggle_fullscreen(void) {
    s_fullscreen = !s_fullscreen;
    SDL_SetWindowFullscreen(s_window,
        s_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

/* ----- Input ----- */

bool platform_poll_input(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return false;

            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                bool pressed = (event.type == SDL_KEYDOWN);
                switch (event.key.keysym.scancode) {
                    /* Player 1 */
                    case SDL_SCANCODE_UP:    io_set_button(0, IO_BTN_UP, pressed); break;
                    case SDL_SCANCODE_DOWN:  io_set_button(0, IO_BTN_DOWN, pressed); break;
                    case SDL_SCANCODE_LEFT:  io_set_button(0, IO_BTN_LEFT, pressed); break;
                    case SDL_SCANCODE_RIGHT: io_set_button(0, IO_BTN_RIGHT, pressed); break;
                    case SDL_SCANCODE_Z:     io_set_button(0, IO_BTN_A, pressed); break;
                    case SDL_SCANCODE_X:     io_set_button(0, IO_BTN_B, pressed); break;
                    case SDL_SCANCODE_C:     io_set_button(0, IO_BTN_C, pressed); break;
                    case SDL_SCANCODE_V:     io_set_button(0, IO_BTN_D, pressed); break;

                    /* System */
                    case SDL_SCANCODE_5:
                        if (pressed) io_insert_coin(0);
                        break;
                    case SDL_SCANCODE_1:
                        /* Start button via status_b, handled differently */
                        break;

                    case SDL_SCANCODE_F11:
                        if (pressed) platform_toggle_fullscreen();
                        break;
                    case SDL_SCANCODE_ESCAPE:
                        return false;

                    default:
                        break;
                }
                break;
            }
        }
    }
    return true;
}

/* ----- Audio ----- */

int platform_audio_init(int sample_rate) {
    SDL_AudioSpec desired = {0};
    desired.freq = sample_rate;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 1024;
    desired.callback = NULL;  /* We use SDL_QueueAudio */

    s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    if (s_audio_dev == 0) {
        fprintf(stderr, "[platform] Audio init failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(s_audio_dev, 0);  /* Start playback */
    printf("[platform] Audio: %d Hz, stereo\n", sample_rate);
    return 0;
}

void platform_audio_queue(const int16_t *samples, int num_samples) {
    if (s_audio_dev) {
        SDL_QueueAudio(s_audio_dev, samples, (uint32_t)(num_samples * 2 * sizeof(int16_t)));
    }
}

/* ----- Timing ----- */

void platform_frame_sync(void) {
    /* Simple frame limiter targeting ~59.19 Hz */
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t target = s_frame_start + (freq * FRAME_TIME_US / 1000000);
    uint64_t now;

    do {
        now = SDL_GetPerformanceCounter();
    } while (now < target);

    s_frame_start = now;
}

uint64_t platform_get_ticks(void) {
    return SDL_GetTicks64();
}

/* ----- Window Title ----- */

void platform_set_title(const char *title) {
    if (s_window) SDL_SetWindowTitle(s_window, title);
}
