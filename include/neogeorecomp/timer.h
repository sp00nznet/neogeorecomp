/*
 * timer.h — Neo Geo interrupt and timer system.
 *
 * The 68000 has 7 interrupt priority levels. The Neo Geo uses three:
 *
 *   IRQ1 (VBlank): Fires at the start of vertical blanking (~60 Hz).
 *                  This is the main game timing interrupt.
 *
 *   IRQ2 (Timer):  Driven by a 32-bit down-counter clocked at 6 MHz.
 *                  Used for scanline effects (raster interrupts).
 *                  Range: ~167 ns to ~11.9 minutes.
 *
 *   IRQ3 (Reset):  Cold boot only (active after power-on until first
 *                  acknowledge). Not used during normal operation.
 *
 * Acknowledgment: Write to REG_IRQACK ($3C000C):
 *   Bit 2: Acknowledge VBlank (IRQ1)
 *   Bit 1: Acknowledge Timer (IRQ2)
 *   Bit 0: Acknowledge Reset (IRQ3)
 *
 * The watchdog timer must be kicked every few frames by writing
 * to REG_DIPSW ($300001). If not kicked, the hardware resets.
 * Timeout is approximately 8 frames (~128 ms).
 */

#ifndef NEOGEORECOMP_TIMER_H
#define NEOGEORECOMP_TIMER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Initialization ----- */

int timer_init(void);
void timer_shutdown(void);

/* ----- Timer Configuration (REG_TIMERHIGH/LOW at $3C0008/$3C000A) ----- */

/* Set the 32-bit timer reload value. */
void timer_set_reload(uint32_t value);

/* Read the current timer counter value. */
uint32_t timer_get_counter(void);

/* Control timer stop-during-border behavior (REG_TIMERSTOP). */
void timer_set_stop_on_border(bool stop);

/* ----- Interrupt Management ----- */

/*
 * Acknowledge interrupts.
 * mask: bitmask (bit 2 = VBlank, bit 1 = Timer, bit 0 = Reset)
 * Called by the bus layer when 68k writes to $3C000C.
 */
void timer_irq_ack(uint8_t mask);

/* Check if a VBlank interrupt is pending. */
bool timer_vblank_pending(void);

/* Check if a timer interrupt is pending. */
bool timer_timer_pending(void);

/* ----- Frame Timing ----- */

/*
 * Advance timing by one scanline.
 *
 * Called 264 times per frame (NTSC). Decrements the timer counter,
 * fires IRQ2 when the counter reaches zero, and fires IRQ1 at the
 * start of VBlank (scanline 224).
 */
void timer_tick_scanline(void);

/* Get the current scanline number (0-263). */
uint16_t timer_get_scanline(void);

/* ----- Watchdog ----- */

/*
 * Kick the watchdog timer.
 * Called by the bus layer when 68k writes to $300001.
 * Resets the watchdog counter.
 */
void timer_kick_watchdog(void);

/*
 * Check if the watchdog has expired.
 * Returns true if too many frames have passed without a kick.
 */
bool timer_watchdog_expired(void);

/* Advance the watchdog by one frame. */
void timer_watchdog_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_TIMER_H */
