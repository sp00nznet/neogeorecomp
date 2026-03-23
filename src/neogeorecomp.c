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

    s_frame_count++;

    /* Force cartridge fix tiles (the game's S ROM, not BIOS SFIX) */
    if (s_frame_count == 1) {
        video_set_fix_source(false);  /* Use cart S ROM */
    }

    /* After 120 frames (~2 sec), simulate a start button press by
     * setting $10041A = 1. This triggers the title animation to exit
     * and advance to the racing demo. Must be set DURING the frame
     * (after USER clears it) for the gameplay dispatcher to see it. */
    /* Check sprite object count and buffer data */
    if (s_frame_count % 60 == 30) {
        uint16_t spr_obj_count = bus_read16(0x1020A0);
        uint16_t spr_list_count = bus_read16(0x1020A2);
        uint16_t spr_upload = bus_read16(0x102224);
        uint16_t vram_swap = bus_read16(0x102532);
        /* Check if upload buffers have data */
        uint16_t shrink0 = bus_read16(0x102230);
        uint16_t ypos0 = bus_read16(0x102430);
        uint16_t xpos0 = bus_read16(0x102330);
        /* Check sprite attribute table */
        uint16_t attr0_flag = bus_read16(0x101B20);
        uint16_t attr0_ptr = bus_read16(0x101B22);
        if (spr_list_count > 0 || spr_obj_count > 0 || attr0_flag > 0) {
            printf("[spr] obj=%d lst=%d upl=%d swp=%d sh=$%04X y=$%04X x=$%04X attr=(%d,$%04X)\n",
                   spr_obj_count, spr_list_count, spr_upload, vram_swap,
                   shrink0, ypos0, xpos0, attr0_flag, attr0_ptr);
        }
    }

    /* Check SCB3 more carefully */
    if (s_frame_count % 60 == 30) {
        const uint16_t *vr = video_get_vram_ptr();
        int scb3_nonzero = 0;
        for (int i = 0; i < 381; i++) {
            if (vr[0x8200/2 + i] != 0) scb3_nonzero++;
        }
        int scb1_nonzero = 0;
        int first_scb1_addr = -1;
        uint16_t first_scb1_val = 0;
        for (int i = 0; i < 381 * 64 && scb1_nonzero < 5; i++) {
            if (vr[i] != 0) {
                if (first_scb1_addr < 0) { first_scb1_addr = i; first_scb1_val = vr[i]; }
                scb1_nonzero++;
            }
        }
        uint16_t swap_flag = bus_read16(0x102532);
        uint16_t spr_flag = bus_read16(0x102224);
        /* Check the double-buffer content */
        uint32_t rd_buf = bus_read32(0x1025D2);
        int buf_nonzero = 0;
        for (int i = 0; i < 128; i++) {
            if (bus_read16(rd_buf + i * 2) != 0) buf_nonzero++;
        }
        /* Check double-buffer content */
        uint32_t buf_rd = bus_read32(0x1025D2);
        uint16_t buf_scb3_0 = bus_read16(buf_rd + 0x42);  /* SCB3 data in buffer */
        uint16_t buf_scb3_1 = bus_read16(buf_rd + 0x44);

        uint32_t buf_wr = bus_read32(0x1025D6);
        uint16_t wr_scb3_0 = bus_read16(buf_wr + 0x42);
        uint16_t wr_scb3_1 = bus_read16(buf_wr + 0x44);

        /* Dump first few active sprites' positions and tiles */
        if (s_frame_count == 200 || s_frame_count == 400) {
            int dumped = 0;
            for (int spr = 0; spr < 381 && dumped < 5; spr++) {
                uint16_t scb3 = vr[0x8200 + spr];
                if ((scb3 & 0x3F) == 0) continue;
                uint16_t scb4 = vr[0x8400 + spr];
                uint16_t scb2 = vr[0x8000 + spr];
                uint16_t tile_lo = vr[spr * 64];
                uint16_t tile_hi = vr[spr * 64 + 1];
                int y_raw = (scb3 >> 7) & 0x1FF;
                int height = scb3 & 0x3F;
                int x = (scb4 >> 7) & 0x1FF;
                int screen_y = (496 - y_raw) & 0x1FF;
                if (screen_y >= 256) screen_y -= 512;
                uint32_t tile_num = (uint32_t)tile_lo | (((uint32_t)(tile_hi >> 12) & 0xF) << 16);
                uint8_t pal = tile_hi & 0xFF;
                printf("  [spr %d] tile=$%05X pal=%d pos=(%d,%d) h=%d shrink=$%04X\n",
                       spr, tile_num, pal, x > 320 ? x-512 : x, screen_y, height, scb2);
                dumped++;
            }
        }
        if (0) printf("[vram %d] SCB1=%d SCB3=%d rd=$%04X,$%04X wr=$%04X,$%04X\n",
               s_frame_count, scb1_nonzero, scb3_nonzero,
               buf_scb3_0, buf_scb3_1, wr_scb3_0, wr_scb3_1);
    }

    /* Save a screenshot at frame 300 (~5 seconds in) */
    if (s_frame_count == 300) {
        FILE *bmp = fopen("screenshot.bmp", "wb");
        if (bmp) {
            /* BMP header for 320x224 32-bit */
            uint32_t pixel_size = NEOGEO_SCREEN_WIDTH * NEOGEO_SCREEN_HEIGHT * 4;
            uint32_t file_size = 54 + pixel_size;
            uint8_t header[54] = {0};
            header[0] = 'B'; header[1] = 'M';
            *(uint32_t*)(header+2) = file_size;
            *(uint32_t*)(header+10) = 54;
            *(uint32_t*)(header+14) = 40;
            *(int32_t*)(header+18) = NEOGEO_SCREEN_WIDTH;
            *(int32_t*)(header+22) = -NEOGEO_SCREEN_HEIGHT; /* top-down */
            *(uint16_t*)(header+26) = 1;
            *(uint16_t*)(header+28) = 32;
            *(uint32_t*)(header+34) = pixel_size;
            fwrite(header, 1, 54, bmp);
            fwrite(s_framebuffer, 4, NEOGEO_SCREEN_WIDTH * NEOGEO_SCREEN_HEIGHT, bmp);
            fclose(bmp);
            printf("[neogeorecomp] Screenshot saved to screenshot.bmp\n");
        }
    }

    /* Diagnostic: log state every 60 frames (~1 second) */
    if (s_frame_count % 60 == 1) {
        uint8_t game_state = bus_read8(0x10FDAE);
        uint16_t sub_state = bus_read16(0x100426);
        uint8_t vbl_flag = bus_read8(0x10FD80);
        uint16_t frame_ready = bus_read16(0x100424);

        /* Check if any sprites have data */
        const uint16_t *vram = video_get_vram_ptr();
        int active_sprites = 0;
        for (int i = 0; i < 381; i++) {
            uint16_t scb3 = vram[0x8200 + i];
            if ((scb3 & 0x3F) != 0) active_sprites++;
        }

        /* Check palette - sample a few entries */
        uint16_t pal0_c1 = palette_read(1);
        uint16_t backdrop = palette_read(255 * 16 + 15);

        /* Also check VRAM for any non-zero data */
        int vram_nonzero = 0;
        for (int i = 0; i < 0x4400; i++) {
            if (vram[i] != 0) { vram_nonzero++; break; }
        }

        /* Check fix layer (VRAM $7000 to $7500, word-addressed) */
        int fix_nonzero = 0;
        int fix_text = 0;  /* Non-space entries */
        uint16_t fix_sample = 0;
        for (int i = 0x7000; i < 0x7500; i++) {
            if (vram[i] != 0) {
                fix_nonzero++;
                if (vram[i] != 0x0020) {
                    fix_text++;
                    if (fix_sample == 0) fix_sample = vram[i];
                }
            }
        }

        /* Check palette RAM for any non-zero colors */
        int pal_nonzero = 0;
        for (int i = 0; i < 256*16; i++) {
            if (palette_read(i) != 0) { pal_nonzero++; break; }
        }

        /* Animation state */
        uint32_t anim_ptr = bus_read32(0x100472);
        uint16_t anim_sub = bus_read16(0x10048A);
        uint16_t scroll_pos = bus_read16(0x10047C);
        uint16_t flag_41A = bus_read16(0x10041A);

        uint32_t vram_read_ptr = bus_read32(0x1025D2);
        uint32_t vram_write_ptr = bus_read32(0x1025D6);
        uint16_t vram_swap = bus_read16(0x102532);
        uint16_t spr_flag = bus_read16(0x102224);

        /* Count total non-zero palette entries */
        int total_pal = 0;
        for (int i = 0; i < 256*16; i++) {
            if (palette_read(i) != 0) total_pal++;
        }

        /* Sample some actual palette colors */
        uint16_t pal_sample[4] = {
            palette_read(0*16 + 1),   /* Pal 0 color 1 */
            palette_read(1*16 + 1),   /* Pal 1 color 1 */
            palette_read(16*16 + 1),  /* Pal 16 color 1 */
            palette_read(255*16+15),  /* Last color (backdrop) */
        };

        /* Find first palette with non-zero data */
        int first_pal = -1;
        uint16_t first_color = 0;
        for (int p = 0; p < 256 && first_pal < 0; p++) {
            for (int c = 1; c < 16; c++) {
                uint16_t v = palette_read(p * 16 + c);
                if (v != 0) { first_pal = p; first_color = v; break; }
            }
        }

        /* Check palette 9 specifically (used by fix layer text) */
        uint16_t pal9_c1 = palette_read(9 * 16 + 1);
        printf("[frame %d] st=%d sub=%d spr=%d pal=%d fix=%d txt=%d(s=$%04X) p9c1=$%04X\n",
               s_frame_count, game_state, sub_state,
               active_sprites, total_pal, fix_nonzero, fix_text, fix_sample, pal9_c1);
        if (0) /* suppress original */
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
    /* Dump active sprite details once */
    {
        static int s_dump = 0;
        if (!s_dump && s_frame_count > 250) {
            const uint16_t *vr = video_get_vram_ptr();
            int active = 0;
            for (int i = 0; i < 381; i++)
                if ((vr[0x8200 + i] & 0x3F) != 0) active++;
            if (active > 0) {
                s_dump = 1;
                printf("[SPRITES] %d active:\n", active);
                for (int spr = 0; spr < 381; spr++) {
                    uint16_t scb3 = vr[0x8200 + spr];
                    if ((scb3 & 0x3F) == 0) continue;
                    uint16_t scb4 = vr[0x8400 + spr];
                    uint16_t scb2 = vr[0x8000 + spr];
                    uint16_t t0 = vr[spr*64], t1 = vr[spr*64+1];
                    int yr = (scb3>>7)&0x1FF, h = scb3&0x3F;
                    int sy = (496-yr)&0x1FF; if(sy>=256) sy-=512;
                    int x = (scb4>>7)&0x1FF; if(x>320) x-=512;
                    uint32_t tn = (uint32_t)t0 | (((uint32_t)(t1>>12)&0xF)<<16);
                    printf("  #%d: tile=$%05X pal=%d pos=(%d,%d) h=%d sh=$%04X\n",
                           spr, tn, t1&0xFF, x, sy, h, scb2);
                }
                fflush(stdout);
            }
        }
    }

    /* HACK: Force visible palette for sprites with tile data but pal=0 */
    {
        uint16_t *vw = (uint16_t *)video_get_vram_ptr(); /* cast away const for hack */
        for (int spr = 0; spr < 381; spr++) {
            uint16_t scb3 = vw[0x8200 + spr];
            if ((scb3 & 0x3F) == 0) continue;
            uint16_t tile_lo = vw[spr * 64];
            uint16_t tile_hi = vw[spr * 64 + 1];
            if (tile_lo != 0 && (tile_hi & 0xFF) == 0) {
                /* Has tile but no palette — force palette 2 */
                vw[spr * 64 + 1] = (tile_hi & 0xFF00) | 0x02;
            }
        }
    }

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
