/*
 * z80.h — Z80 audio CPU subsystem.
 *
 * The Neo Geo's Z80 runs at 4 MHz and is dedicated to audio control.
 * It communicates with the 68k via a single-byte latch (no shared RAM):
 *
 *   68k -> Z80: Write to REG_SOUND ($320000), triggers NMI on Z80
 *   Z80 -> 68k: Write to port $0C, 68k reads from REG_SOUND
 *
 * The Z80's job is to receive sound commands from the 68k and program
 * the YM2610 accordingly. Most games use a standard sound driver (often
 * derived from SNK's reference driver) that maps command bytes to
 * music tracks and sound effects.
 *
 * Z80 Memory Map:
 *   $0000-$7FFF  M ROM static bank (32 KB, main sound driver code)
 *   $8000-$BFFF  Switchable window 3 (16 KB, via NEO-ZMC2)
 *   $C000-$DFFF  Switchable window 2 (8 KB)
 *   $E000-$EFFF  Switchable window 1 (4 KB)
 *   $F000-$F7FF  Switchable window 0 (2 KB)
 *   $F800-$FFFF  Work RAM (2 KB)
 *
 * For static recompilation, the Z80 can be handled in two ways:
 *   1. Traditional interpretation (run a Z80 emulator core)
 *   2. HLE (high-level emulation) of known sound drivers
 *
 * Option 1 is more accurate; option 2 is simpler but game-specific.
 * We default to interpreted emulation for correctness.
 */

#ifndef NEOGEORECOMP_Z80_H
#define NEOGEORECOMP_Z80_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Initialization ----- */

int z80_init(void);
void z80_shutdown(void);

/* ----- M ROM Loading ----- */

/* Load the Z80 sound program (M1 ROM). */
int z80_load_mrom(const char *mrom_path);

/* ----- Execution ----- */

/*
 * Execute Z80 instructions for the given number of cycles.
 *
 * Called by the main frame loop to keep the Z80 in sync with
 * the 68k. At 4 MHz and ~59.19 Hz, that's ~67,614 cycles per frame.
 */
void z80_execute(int cycles);

/* ----- 68k <-> Z80 Communication ----- */

/*
 * Send a sound command from the 68k to the Z80.
 * Latches the byte and fires an NMI on the Z80.
 * Called by the bus layer when the 68k writes to $320000.
 */
void z80_send_command(uint8_t cmd);

/*
 * Read the Z80's reply byte.
 * Called by the bus layer when the 68k reads from $320000.
 */
uint8_t z80_read_reply(void);

/* ----- NMI Control ----- */

/* Enable/disable NMI (Z80 ports $08/$18). */
void z80_set_nmi_enabled(bool enabled);

/* ----- Reset ----- */

void z80_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_Z80_H */
