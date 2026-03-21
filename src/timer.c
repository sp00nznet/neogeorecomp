/*
 * timer.c — Neo Geo interrupt and timer system implementation.
 *
 * Manages VBlank (IRQ1), timer (IRQ2), and watchdog timing.
 *
 * The timer is a 32-bit down-counter clocked at 6 MHz (half the
 * 12 MHz 68k clock). It fires IRQ2 when it reaches zero, then
 * reloads and continues counting. Games use this for raster effects
 * by setting the timer to fire at specific scanlines.
 */

#include <neogeorecomp/timer.h>
#include <string.h>
#include <stdio.h>

/* ----- Internal State ----- */

static uint32_t s_timer_reload = 0;    /* Timer reload value */
static uint32_t s_timer_counter = 0;   /* Current timer counter */
static bool s_timer_stop_border = false;

static bool s_vblank_pending = false;
static bool s_timer_pending = false;
static uint16_t s_scanline = 0;

/* Watchdog: counts frames since last kick */
static int s_watchdog_counter = 0;
#define WATCHDOG_TIMEOUT 8  /* Approximately 8 frames */

/* Neo Geo timing constants */
#define SCANLINES_PER_FRAME   264
#define VBLANK_START_LINE     224

/* Cycles of the 6 MHz timer clock per scanline */
/* 6 MHz / (59.185606 Hz * 264 lines) = ~384 timer ticks per scanline */
#define TIMER_TICKS_PER_SCANLINE 384

/* ----- Initialization ----- */

int timer_init(void) {
    s_timer_reload = 0;
    s_timer_counter = 0;
    s_timer_stop_border = false;
    s_vblank_pending = false;
    s_timer_pending = false;
    s_scanline = 0;
    s_watchdog_counter = 0;
    return 0;
}

void timer_shutdown(void) {
    /* Nothing to free */
}

/* ----- Timer Configuration ----- */

void timer_set_reload(uint32_t value) {
    s_timer_reload = value;
    s_timer_counter = value;
}

uint32_t timer_get_counter(void) {
    return s_timer_counter;
}

void timer_set_stop_on_border(bool stop) {
    s_timer_stop_border = stop;
}

/* ----- Interrupt Management ----- */

void timer_irq_ack(uint8_t mask) {
    if (mask & 0x04) s_vblank_pending = false;  /* Bit 2: VBlank */
    if (mask & 0x02) s_timer_pending = false;   /* Bit 1: Timer */
    /* Bit 0: Reset IRQ3 — not tracked */
}

bool timer_vblank_pending(void) {
    return s_vblank_pending;
}

bool timer_timer_pending(void) {
    return s_timer_pending;
}

/* ----- Frame Timing ----- */

void timer_tick_scanline(void) {
    s_scanline++;
    if (s_scanline >= SCANLINES_PER_FRAME) {
        s_scanline = 0;
    }

    /* Fire VBlank interrupt at the start of the blanking period */
    if (s_scanline == VBLANK_START_LINE) {
        s_vblank_pending = true;
    }

    /* Decrement the timer counter */
    bool in_border = (s_scanline < 8) || (s_scanline >= VBLANK_START_LINE + 8);
    if (!(s_timer_stop_border && in_border)) {
        if (s_timer_counter > 0) {
            if (s_timer_counter > TIMER_TICKS_PER_SCANLINE) {
                s_timer_counter -= TIMER_TICKS_PER_SCANLINE;
            } else {
                s_timer_counter = 0;
                s_timer_pending = true;
                s_timer_counter = s_timer_reload;  /* Auto-reload */
            }
        }
    }
}

uint16_t timer_get_scanline(void) {
    return s_scanline;
}

/* ----- Watchdog ----- */

void timer_kick_watchdog(void) {
    s_watchdog_counter = 0;
}

bool timer_watchdog_expired(void) {
    return s_watchdog_counter >= WATCHDOG_TIMEOUT;
}

void timer_watchdog_tick(void) {
    s_watchdog_counter++;
    if (s_watchdog_counter >= WATCHDOG_TIMEOUT) {
        /* In real hardware, this triggers a reset.
         * During development, we just warn. */
        printf("[timer] WARNING: Watchdog expired! (would reset on real hardware)\n");
        s_watchdog_counter = 0;  /* Auto-reset for development */
    }
}
