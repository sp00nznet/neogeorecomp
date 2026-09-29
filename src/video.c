/*
 * video.c — palette RAM and the per-scanline renderer.
 *
 * The frame loop calls video_render_line() as each visible line (vpos
 * 16..239) is reached, so mid-frame VRAM or palette changes made from the
 * timer IRQ land on the right lines.
 *
 * Sprites follow the LSPC: 381 sprites per frame and at most 96 per line,
 * with later sprites drawn over earlier ones. A sprite with the sticky bit
 * set in SCB3 continues the previous one (same Y, height and vertical
 * shrink; X advanced by the previous width). Vertical shrink goes through
 * the L0 ROM (000-lo.lo): indexed by shrink value and line, it gives the
 * tile and row to fetch. Horizontal shrink drops pixels in a fixed order.
 *
 * C ROM tiles are 16x16, 128 bytes once the odd/even ROMs are interleaved,
 * right half first; they are decoded to one byte per pixel at load time.
 */
#include "ng_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint16_t s_palram[2][0x1000];
static uint32_t s_pens[2][0x1000];
static int      s_pal_bank;

static uint8_t *s_spr;          /* decoded sprites: 256 bytes per tile */
static uint32_t s_spr_tiles_mask;
static uint8_t *s_fix_cart, *s_fix_bios;
static size_t   s_fix_cart_tiles;
static int      s_fix_use_bios = 1;
static uint8_t  s_zoom_rom[0x10000];

