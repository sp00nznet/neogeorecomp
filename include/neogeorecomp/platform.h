/*
 * platform.h — SDL2 platform abstraction layer.
 *
 * Handles windowing, input mapping, audio output, and frame timing.
 * This is the only file that touches SDL2 directly — everything else
 * in the runtime is platform-agnostic.
 */

#ifndef NEOGEORECOMP_PLATFORM_H
#define NEOGEORECOMP_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Initialization ----- */

/*
 * Initialize the platform layer.
 *
 * Creates the SDL2 window, renderer, and audio device.
 * window_scale: pixel multiplier (3 = 960x672 window for 320x224 output)
 * fullscreen: start in fullscreen mode
 * vsync: enable VSync
 */
int platform_init(int window_scale, bool fullscreen, bool vsync);
void platform_shutdown(void);

/* ----- Display ----- */

/*
 * Present a rendered frame to the screen.
 *
 * framebuffer: 320x224 array of ARGB8888 pixels
 */
void platform_present(const uint32_t *framebuffer);

/* Toggle fullscreen mode. */
void platform_toggle_fullscreen(void);

/* ----- Input ----- */

/*
 * Poll SDL2 input events and update the I/O subsystem.
 * Returns false if the user has requested to quit (window close, etc.)
 */
bool platform_poll_input(void);

/* ----- Audio ----- */

/*
 * Start audio playback.
 * sample_rate: audio sample rate (typically 44100 or 48000)
 */
int platform_audio_init(int sample_rate);

/* Queue audio samples for playback. */
void platform_audio_queue(const int16_t *samples, int num_samples);

/* ----- Timing ----- */

/*
 * Wait until it's time for the next frame.
 * Targets ~59.185606 Hz (Neo Geo NTSC refresh rate).
 */
void platform_frame_sync(void);

/* Get the current time in milliseconds (for profiling). */
uint64_t platform_get_ticks(void);

/* ----- Window Title ----- */

void platform_set_title(const char *title);

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_PLATFORM_H */
