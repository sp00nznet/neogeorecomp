/*
 * palette.c — Neo Geo dual-bank palette system implementation.
 *
 * Maintains two banks of palette RAM and a pre-converted ARGB lookup
 * table that's updated on every palette write for fast rendering.
 */

#include <neogeorecomp/palette.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ----- Internal State ----- */

/* Raw Neo Geo palette data (2 banks x 256 palettes x 16 colors) */
static uint16_t s_palram[NEOGEO_PALETTE_BANKS][NEOGEO_NUM_PALETTES * NEOGEO_COLORS_PER_PAL];

/* Pre-converted ARGB8888 lookup table for the active bank */
static uint32_t s_argb_table[NEOGEO_NUM_PALETTES * NEOGEO_COLORS_PER_PAL];

static uint8_t s_active_bank = 0;

/* ----- Color Conversion ----- */

uint32_t palette_neo_to_argb(uint16_t neo_color) {
    /*
     * Neo Geo color format:
     *   Bit 15: D (dark bit, shared LSB for all channels)
     *   Bit 14: R0    Bit 13: G0    Bit 12: B0
     *   Bits 11-8: R4-R1
     *   Bits 7-4:  G4-G1
     *   Bits 3-0:  B4-B1
     *
     * Each channel: {X4, X3, X2, X1, X0, D} = 6 bits
     * Scale to 8 bits: (6-bit value << 2) | (6-bit value >> 4)
     */
    uint8_t dark = (neo_color >> 15) & 1;

    uint8_t r6 = (uint8_t)((((neo_color >> 8) & 0x0F) << 2) | (((neo_color >> 14) & 1) << 1) | dark);
    uint8_t g6 = (uint8_t)((((neo_color >> 4) & 0x0F) << 2) | (((neo_color >> 13) & 1) << 1) | dark);
    uint8_t b6 = (uint8_t)((((neo_color >> 0) & 0x0F) << 2) | (((neo_color >> 12) & 1) << 1) | dark);

    /* Scale 6-bit to 8-bit */
    uint8_t r8 = (r6 << 2) | (r6 >> 4);
    uint8_t g8 = (g6 << 2) | (g6 >> 4);
    uint8_t b8 = (b6 << 2) | (b6 >> 4);

    return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

/* Rebuild the ARGB table for the active bank */
static void rebuild_argb_table(void) {
    int total = NEOGEO_NUM_PALETTES * NEOGEO_COLORS_PER_PAL;
    for (int i = 0; i < total; i++) {
        /* Color index 0 of every palette is always transparent */
        if ((i & 0xF) == 0) {
            s_argb_table[i] = 0x00000000;
        } else {
            s_argb_table[i] = palette_neo_to_argb(s_palram[s_active_bank][i]);
        }
    }
}

/* ----- Initialization ----- */

int palette_init(void) {
    memset(s_palram, 0, sizeof(s_palram));
    memset(s_argb_table, 0, sizeof(s_argb_table));
    s_active_bank = 0;
    return 0;
}

void palette_shutdown(void) {
    /* Nothing to free — all static */
}

/* ----- Palette RAM Access ----- */

uint16_t palette_read(uint32_t offset) {
    if (offset >= NEOGEO_NUM_PALETTES * NEOGEO_COLORS_PER_PAL) return 0;
    return s_palram[s_active_bank][offset];
}

void palette_write(uint32_t offset, uint16_t val) {
    if (offset >= NEOGEO_NUM_PALETTES * NEOGEO_COLORS_PER_PAL) return;
    s_palram[s_active_bank][offset] = val;
    /* Update the ARGB cache for this entry */
    if ((offset & 0xF) == 0) {
        s_argb_table[offset] = 0x00000000;  /* Color 0 of every palette = transparent */
    } else {
        s_argb_table[offset] = palette_neo_to_argb(val);
    }
}

/* ----- Bank Switching ----- */

void palette_set_bank(uint8_t bank) {
    if (bank > 1) return;
    if (bank != s_active_bank) {
        s_active_bank = bank;
        rebuild_argb_table();
    }
}

uint8_t palette_get_bank(void) {
    return s_active_bank;
}

/* ----- Fast Access ----- */

const uint32_t *palette_get_argb_table(void) {
    return s_argb_table;
}

uint32_t palette_get_backdrop(void) {
    /* Last color in the active bank */
    return s_argb_table[NEOGEO_NUM_PALETTES * NEOGEO_COLORS_PER_PAL - 1];
}
