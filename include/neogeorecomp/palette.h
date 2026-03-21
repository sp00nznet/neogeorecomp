/*
 * palette.h — Neo Geo dual-bank palette system.
 *
 * The Neo Geo has 8 KB of Palette RAM at $400000-$401FFF, organized
 * as 2 banks of 256 palettes, each with 16 colors (index 0 = transparent).
 *
 * Color format (16 bits per entry):
 *   Bit 15: Dark bit (shared LSB for all channels)
 *   Bits 14-12: R0, G0, B0 (LSBs of each channel before dark bit)
 *   Bits 11-8: R4-R1
 *   Bits 7-4: G4-G1
 *   Bits 3-0: B4-B1
 *
 * Effective color depth: 6 bits per channel (R={R4,R3,R2,R1,R0,D}),
 * yielding 65,536 possible colors.
 *
 * Special entries:
 *   $400000 (palette 0, color 0): Reference color, must be $8000 (black)
 *   $401FFE (last color): Backdrop / background color
 *
 * Bank switching is instantaneous via REG_PALBANK0/1 ($3A000F/$3A001F),
 * enabling double-buffered palette updates without tearing.
 */

#ifndef NEOGEORECOMP_PALETTE_H
#define NEOGEORECOMP_PALETTE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Constants ----- */

#define NEOGEO_NUM_PALETTES   256
#define NEOGEO_COLORS_PER_PAL 16
#define NEOGEO_PALETTE_BANKS  2
#define NEOGEO_PALRAM_SIZE    0x2000  /* 8 KB per bank */

/* ----- Initialization ----- */

int palette_init(void);
void palette_shutdown(void);

/* ----- Palette RAM Access (called by bus on $400000-$401FFF access) ----- */

uint16_t palette_read(uint32_t offset);
void palette_write(uint32_t offset, uint16_t val);

/* ----- Bank Switching ----- */

/* Select active palette bank (0 or 1). */
void palette_set_bank(uint8_t bank);
uint8_t palette_get_bank(void);

/* ----- Color Conversion ----- */

/*
 * Convert a Neo Geo 16-bit color value to 32-bit ARGB8888.
 * Handles the dark bit and channel reconstruction.
 */
uint32_t palette_neo_to_argb(uint16_t neo_color);

/*
 * Get the pre-converted ARGB lookup table for the active bank.
 * Returns a pointer to 256*16 = 4096 ARGB values.
 * Updated automatically on palette writes for fast rendering.
 */
const uint32_t *palette_get_argb_table(void);

/* Get the current backdrop color (last entry in active bank). */
uint32_t palette_get_backdrop(void);

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_PALETTE_H */
