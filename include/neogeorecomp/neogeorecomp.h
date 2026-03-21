/*
 * neogeorecomp.h — Main header for the Neo Geo static recompilation runtime.
 *
 * This is the top-level include for game projects. It pulls in the full
 * runtime API: CPU context, bus, video, audio, input, and platform.
 *
 * Typical usage in a game's main.c:
 *
 *   #include <neogeorecomp/neogeorecomp.h>
 *
 *   int main(int argc, char *argv[]) {
 *       neogeo_init(&(neogeo_config_t){
 *           .rom_path = argv[1],
 *           .window_scale = 3,
 *       });
 *
 *       // Register recompiled functions at their original 68k addresses
 *       func_table_register(0x000200, func_000200);
 *       func_table_register(0x000400, func_000400);
 *       // ...
 *
 *       neogeo_run();  // enters the main loop, never returns
 *       return 0;
 *   }
 *
 * The runtime handles the frame loop internally:
 *   1. Execute recompiled 68k code (starting from reset vector)
 *   2. At VBlank, render the current sprite/fix layer state
 *   3. Mix and output audio
 *   4. Poll input
 *   5. Repeat at ~59.19 Hz
 */

#ifndef NEOGEORECOMP_H
#define NEOGEORECOMP_H

#include "m68k.h"
#include "bus.h"
#include "func_table.h"
#include "video.h"
#include "palette.h"
#include "io.h"
#include "ym2610.h"
#include "z80.h"
#include "timer.h"
#include "platform.h"
#include "debug.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Configuration ----- */

typedef struct {
    const char *rom_path;       /* Path to directory containing ROM files */
    const char *bios_path;      /* Path to Neo Geo BIOS (NULL = same as rom_path) */
    int window_scale;           /* Window size multiplier (1 = 320x224, 2 = 640x448, etc.) */
    bool fullscreen;            /* Start in fullscreen mode */
    bool vsync;                 /* Enable VSync */
    bool mvs_mode;              /* true = MVS (arcade), false = AES (home) */
    int region;                 /* 0 = Japan, 1 = USA, 2 = Europe */
} neogeo_config_t;

/* ----- Lifecycle ----- */

/*
 * Initialize the Neo Geo runtime.
 *
 * This sets up SDL2, allocates memory for all hardware subsystems,
 * loads the BIOS and game ROM files, and prepares the 68k context.
 * Must be called before any other runtime function.
 *
 * Returns 0 on success, nonzero on failure.
 */
int neogeo_init(const neogeo_config_t *config);

/*
 * Enter the main run loop.
 *
 * This function does not return. It runs the frame loop:
 *   - Execute recompiled code from the reset vector
 *   - Fire VBlank interrupt at the end of each frame
 *   - Render sprites and fix layer
 *   - Output audio
 *   - Poll input
 *
 * The game's recompiled functions must be registered via
 * func_table_register() before calling this.
 */
void neogeo_run(void);

/*
 * Shut down the runtime and free all resources.
 *
 * Typically not called (neogeo_run never returns), but available
 * for tools and tests that need clean teardown.
 */
void neogeo_shutdown(void);

/*
 * Yield to the runtime for one frame.
 *
 * Called by recompiled game code when it's waiting for VBlank
 * (e.g., spinning on the $100424 frame-ready flag). This function:
 *   1. Fires the VBlank handler (uploads VRAM/palette/sprites)
 *   2. Renders the current frame
 *   3. Presents to screen
 *   4. Polls input
 *   5. Syncs to ~59.19 Hz
 *
 * Returns false if the user requested quit (window close / ESC).
 */
bool neogeo_frame_yield(void);

/* ----- Frame Hooks ----- */

/*
 * Called by recompiled code at the start of each frame.
 * Advances internal timing and prepares the next frame.
 */
void neogeo_begin_frame(void);

/*
 * Called by recompiled code when the VBlank interrupt fires.
 * Triggers sprite rendering and palette upload.
 */
void neogeo_trigger_vblank(void);

/*
 * Called by recompiled code at the end of each frame.
 * Presents the rendered frame, outputs audio, polls input.
 */
void neogeo_end_frame(void);

/* ----- Version ----- */

#define NEOGEORECOMP_VERSION_MAJOR 0
#define NEOGEORECOMP_VERSION_MINOR 1
#define NEOGEORECOMP_VERSION_PATCH 0

const char *neogeo_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_H */
