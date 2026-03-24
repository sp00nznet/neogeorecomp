/*
 * bus.h — Neo Geo memory bus and address space routing.
 *
 * The 68000 has a 24-bit address bus (16 MB address space). Every memory
 * access from recompiled code goes through this bus layer, which routes
 * reads and writes to the appropriate hardware subsystem:
 *
 *   $000000-$0FFFFF  P ROM bank 1 (fixed, vectors + main code)
 *   $100000-$10FFFF  Work RAM (64 KB)
 *   $200000-$2FFFFF  P ROM bank 2 (bankswitchable for ROMs > 2 MB)
 *   $300000-$3FFFFF  I/O registers (input, video, system control)
 *   $400000-$401FFF  Palette RAM (8 KB, dual-banked)
 *   $800000-$BFFFFF  Memory card
 *   $C00000-$C1FFFF  System ROM / BIOS (128 KB)
 *   $D00000-$D0FFFF  Backup RAM (MVS only, battery-backed SRAM)
 *
 * The bus handles big-endian byte ordering (the 68000 is big-endian,
 * x86 is little-endian). All read/write functions perform the necessary
 * byte swapping transparently.
 *
 * For performance, Work RAM has fast-path accessors (bus_wram_read/write)
 * that skip the address decoder. Use these when you know the access is
 * to Work RAM (e.g., recompiled code accessing known RAM variables).
 */

#ifndef NEOGEORECOMP_BUS_H
#define NEOGEORECOMP_BUS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Initialization ----- */

/*
 * Initialize the bus subsystem.
 *
 * Allocates Work RAM, Palette RAM, and Backup RAM. Must be called
 * after ROM loading but before any bus access.
 */
int bus_init(void);

/* Free all bus-allocated memory. */
void bus_shutdown(void);

/* ----- ROM Loading ----- */

/*
 * Load the P ROM (68k program code) from file(s).
 *
 * For games with a single P1 ROM (<= 1 MB), only p1_path is needed.
 * For games with P1 + P2 ROMs, both paths are required.
 *
 * The bus handles the Neo Geo's reversed mapping for 2 MB P ROMs
 * (second MiB at $000000, first at $200000) internally.
 */
int bus_load_prom(const char *p1_path, const char *p2_path);

/* Load the system BIOS ROM (128 KB, maps to $C00000). */
int bus_load_bios(const char *bios_path);

/* ----- General Bus Access (big-endian, address-decoded) ----- */

uint8_t  bus_read8(uint32_t addr);
uint16_t bus_read16(uint32_t addr);
uint32_t bus_read32(uint32_t addr);

void bus_write8(uint32_t addr, uint8_t val);
void bus_write16(uint32_t addr, uint16_t val);
void bus_write32(uint32_t addr, uint32_t val);

/* BIOS-privileged write — bypasses protection on BIOS-owned addresses */
void bus_bios_write8(uint32_t addr, uint8_t val);

/* ----- Fast Work RAM Access (offset into $100000 base) ----- */

/*
 * These skip the address decoder for performance. The offset is
 * relative to the Work RAM base ($100000), not the full 68k address.
 *
 * Example: bus_wram_read16(0x0000) reads $100000 (first word of Work RAM).
 */
uint8_t  bus_wram_read8(uint32_t offset);
uint16_t bus_wram_read16(uint32_t offset);
uint32_t bus_wram_read32(uint32_t offset);

void bus_wram_write8(uint32_t offset, uint8_t val);
void bus_wram_write16(uint32_t offset, uint16_t val);
void bus_wram_write32(uint32_t offset, uint32_t val);

/* ----- P ROM Banking ----- */

/*
 * Set the P ROM bank for the $200000-$2FFFFF window.
 *
 * For games <= 2 MB, this is never called (the mapping is fixed).
 * For larger games, the game code writes to $200000-$2FFFFF odd addresses
 * to select which 1 MB slice of the P2 ROM appears in that window.
 *
 * The bus layer intercepts writes to $2xxxxx and calls this internally.
 */
void bus_set_prom_bank(uint8_t bank);

/* Get the currently active P ROM bank number. */
uint8_t bus_get_prom_bank(void);

/* ----- Vector Table Swap ----- */

/*
 * The Neo Geo can swap between BIOS vectors and cartridge vectors
 * at $000000-$000007 (SSP and PC reset vectors) and the full
 * exception vector table. This is controlled by writes to:
 *   $3A0003 (REG_SWPBIOS) — use BIOS vectors
 *   $3A0013 (REG_SWPROM)  — use cartridge vectors
 */
void bus_set_vector_source(bool use_bios);
bool bus_get_vector_source(void);

/* ----- Direct ROM Pointer (for tools/analysis) ----- */

/* Get a pointer to the raw P ROM data. */
const uint8_t *bus_get_prom_ptr(void);
uint32_t bus_get_prom_size(void);

/* Get a pointer to the raw Work RAM. */
uint8_t *bus_get_wram_ptr(void);

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_BUS_H */
