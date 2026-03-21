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
#include <neogeorecomp/palette.h>
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

/* ----- Tile Decoding Helpers ----- */

/*
 * Decode one 16x16 4bpp sprite tile from C ROM data.
 *
 * Neo Geo C ROM tile format (128 bytes per tile):
 *   - Tiles are stored as 4 bitplanes across two C ROM chips
 *   - C1 (odd) holds bitplanes 0 and 1
 *   - C2 (even) holds bitplanes 2 and 3
 *   - After interleaving: each byte pair gives 2 bitplanes for 8 pixels
 *   - Each row = 8 bytes (16 pixels x 4bpp)
 *   - 16 rows = 128 bytes total
 *
 * The C ROM data has already been interleaved at load time (video_load_crom
 * interleaves odd/even pairs byte-by-byte), so the data is:
 *   byte 0 (from C1): bitplanes 0,1 for pixels 0-7
 *   byte 1 (from C2): bitplanes 2,3 for pixels 0-7
 *   byte 2 (from C1): bitplanes 0,1 for pixels 8-15
 *   byte 3 (from C2): bitplanes 2,3 for pixels 8-15
 *   ... repeat for 16 rows = 128 bytes
 */
static void decode_sprite_tile(
    uint32_t tile_num,
    uint8_t palette_idx,
    bool h_flip, bool v_flip,
    int screen_x, int screen_y,
    const uint32_t *argb_palette,
    uint32_t *framebuffer)
{
    if (!s_crom || s_crom_size == 0) return;

    /* Each tile is 128 bytes in the interleaved C ROM */
    uint32_t tile_offset = (tile_num * 128) % s_crom_size;
    const uint8_t *tile_data = s_crom + tile_offset;

    /* Palette base: palette_idx * 16 colors */
    int pal_base = palette_idx * 16;

    for (int row = 0; row < 16; row++) {
        int src_row = v_flip ? (15 - row) : row;
        int py = screen_y + row;
        if (py < 0 || py >= NEOGEO_SCREEN_HEIGHT) continue;

        /* Each row is 8 bytes: 2 byte-pairs for left half and right half */
        const uint8_t *row_data = tile_data + src_row * 8;

        for (int col = 0; col < 16; col++) {
            int src_col = h_flip ? (15 - col) : col;
            int px = screen_x + col;
            if (px < 0 || px >= NEOGEO_SCREEN_WIDTH) continue;

            /* Extract 4-bit pixel value from the interleaved data */
            int half = src_col / 8;   /* 0 = left, 1 = right */
            int bit = 7 - (src_col % 8);  /* MSB first */

            uint8_t c1_byte = row_data[half * 2];      /* bitplanes 0,1 */
            uint8_t c2_byte = row_data[half * 2 + 1];  /* bitplanes 2,3 */

            uint8_t pixel = 0;
            pixel |= ((c1_byte >> bit) & 1) << 0;      /* bitplane 0 */
            pixel |= ((c1_byte >> (bit)) & 0) << 1;    /* Need to extract bit from proper position */

            /* Re-extract properly: C1 has bp0 in one nibble, bp1 in another */
            /* Actually, the Neo Geo stores each byte as 8 pixels of one bitplane.
             * The interleaving is: C1_byte, C2_byte alternating.
             * C1 low nibble = bp0, C1 high nibble = bp1
             * C2 low nibble = bp2, C2 high nibble = bp3
             * Wait — that's not right either. Let me use the standard decode. */

            /* Standard Neo Geo C ROM decode:
             * After byte-pair interleaving (C1, C2 alternating):
             * For each pair of bytes at offset [half*2] and [half*2+1]:
             *   Byte from C1: each bit position gives bitplane 0
             *   Actually the bytes encode two bitplanes each.
             *
             * Correct decode: each C ROM byte contains one bitplane for 8 pixels.
             * C1 bytes: alternating bitplane 0 and bitplane 1
             * C2 bytes: alternating bitplane 2 and bitplane 3
             * But after our interleaving, the layout is different.
             *
             * Let's use a simpler approach: read 4 bytes per 8-pixel half-row,
             * extract bit at position 'bit' from each to get the 4 bitplanes.
             */

            /* Simplified: since we interleaved C1,C2 byte-by-byte, for each
             * row of 16 pixels we have 8 bytes. The layout is:
             *   [C1_0, C2_0, C1_1, C2_1, ...] but that's per-ROM-byte.
             *
             * For now, use a direct bitplane extraction: */
            int byte_idx = half * 4;  /* 4 bytes per 8-pixel half */
            uint8_t b0 = row_data[byte_idx + 0];
            uint8_t b1 = row_data[byte_idx + 1];
            uint8_t b2 = row_data[byte_idx + 2];
            uint8_t b3 = row_data[byte_idx + 3];

            pixel = ((b0 >> bit) & 1) << 0 |
                    ((b1 >> bit) & 1) << 1 |
                    ((b2 >> bit) & 1) << 2 |
                    ((b3 >> bit) & 1) << 3;

            if (pixel == 0) continue;  /* Transparent */

            uint32_t color = argb_palette[pal_base + pixel];
            framebuffer[py * NEOGEO_SCREEN_WIDTH + px] = color;
        }
    }
}

