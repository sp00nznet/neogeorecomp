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
static bool s_frame_active = false;
static neogeo_func_t s_vblank_func = NULL;

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

    /*
     * Neo Geo boot sequence:
     *   1. BIOS at $C00402 runs first (handles hardware init, eyecatcher)
     *   2. BIOS calls game's USER routine ($00068C) once per frame
     *   3. Game's VBlank handler ($00022C) fires on IRQ1
     *
     * Since we don't have the BIOS ROM, we set up the initial state
     * that the BIOS would establish, then drive the game directly.
     */

    /* Set up initial CPU state (as BIOS would leave it) */
    const uint8_t *prom = bus_get_prom_ptr();
    if (prom) {
        m68k_load_vectors(prom);
        printf("[neogeorecomp] Vectors: SSP=$%08X PC=$%06X\n", g_m68k.ssp, g_m68k.pc);
    }

    /* Set supervisor mode with interrupts enabled (as BIOS leaves it) */
    m68k_set_sr(0x2000);

    /* Initialize BIOS RAM locations that games expect */
    bus_write8(0x10FD80, 0x00);  /* Game VBlank not active yet */
    bus_write8(0x10FD82, 0x00);  /* System type: standard */
    bus_write8(0x10FD83, 0x00);  /* Region: Japan */
    bus_write8(0x10FDAE, 0x00);  /* Game state: 0 (init) */

    /* Find the game's key entry points */
    neogeo_func_t user_func = func_table_lookup(0x00068C);    /* USER routine */
    neogeo_func_t vblank_func = func_table_lookup(0x00022C);  /* VBlank handler */

    if (user_func) {
        printf("[neogeorecomp] USER routine at $00068C: found\n");
    } else {
        fprintf(stderr, "[neogeorecomp] WARNING: No USER routine at $00068C\n");
    }
    if (vblank_func) {
        printf("[neogeorecomp] VBlank handler at $00022C: found\n");
    } else {
        fprintf(stderr, "[neogeorecomp] WARNING: No VBlank handler at $00022C\n");
    }

    printf("[neogeorecomp] Entering main loop — %u functions available\n",
           func_table_count());

    /*
     * Execution model:
     *
     * On real Neo Geo hardware, the BIOS calls the game's USER routine
     * once per frame. Some games (like Neo Drift Out) have their own
     * internal frame loop inside the gameplay dispatcher that spins
     * waiting for VBlank to set the "frame ready" flag at $100424.
     *
     * The game's VBlank handler ($00022C) fires as an interrupt — it
     * runs asynchronously between frames, uploading VRAM/palette/sprite
     * data and setting $100424 = 1 to signal the gameplay dispatcher.
     *
     * To simulate this without real interrupts, we hook into the
     * platform_poll_input() call that the gameplay dispatcher makes
     * on each iteration. Before returning, we fire the VBlank handler,
     * render, and present. This way VBlank "fires" naturally during
     * the game's own spin-wait loop.
     *
     * For the initial call (State 0 init), the game sets up and returns
     * quickly (no spin loop), so we handle that as a simple call.
     */

    /* Store VBlank handler globally so the frame hook can call it */
    s_vblank_func = vblank_func;
    s_frame_active = true;

    /* Call USER — for State 0, this sets up init and returns to BIOS.
     * For States 2/3, the gameplay dispatcher takes over with its own loop. */
    printf("[neogeorecomp] Calling USER routine (state %d)...\n", bus_read8(0x10FDAE));
    fflush(stdout);
    if (user_func) {
        user_func();
    }
    printf("[neogeorecomp] USER returned. State now: %d\n", bus_read8(0x10FDAE));
    fflush(stdout);

    /* If USER returns (e.g., after BIOS return stub advances state),
     * loop: fire VBlank, render, then call USER again with the new state.
     * This handles the State 0 -> State 2 transition where State 0
     * returns to BIOS (our stub) which sets state=2, then we need to
     * call USER again to run State 2's handler. */
    while (s_frame_active) {
        printf("[neogeorecomp] Outer loop: state=%d, firing VBlank + USER\n", bus_read8(0x10FDAE));
        fflush(stdout);

        /* Fire VBlank + render */
        neogeo_begin_frame();
        if (vblank_func) vblank_func();
        neogeo_trigger_vblank();
        neogeo_end_frame();

        if (!platform_poll_input()) break;

        /* Call USER for the current state */
        if (user_func) user_func();
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

/* ----- Frame Yield (called by game's spin-wait loops) ----- */

static int s_frame_count = 0;

bool neogeo_frame_yield(void) {
    neogeo_begin_frame();

    /* Fire the VBlank handler — this uploads VRAM/palette/sprites
     * and sets the frame-ready flag ($100424 = 1) */
    if (s_vblank_func) {
        s_vblank_func();
    }

    /* Render and present */
    neogeo_trigger_vblank();
    neogeo_end_frame();

    /* Diagnostic: log state every 60 frames (~1 second) */
    s_frame_count++;
    if (s_frame_count % 60 == 1) {
        uint8_t game_state = bus_read8(0x10FDAE);
        uint16_t sub_state = bus_read16(0x100426);
        uint8_t vbl_flag = bus_read8(0x10FD80);
        uint16_t frame_ready = bus_read16(0x100424);

        /* Check if any sprites have data */
        const uint16_t *vram = video_get_vram_ptr();
        int active_sprites = 0;
        for (int i = 0; i < 381; i++) {
            uint16_t scb3 = vram[0x8200 / 2 + i];
            if ((scb3 & 0x3F) != 0) active_sprites++;
        }

        /* Check palette - sample a few entries */
        uint16_t pal0_c1 = palette_read(1);
        uint16_t backdrop = palette_read(255 * 16 + 15);

        printf("[frame %d] state=%d sub=%d vbl=$%02X ready=%d sprites=%d pal0c1=$%04X backdrop=$%04X\n",
               s_frame_count, game_state, sub_state, vbl_flag, frame_ready,
               active_sprites, pal0_c1, backdrop);
    }

    /* Poll input */
    if (!platform_poll_input()) {
        s_frame_active = false;
        return false;
    }
    return true;
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
