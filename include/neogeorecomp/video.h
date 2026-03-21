/*
 * video.h — Neo Geo LSPC (Line SPrite Controller) video system.
 *
 * The Neo Geo's video hardware is unique: it has NO background tile
 * layers. Everything is drawn with sprites, except for a fixed text
 * overlay (the "fix layer"). The LSPC manages:
 *
 *   - 381 sprites per frame (96 per scanline)
 *   - Each sprite is a vertical strip: 1 tile wide, up to 32 tiles tall
 *   - Tiles are 16x16 pixels, 4bpp (16 colors from a 256-palette pool)
 *   - Sprites can be chained horizontally via a "sticky bit"
 *   - Per-sprite shrinking (no zoom/magnification)
 *   - Hardware auto-animation (4-frame or 8-frame tile cycling)
 *   - Fix layer: 40x32 grid of 8x8 tiles, always rendered on top
 *
 * VRAM is not memory-mapped — it's accessed indirectly through three
 * registers at $3C0000-$3C0004:
 *   REG_VRAMADDR ($3C0000) — set the VRAM address
 *   REG_VRAMRW   ($3C0002) — read/write VRAM data
 *   REG_VRAMMOD  ($3C0004) — auto-increment value after each access
 *
 * VRAM layout:
 *   $0000-$6FFF  SCB1 — sprite tilemaps (tile numbers, palettes, flip)
 *   $7000-$74FF  Fix layer tilemap
 *   $8000-$81FF  SCB2 — sprite shrink coefficients
 *   $8200-$83FF  SCB3 — sprite Y position, height, sticky bit
 *   $8400-$85FF  SCB4 — sprite X position
 */

#ifndef NEOGEORECOMP_VIDEO_H
#define NEOGEORECOMP_VIDEO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Constants ----- */

#define NEOGEO_SCREEN_WIDTH   320
#define NEOGEO_SCREEN_HEIGHT  224
#define NEOGEO_MAX_SPRITES    381
#define NEOGEO_MAX_SCANLINE_SPRITES 96
#define NEOGEO_FIX_COLS       40
#define NEOGEO_FIX_ROWS       32
#define NEOGEO_VRAM_SIZE      0x8800  /* 68 KB: 64 KB lower + 4 KB upper */

/* ----- Initialization ----- */

/*
 * Initialize the video subsystem.
 *
 * Allocates VRAM, loads S ROM (fix layer tiles) and C ROM (sprite tiles),
 * and prepares the software renderer.
 */
int video_init(void);
void video_shutdown(void);

/* ----- ROM Loading ----- */

/* Load sprite tile graphics (C ROMs, always in pairs). */
int video_load_crom(const char **crom_paths, int num_croms);

/* Load fix layer tile graphics (S ROM). */
int video_load_srom(const char *srom_path);

/* Load BIOS fix tiles (SFIX ROM). */
int video_load_sfix(const char *sfix_path);

/* ----- VRAM Access (called by bus layer on $3C0000-$3C0004 access) ----- */

/* Set the VRAM address pointer. */
void video_set_vram_addr(uint16_t addr);

/* Read from VRAM at the current address, then auto-increment. */
uint16_t video_read_vram(void);

/* Write to VRAM at the current address, then auto-increment. */
void video_write_vram(uint16_t val);

/* Set the VRAM address auto-increment modulo. */
void video_set_vram_mod(uint16_t mod);

/* Set the LSPC mode register (auto-animation, timer control). */
void video_set_lspc_mode(uint16_t mode);

/* Read the LSPC mode register (includes current raster line in upper byte). */
uint16_t video_get_lspc_mode(void);

/* ----- Rendering ----- */

/*
 * Render the current frame.
 *
 * Processes all 381 sprite entries from SCB1-4 and the fix layer,
 * compositing them into the framebuffer. Respects sprite priority
 * (lower-numbered sprites have higher priority), shrinking, chaining,
 * and auto-animation.
 *
 * Call this once per VBlank.
 */
void video_render_frame(uint32_t *framebuffer);

/* ----- Fix Layer Control ----- */

/*
 * Select between BIOS fix tiles (SFIX) and cartridge fix tiles (S ROM).
 * Controlled by writes to REG_BRDFIX ($3A000B) / REG_CRTFIX ($3A001B).
 */
void video_set_fix_source(bool use_bios);

/* ----- Shadow/Darken ----- */

/*
 * Enable/disable shadow mode (darkens entire display).
 * Controlled by REG_SHADOW ($3A0011) / REG_NOSHADOW ($3A0001).
 */
void video_set_shadow(bool enabled);

/* ----- Auto-Animation ----- */

/* Get the current auto-animation counter value (2-bit or 3-bit). */
uint8_t video_get_auto_anim_counter(void);

/* ----- Debug ----- */

/* Get a direct pointer to VRAM (for debug inspection). */
const uint16_t *video_get_vram_ptr(void);

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_VIDEO_H */