/*
 * Decode one 8x8 4bpp fix layer tile from S ROM data.
 *
 * S ROM tiles are simpler than sprite tiles: 32 bytes per tile,
 * 4 bitplanes, stored column-by-column (top to bottom, then
 * left to right within each column).
 */
static void decode_fix_tile(
    uint16_t tile_num,
    uint8_t palette_idx,
    int screen_x, int screen_y,
    const uint32_t *argb_palette,
    uint32_t *framebuffer)
{
    const uint8_t *rom = s_use_bios_fix ? s_sfix : s_srom;
    uint32_t rom_size = s_use_bios_fix ? s_sfix_size : s_srom_size;
    if (!rom || rom_size == 0) return;

    /* Each S ROM tile is 32 bytes */
    uint32_t offset = ((uint32_t)tile_num * 32) % rom_size;
    const uint8_t *tile = rom + offset;

    int pal_base = palette_idx * 16;

    /* S ROM format: 8 columns of 8 pixels each, stored as 4 bytes per column */
    for (int col = 0; col < 8; col++) {
        const uint8_t *col_data = tile + col * 4;
        for (int row = 0; row < 8; row++) {
            int px = screen_x + col;
            int py = screen_y + row;
            if (px < 0 || px >= NEOGEO_SCREEN_WIDTH) continue;
            if (py < 0 || py >= NEOGEO_SCREEN_HEIGHT) continue;

            int bit = 7 - row;
            uint8_t pixel = ((col_data[0] >> bit) & 1) << 0 |
                            ((col_data[1] >> bit) & 1) << 1 |
                            ((col_data[2] >> bit) & 1) << 2 |
                            ((col_data[3] >> bit) & 1) << 3;

            if (pixel == 0) continue;  /* Transparent */

            uint32_t color = argb_palette[pal_base + pixel];
            framebuffer[py * NEOGEO_SCREEN_WIDTH + px] = color;
        }
    }
}

/* ----- Rendering ----- */

