/*
 * io.h — Neo Geo input and system I/O registers.
 *
 * Handles controller input, DIP switches, system status, and the
 * various system control registers in the $300000-$3FFFFF range.
 *
 * Controller layout (active low — 0 = pressed):
 *   Bit 0: Up       Bit 4: A
 *   Bit 1: Down     Bit 5: B
 *   Bit 2: Left     Bit 6: C
 *   Bit 3: Right    Bit 7: D
 *
 * System status (REG_STATUS_B at $380000):
 *   Bit 0: P1 Select    Bit 4: Memory card inserted
 *   Bit 1: P1 Start     Bit 5: Memory card write-protected
 *   Bit 2: P2 Select    Bit 6: System type (0=AES, 1=MVS)
 *   Bit 3: P2 Start     Bit 7: (unused)
 */

#ifndef NEOGEORECOMP_IO_H
#define NEOGEORECOMP_IO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Initialization ----- */

int io_init(bool mvs_mode, int region);
void io_shutdown(void);

/* ----- Input State ----- */

/*
 * Update controller state from the platform layer.
 * Called once per frame by the main loop.
 */
void io_update(void);

/*
 * Set the state of a controller button.
 * player: 0 or 1
 * button: one of the IO_BTN_* constants
 * pressed: true if the button is held down
 */
void io_set_button(int player, uint8_t button, bool pressed);

/* Button constants */
#define IO_BTN_UP      0x01
#define IO_BTN_DOWN    0x02
#define IO_BTN_LEFT    0x04
#define IO_BTN_RIGHT   0x08
#define IO_BTN_A       0x10
#define IO_BTN_B       0x20
#define IO_BTN_C       0x40
#define IO_BTN_D       0x80

#define IO_BTN_SELECT  0x01
/* STATUS_B: bit 0 = Select, bit 1 = Start (active low) */
#define IO_BTN_START   0x02

/* Coin/service */
void io_insert_coin(int slot);   /* slot: 0-3 */
void io_press_service(void);

/* ----- Register Reads (called by bus layer) ----- */

uint8_t io_read_p1cnt(void);         /* $300000: Player 1 controls */
uint8_t io_read_dipsw(void);         /* $300001: DIP switches */
uint8_t io_read_systype(void);       /* $300081: System type / test button */
uint8_t io_read_status_a(void);      /* $320001: Coins, RTC, service */
uint8_t io_read_p2cnt(void);         /* $340000: Player 2 controls */
uint8_t io_read_status_b(void);      /* $380000: Start/Select, card, AES/MVS */

/* ----- Register Writes (called by bus layer) ----- */

/* Watchdog kick (write to $300001). */
void io_kick_watchdog(void);

/* System control registers ($3A00xx). */
void io_write_sysctrl(uint32_t addr);

/* ----- DIP Switch Configuration ----- */

/*
 * Set DIP switch configuration.
 * Used to configure freeplay, difficulty, etc.
 */
void io_set_dipsw(uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_IO_H */
