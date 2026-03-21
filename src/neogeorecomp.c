/*
 * neogeorecomp.c — Main runtime initialization and frame loop.
 *
 * This is the orchestrator: it initializes all subsystems, loads ROMs,
 * and runs the main frame loop that drives the recompiled game code.
 *
 * Frame loop overview:
 *   1. Execute recompiled 68k code (from reset vector on first frame)
 *   2. At scanline 224, fire VBlank interrupt (IRQ1)
 *   3. Render sprites and fix layer from VRAM state
 *   4. Run Z80 for ~67,614 cycles (one frame's worth at 4 MHz)
 *   5. Generate audio samples from YM2610
 *   6. Present frame, poll input, sync to ~59.19 Hz
 */

#include <neogeorecomp/neogeorecomp.h>
#include <stdio.h>
#include <string.h>

/* ----- Internal State ----- */

static neogeo_config_t s_config;
static bool s_initialized = false;

/* 320x224 ARGB8888 framebuffer */
static uint32_t s_framebuffer[NEOGEO_SCREEN_WIDTH * NEOGEO_SCREEN_HEIGHT];

/* Audio output buffer (stereo, 16-bit, one frame's worth at 48 kHz / 59.19 Hz ~ 811 samples) */
#define AUDIO_SAMPLES_PER_FRAME 811
static int16_t s_audio_buffer[AUDIO_SAMPLES_PER_FRAME * 2];

/* ----- Lifecycle ----- */

int neogeo_init(const neogeo_config_t *config) {
    if (s_initialized) {
        return -1;
    }

    memcpy(&s_config, config, sizeof(neogeo_config_t));

    /* Default values */
    if (s_config.window_scale <= 0) s_config.window_scale = 3;
    if (!s_config.bios_path) s_config.bios_path = s_config.rom_path;

    printf("[neogeorecomp] Initializing v%s\n", neogeo_version_string());
    printf("[neogeorecomp] Mode: %s, Region: %s\n",
           s_config.mvs_mode ? "MVS (arcade)" : "AES (home)",
           s_config.region == 0 ? "Japan" :
           s_config.region == 1 ? "USA" : "Europe");

    /* Initialize subsystems in dependency order */
    int rc;

    rc = bus_init();
    if (rc != 0) { fprintf(stderr, "[neogeorecomp] bus_init failed\n"); return rc; }

    rc = func_table_init();
    if (rc != 0) { fprintf(stderr, "[neogeorecomp] func_table_init failed\n"); return rc; }

    m68k_init();

    rc = video_init();
    if (rc != 0) { fprintf(stderr, "[neogeorecomp] video_init failed\n"); return rc; }

    rc = palette_init();
    if (rc != 0) { fprintf(stderr, "[neogeorecomp] palette_init failed\n"); return rc; }

    rc = io_init(s_config.mvs_mode, s_config.region);
    if (rc != 0) { fprintf(stderr, "[neogeorecomp] io_init failed\n"); return rc; }

    rc = timer_init();
    if (rc != 0) { fprintf(stderr, "[neogeorecomp] timer_init failed\n"); return rc; }

    rc = z80_init();
    if (rc != 0) { fprintf(stderr, "[neogeorecomp] z80_init failed\n"); return rc; }

    rc = ym2610_init(48000);
    if (rc != 0) { fprintf(stderr, "[neogeorecomp] ym2610_init failed\n"); return rc; }

    rc = platform_init(s_config.window_scale, s_config.fullscreen, s_config.vsync);
    if (rc != 0) { fprintf(stderr, "[neogeorecomp] platform_init failed\n"); return rc; }

    rc = platform_audio_init(48000);
    if (rc != 0) { fprintf(stderr, "[neogeorecomp] platform_audio_init failed\n"); return rc; }

    debug_init();

    s_initialized = true;
    printf("[neogeorecomp] Initialization complete. %u functions registered.\n",
           func_table_count());

    return 0;
}

void neogeo_run(void) {
    if (!s_initialized) {
        fprintf(stderr, "[neogeorecomp] Cannot run: not initialized\n");
        return;
    }

    printf("[neogeorecomp] Starting main loop\n");

    /* Load initial SSP and PC from the P ROM vector table */
    m68k_load_vectors(bus_get_prom_ptr());

    /* Call the reset vector entry point */
    neogeo_func_t reset_func = func_table_lookup(g_m68k.pc);
    if (reset_func) {
        printf("[neogeorecomp] Executing reset vector at $%06X\n", g_m68k.pc);
    } else {
        fprintf(stderr, "[neogeorecomp] WARNING: No function registered at reset vector $%06X\n",
                g_m68k.pc);
    }

    /* Main frame loop */
    while (1) {
        neogeo_begin_frame();

        /* Execute one frame of recompiled code.
         * The recompiled code handles its own control flow via
         * func_table_call(). The VBlank interrupt handler is called
         * when timer_vblank_pending() returns true. */
        if (reset_func) {
            reset_func();
        }

        neogeo_trigger_vblank();
        neogeo_end_frame();

        /* Poll input — returns false if the user wants to quit */
        if (!platform_poll_input()) {
            break;
        }
    }

    neogeo_shutdown();
}

void neogeo_shutdown(void) {
    if (!s_initialized) return;

    printf("[neogeorecomp] Shutting down\n");

    debug_shutdown();
    platform_shutdown();
    ym2610_shutdown();
    z80_shutdown();
    timer_shutdown();
    io_shutdown();
    palette_shutdown();
    video_shutdown();
    func_table_shutdown();
    bus_shutdown();

    s_initialized = false;
}

/* ----- Frame Hooks ----- */

void neogeo_begin_frame(void) {
    io_update();
    timer_kick_watchdog();  /* Stub: auto-kick for now during development */
}

void neogeo_trigger_vblank(void) {
    /* Render the current VRAM state to the framebuffer */
    video_render_frame(s_framebuffer);

    /* Run the Z80 for one frame (~67,614 cycles at 4 MHz / 59.19 Hz) */
    z80_execute(67614);

    /* Generate audio */
    ym2610_generate(s_audio_buffer, AUDIO_SAMPLES_PER_FRAME);
    platform_audio_queue(s_audio_buffer, AUDIO_SAMPLES_PER_FRAME);
}

void neogeo_end_frame(void) {
    /* Present the rendered framebuffer */
    platform_present(s_framebuffer);

    /* Advance watchdog */
    timer_watchdog_tick();

    /* Sync to ~59.19 Hz */
    platform_frame_sync();
}

/* ----- Version ----- */

const char *neogeo_version_string(void) {
    return "0.1.0";
}