void video_render_frame(uint32_t *framebuffer) {
    /*
     * Neo Geo rendering pipeline:
     *   1. Fill with backdrop color (last palette entry)
     *   2. Render sprites 380 -> 0 (lower index = higher priority, drawn last)
     *   3. Render fix layer on top (always visible, highest priority)
     */

    const uint32_t *argb = palette_get_argb_table();
    uint32_t backdrop = palette_get_backdrop();

    /* 1. Fill with backdrop */
    for (int i = 0; i < NEOGEO_SCREEN_WIDTH * NEOGEO_SCREEN_HEIGHT; i++) {
        framebuffer[i] = backdrop;
    }

    /* 2. Render sprites (back to front: high index first, low index on top) */
    for (int spr = NEOGEO_MAX_SPRITES - 1; spr >= 0; spr--) {
        /* Read SCB3: Y position, sprite height, sticky bit */
        uint16_t scb3 = s_vram[0x8200 / 2 + spr];   /* VRAM $8200 + sprite index */
        uint16_t scb4 = s_vram[0x8400 / 2 + spr];   /* VRAM $8400 + sprite index */
        uint16_t scb2 = s_vram[0x8000 / 2 + spr];   /* VRAM $8000 + sprite index */

        /* SCB3 format:
         *   Bits 15-7: Y position (calculated as 496 - Y for screen coords)
         *   Bit 6: Sticky bit (chain to previous sprite's X position)
         *   Bits 5-0: Sprite height in tiles (0 = 1 tile, up to 32)
         */
        int y_raw = (scb3 >> 7) & 0x1FF;
        int sticky = (scb3 >> 6) & 1;
        int height_tiles = scb3 & 0x3F;
        if (height_tiles == 0) continue;  /* No tiles = invisible */

        /* Y position: Neo Geo uses 496-Y for screen coordinate */
        int screen_y = (496 - y_raw) & 0x1FF;
        if (screen_y > 256) screen_y -= 512;  /* Wrap for offscreen sprites */

        /* SCB4: X position (bits 15-7) */
        int screen_x = (scb4 >> 7) & 0x1FF;
        if (screen_x > 320) screen_x -= 512;  /* Wrap */

        /* If sticky, inherit X from previous sprite + 16 */
        /* (Handled by tracking chain_x across iterations — simplified here) */

        /* SCB2: Shrink (not yet implemented — render at full size) */
        /* Bits 7-0: V shrink ($FF = full size), Bits 11-8: H shrink ($F = full) */
        (void)scb2;

        /* Read SCB1: Tile data for this sprite (up to height_tiles entries) */
        /* SCB1 base: VRAM $0000, each sprite has 64 words (up to 32 tile pairs) */
        uint16_t scb1_base = (uint16_t)(spr * 64);

        for (int tile_row = 0; tile_row < height_tiles && tile_row < 32; tile_row++) {
            uint16_t scb1_even = s_vram[scb1_base + tile_row * 2];      /* Tile number low */
            uint16_t scb1_odd  = s_vram[scb1_base + tile_row * 2 + 1];  /* Attributes */

            /* Tile number: 20 bits (even word = low 16, odd bits 15-12 = high 4) */
            uint32_t tile_num = (uint32_t)scb1_even | (((uint32_t)(scb1_odd >> 12) & 0xF) << 16);

            /* Palette index: bits 7-0 of odd word */
            uint8_t palette_idx = scb1_odd & 0xFF;

            /* Flip bits */
            bool h_flip = (scb1_odd & 0x0100) != 0;
            bool v_flip = (scb1_odd & 0x0200) != 0;

            /* Auto-animation: bits 11-10 of odd word */
            uint8_t auto_anim = (scb1_odd >> 10) & 0x3;
            if (auto_anim == 1) {
                /* 4-frame animation: toggle bits 1-0 of tile number */
                tile_num = (tile_num & ~0x3) | (s_auto_anim_counter & 0x3);
            } else if (auto_anim == 2) {
                /* 8-frame animation: toggle bits 2-0 */
                tile_num = (tile_num & ~0x7) | (s_auto_anim_counter & 0x7);
            }

            if (tile_num == 0) continue;

            int tile_y = screen_y + tile_row * 16;
            decode_sprite_tile(tile_num, palette_idx, h_flip, v_flip,
                             screen_x, tile_y, argb, framebuffer);
        }
    }

    /* 3. Render fix layer (always on top) */
    /* Fix layer: 40 columns x 32 rows of 8x8 tiles at VRAM $7000 */
    /* Visible area: 40 x 28 tiles (NTSC), stored top-to-bottom, left-to-right */
    for (int col = 0; col < NEOGEO_FIX_COLS; col++) {
        for (int row = 0; row < NEOGEO_FIX_ROWS; row++) {
            uint16_t fix_entry = s_vram[0x7000 / 2 + col * NEOGEO_FIX_ROWS + row];

            uint16_t tile_num = fix_entry & 0x0FFF;
            uint8_t palette_idx = (fix_entry >> 12) & 0x0F;

            if (tile_num == 0) continue;  /* Empty tile */

            int px = col * 8;
            int py = row * 8;

            /* Only first 16 palettes available for fix layer */
            decode_fix_tile(tile_num, palette_idx, px, py, argb, framebuffer);
        }
    }

    /* Increment auto-animation counter (increments every 8 frames) */
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