/* Which of a tile's 16 pixels survive horizontal shrink value n (n+1 of them). */
static const uint8_t s_zoom_x[16][16] = {
    {0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
    {0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0},
    {0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},
    {0,0,1,0,1,0,0,0,1,0,0,0,1,0,0,0},
    {0,0,1,0,1,0,0,0,1,0,0,0,1,0,1,0},
    {0,0,1,0,1,0,1,0,1,0,0,0,1,0,1,0},
    {0,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
    {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
    {1,0,1,0,1,0,1,0,1,1,1,0,1,0,1,0},
    {1,0,1,1,1,0,1,0,1,1,1,0,1,0,1,0},
    {1,0,1,1,1,0,1,0,1,1,1,0,1,0,1,1},
    {1,0,1,1,1,0,1,1,1,1,1,0,1,0,1,1},
    {1,0,1,1,1,0,1,1,1,1,1,0,1,1,1,1},
    {1,1,1,1,1,0,1,1,1,1,1,0,1,1,1,1},
    {1,1,1,1,1,0,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

/* ---- palette ---- */

static uint32_t color_to_argb(uint16_t w) {
    /* 5 bits per gun (4 high bits + a shared-position low bit) and a
     * "dark" bit that removes the DAC's lowest step from all three. */
    int dark = (w >> 15) & 1;
    int r = ((w >> 7) & 0x1E) | ((w >> 14) & 1);
    int g = ((w >> 3) & 0x1E) | ((w >> 13) & 1);
    int b = ((w << 1) & 0x1E) | ((w >> 12) & 1);
    r = ((r << 1) | !dark) * 255 / 63;
    g = ((g << 1) | !dark) * 255 / 63;
    b = ((b << 1) | !dark) * 255 / 63;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

uint16_t pal_read(uint32_t addr) { return s_palram[s_pal_bank][(addr >> 1) & 0xFFF]; }

void pal_write(uint32_t addr, uint16_t v) {
    uint32_t i = (addr >> 1) & 0xFFF;
    s_palram[s_pal_bank][i] = v;
    s_pens[s_pal_bank][i] = color_to_argb(v);
}

void pal_select_bank(int bank) { s_pal_bank = bank & 1; }

/* ---- loading ---- */

static void decode_sprites(const uint8_t *crom, size_t size) {
    size_t tiles = size / 128;
    s_spr = (uint8_t *)malloc(tiles * 256);
    for (size_t t = 0; t < tiles; t++) {
        const uint8_t *src = crom + t * 128;
        uint8_t *dst = s_spr + t * 256;
        for (int y = 0; y < 16; y++) {
            const uint8_t *l = src + 0x40 + y * 4, *r = src + y * 4;
            for (int x = 0; x < 8; x++) {
                dst[y * 16 + x] = (uint8_t)((((l[3] >> x) & 1) << 3) | (((l[1] >> x) & 1) << 2) |
                                            (((l[2] >> x) & 1) << 1) | ((l[0] >> x) & 1));
                dst[y * 16 + 8 + x] = (uint8_t)((((r[3] >> x) & 1) << 3) | (((r[1] >> x) & 1) << 2) |
                                                (((r[2] >> x) & 1) << 1) | ((r[0] >> x) & 1));
            }
        }
    }
    /* Tile numbers are masked to the next power of two, as the address
     * lines above the fitted ROMs are simply not connected. */
    uint32_t pow2 = 1;
    while (pow2 < tiles) pow2 <<= 1;
    s_spr_tiles_mask = pow2 - 1;
}

static uint8_t *decode_fix(const uint8_t *rom, size_t size) {
    /* 8x8, 32 bytes per tile; bytes $10/$18/$00/$08 hold column pairs
     * 0-1/2-3/4-5/6-7 for each row, left pixel in the low nibble. */
    static const int col_off[4] = {0x10, 0x18, 0x00, 0x08};
    size_t tiles = size / 32;
    uint8_t *out = (uint8_t *)malloc(tiles * 64);
    for (size_t t = 0; t < tiles; t++)
        for (int y = 0; y < 8; y++)
            for (int p = 0; p < 4; p++) {
                uint8_t b = rom[t * 32 + col_off[p] + y];
                out[t * 64 + y * 8 + p * 2] = b & 0x0F;
                out[t * 64 + y * 8 + p * 2 + 1] = b >> 4;
            }
    return out;
}

int video_load(const uint8_t *crom, size_t crom_size, const uint8_t *srom, size_t srom_size,
               const uint8_t *sfix, const uint8_t *zoom_rom) {
    decode_sprites(crom, crom_size);
    s_fix_cart = decode_fix(srom, srom_size);
    s_fix_cart_tiles = srom_size / 32;
    s_fix_bios = decode_fix(sfix, 0x20000);
    memcpy(s_zoom_rom, zoom_rom, sizeof(s_zoom_rom));
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 0x1000; i++) s_pens[b][i] = color_to_argb(0);
    return 0;
}

void video_set_fix_bios(int bios) { s_fix_use_bios = bios; }

void video_shutdown(void) {
    free(s_spr); free(s_fix_cart); free(s_fix_bios);
    s_spr = s_fix_cart = s_fix_bios = NULL;
}

/* ---- rendering ---- */

static int on_line(int scanline, int y, int rows) {
    if (rows > 0x20) rows = 0x20;
    int max_y = (y + rows * 16 - 1) & 0x1FF;
    if (max_y >= y) return scanline >= y && scanline <= max_y;
    return scanline >= y || scanline <= max_y;
}

static void draw_sprites(int scanline, uint32_t *row) {
    const uint32_t *pens = s_pens[s_pal_bank];
    int x = 0, y = 0, rows = 0, zoom_y = 0, zoom_x = 0, drawn = 0;

    for (int n = 0; n < 381; n++) {
        uint16_t ctl = lspc_vram[0x8200 + n];
        uint16_t zoom = lspc_vram[0x8000 + n];
        if (ctl & 0x40) {
            x = (x + zoom_x + 1) & 0x1FF;
            zoom_x = (zoom >> 8) & 0x0F;
        } else {
            y = 0x200 - (ctl >> 7);
            x = lspc_vram[0x8400 + n] >> 7;
            zoom_y = zoom & 0xFF;
            zoom_x = (zoom >> 8) & 0x0F;
            rows = ctl & 0x3F;
        }
        if (rows == 0 || !on_line(scanline, y, rows)) continue;
        if (++drawn > 96) break;
        if (x >= 0x140 && x <= 0x1F0) continue;

        int sprite_line = (scanline - y) & 0x1FF;
        int zoom_line = sprite_line & 0xFF;
        int invert = sprite_line & 0x100;
        if (invert) zoom_line ^= 0xFF;
        if (rows > 0x20) {
            /* Height 33+: the shrunk sprite repeats down the screen. */
            zoom_line %= (zoom_y + 1) << 1;
            if (zoom_line > zoom_y) {
                zoom_line = ((zoom_y + 1) << 1) - 1 - zoom_line;
                invert = !invert;
            }
        }
        int yt = s_zoom_rom[(zoom_y << 8) | zoom_line];
        int tile = yt >> 4, tile_row = yt & 0x0F;
        if (invert) { tile_row ^= 0x0F; tile ^= 0x1F; }

        uint32_t tm = (uint32_t)(n << 6) | (uint32_t)(tile << 1);
        uint16_t attr = lspc_vram[tm + 1];
        uint32_t code = lspc_vram[tm] | (((uint32_t)attr << 12) & 0xF0000u);
        if (attr & 0x0008) code = (code & ~7u) | (lspc_anim_counter & 7);
        else if (attr & 0x0004) code = (code & ~3u) | (lspc_anim_counter & 3);
        if (attr & 0x0002) tile_row ^= 0x0F;

        const uint8_t *gfx = s_spr + ((size_t)(code & s_spr_tiles_mask) << 8) + (tile_row << 4);
        const uint32_t *pal = pens + ((attr >> 8) << 4);
        const uint8_t *zx = s_zoom_x[zoom_x];
        int px = x;
        for (int i = 0; i < 16; i++) {
            if (!zx[i]) continue;
            int c = (attr & 1) ? gfx[15 - i] : gfx[i];
            if (c && px < NG_SCREEN_W) row[px] = pal[c];
            px = (px + 1) & 0x1FF;
        }
    }
}

static void draw_fix(int scanline, uint32_t *row) {
    const uint32_t *pens = s_pens[s_pal_bank];
    const uint8_t *gfx = s_fix_use_bios ? s_fix_bios : s_fix_cart;
    size_t tiles = s_fix_use_bios ? 0x20000 / 32 : s_fix_cart_tiles;
    int map_row = scanline >> 3, y = scanline & 7;
    for (int col = 0; col < 40; col++) {
        uint16_t e = lspc_vram[0x7000 + (col << 5) + map_row];
        uint32_t code = e & 0x0FFF;
        if (code >= tiles) continue;
        const uint8_t *px = gfx + code * 64 + y * 8;
        const uint32_t *pal = pens + ((e >> 12) << 4);
        for (int i = 0; i < 8; i++)
            if (px[i]) row[col * 8 + i] = pal[px[i]];
    }
}

void video_render_line(int line, uint32_t *row) {
    int scanline = line + NG_FIRST_VISIBLE;
    uint32_t backdrop = s_pens[s_pal_bank][0xFFF];
    for (int i = 0; i < NG_SCREEN_W; i++) row[i] = backdrop;
    draw_sprites(scanline, row);
    draw_fix(scanline, row);
}
