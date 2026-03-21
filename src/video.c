/*
 * video.c — Neo Geo LSPC video system implementation.
 *
 * Handles VRAM management, sprite rendering, and fix layer compositing.
 *
 * The Neo Geo renders 381 sprites per frame. Each sprite is a vertical
 * strip of 16x16 tiles. Wide objects are built by chaining sprites
 * horizontally via the "sticky bit" in SCB3.
 *
 * Rendering priority: Lower sprite numbers have higher priority.
 * The fix layer is always on top of all sprites.
 *
 * TODO:
 *   - Implement per-sprite shrinking (SCB2)
 *   - Implement auto-animation (4-frame and 8-frame)
 *   - Implement scanline-accurate rendering for raster effects
 *   - GPU-accelerated rendering path
 */

#include <neogeorecomp/video.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ----- Internal State ----- */

static uint16_t s_vram[NEOGEO_VRAM_SIZE / 2];  /* VRAM (word-addressed) */
static uint16_t s_vram_addr = 0;               /* Current VRAM address */
static int16_t  s_vram_mod = 0;                /* Auto-increment value */
static uint16_t s_lspc_mode = 0;               /* LSPC mode register */

static uint8_t *s_crom = NULL;       /* Sprite tile data (C ROMs) */
static uint32_t s_crom_size = 0;
static uint8_t *s_srom = NULL;       /* Fix layer tile data (S ROM) */
static uint32_t s_srom_size = 0;
static uint8_t *s_sfix = NULL;       /* BIOS fix tiles (SFIX ROM) */
static uint32_t s_sfix_size = 0;

static bool s_use_bios_fix = true;   /* Fix layer source selection */
static bool s_shadow = false;        /* Shadow/darken mode */
static uint8_t s_auto_anim_counter = 0;  /* Auto-animation frame counter */

/* ----- Initialization ----- */

int video_init(void) {
    memset(s_vram, 0, sizeof(s_vram));
    s_vram_addr = 0;
    s_vram_mod = 1;  /* Default auto-increment */
    s_lspc_mode = 0;
    s_auto_anim_counter = 0;
    return 0;
}

void video_shutdown(void) {
    free(s_crom); s_crom = NULL; s_crom_size = 0;
    free(s_srom); s_srom = NULL; s_srom_size = 0;
    free(s_sfix); s_sfix = NULL; s_sfix_size = 0;
}

/* ----- ROM Loading ----- */

int video_load_crom(const char **crom_paths, int num_croms) {
    /*
     * C ROMs always come in pairs (odd = bitplanes 0-1, even = bitplanes 2-3).
     * We interleave them into a single buffer for efficient tile lookup.
     */
    if (num_croms < 2 || num_croms % 2 != 0) {
        fprintf(stderr, "[video] C ROMs must be in pairs (got %d)\n", num_croms);
        return -1;
    }

    /* Determine total size from first pair */
    FILE *f = fopen(crom_paths[0], "rb");
    if (!f) { fprintf(stderr, "[video] Failed to open C ROM: %s\n", crom_paths[0]); return -1; }
    fseek(f, 0, SEEK_END);
    long per_rom_size = ftell(f);
    fclose(f);

    s_crom_size = (uint32_t)(per_rom_size * num_croms);
    s_crom = (uint8_t *)malloc(s_crom_size);
    if (!s_crom) return -1;

    /* Load each C ROM pair interleaved */
    uint32_t offset = 0;
    for (int i = 0; i < num_croms; i += 2) {
        FILE *f_odd  = fopen(crom_paths[i], "rb");
        FILE *f_even = fopen(crom_paths[i + 1], "rb");
        if (!f_odd || !f_even) {
            fprintf(stderr, "[video] Failed to open C ROM pair %d/%d\n", i, i + 1);
            if (f_odd) fclose(f_odd);
            if (f_even) fclose(f_even);
            return -1;
        }

        /* Interleave byte-by-byte: odd, even, odd, even... */
        for (long j = 0; j < per_rom_size; j++) {
            s_crom[offset++] = (uint8_t)fgetc(f_odd);
            s_crom[offset++] = (uint8_t)fgetc(f_even);
        }

        fclose(f_odd);
        fclose(f_even);
    }

    printf("[video] Loaded %d C ROMs: %u bytes total\n", num_croms, s_crom_size);
    return 0;
}

int video_load_srom(const char *srom_path) {
    FILE *f = fopen(srom_path, "rb");
    if (!f) { fprintf(stderr, "[video] Failed to open S ROM: %s\n", srom_path); return -1; }
    fseek(f, 0, SEEK_END);
    s_srom_size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    s_srom = (uint8_t *)malloc(s_srom_size);
    if (!s_srom) { fclose(f); return -1; }
    fread(s_srom, 1, s_srom_size, f);
    fclose(f);
    printf("[video] Loaded S ROM: %u bytes\n", s_srom_size);
    return 0;
}

int video_load_sfix(const char *sfix_path) {
    FILE *f = fopen(sfix_path, "rb");
    if (!f) { fprintf(stderr, "[video] Failed to open SFIX ROM: %s\n", sfix_path); return -1; }
    fseek(f, 0, SEEK_END);
    s_sfix_size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    s_sfix = (uint8_t *)malloc(s_sfix_size);
    if (!s_sfix) { fclose(f); return -1; }
    fread(s_sfix, 1, s_sfix_size, f);
    fclose(f);
    printf("[video] Loaded SFIX ROM: %u bytes\n", s_sfix_size);
    return 0;
}

/* ----- VRAM Access ----- */

void video_set_vram_addr(uint16_t addr) {
    s_vram_addr = addr;
}

uint16_t video_read_vram(void) {
    uint16_t val = 0;
    if (s_vram_addr < NEOGEO_VRAM_SIZE / 2) {
        val = s_vram[s_vram_addr];
    }
    s_vram_addr = (uint16_t)(s_vram_addr + s_vram_mod);
    return val;
}

void video_write_vram(uint16_t val) {
    if (s_vram_addr < NEOGEO_VRAM_SIZE / 2) {
        s_vram[s_vram_addr] = val;
    }
    s_vram_addr = (uint16_t)(s_vram_addr + s_vram_mod);
}

void video_set_vram_mod(uint16_t mod) {
    s_vram_mod = (int16_t)mod;
}

void video_set_lspc_mode(uint16_t mode) {
    s_lspc_mode = mode;
}

uint16_t video_get_lspc_mode(void) {
    /* Upper byte = current raster line (set by timer system) */
    return s_lspc_mode;
}

/* ----- Rendering ----- */

void video_render_frame(uint32_t *framebuffer) {
    /*
     * TODO: Full sprite and fix layer rendering.
     *
     * For now, clear to backdrop color. The rendering pipeline will be:
     *   1. Fill framebuffer with backdrop color
     *   2. Render sprites 380 down to 0 (lower index = higher priority)
     *      a. Read SCB3 for Y pos, height, sticky bit
     *      b. Read SCB4 for X pos
     *      c. Read SCB2 for shrink coefficients
     *      d. Read SCB1 for tile numbers, palettes, flip
     *      e. Decode tiles from C ROM data and draw
     *   3. Render fix layer on top (always visible)
     */
    memset(framebuffer, 0, NEOGEO_SCREEN_WIDTH * NEOGEO_SCREEN_HEIGHT * sizeof(uint32_t));

    /* Increment auto-animation counter */
    s_auto_anim_counter++;
}

/* ----- Fix Layer Control ----- */

void video_set_fix_source(bool use_bios) {
    s_use_bios_fix = use_bios;
}

/* ----- Shadow ----- */

void video_set_shadow(bool enabled) {
    s_shadow = enabled;
}

/* ----- Auto-Animation ----- */

uint8_t video_get_auto_anim_counter(void) {
    return s_auto_anim_counter;
}

/* ----- Debug ----- */

const uint16_t *video_get_vram_ptr(void) {
    return s_vram;
}
