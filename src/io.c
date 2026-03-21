/*
 * io.c — Neo Geo input and system I/O implementation.
 *
 * Manages controller state, DIP switches, system mode (MVS/AES),
 * and the system control register bank at $3A00xx.
 */

#include <neogeorecomp/io.h>
#include <neogeorecomp/video.h>
#include <neogeorecomp/palette.h>
#include <neogeorecomp/bus.h>
#include <neogeorecomp/timer.h>
#include <string.h>
#include <stdio.h>

/* ----- Internal State ----- */

/* Controller states (active low: 0xFF = nothing pressed) */
static uint8_t s_p1_controls = 0xFF;
static uint8_t s_p2_controls = 0xFF;
static uint8_t s_start_select = 0xFF;  /* Start/Select for both players */

static uint8_t s_dipsw = 0xFF;        /* DIP switch state */
static bool s_mvs_mode = true;
static int s_region = 0;              /* 0=Japan, 1=USA, 2=Europe */
static uint8_t s_coin_counters = 0xFF; /* Coin inputs (active low) */

/* ----- Initialization ----- */

int io_init(bool mvs_mode, int region) {
    s_p1_controls = 0xFF;
    s_p2_controls = 0xFF;
    s_start_select = 0xFF;
    s_dipsw = 0xFF;
    s_mvs_mode = mvs_mode;
    s_region = region;
    s_coin_counters = 0xFF;
    printf("[io] Mode: %s, Region: %d\n", mvs_mode ? "MVS" : "AES", region);
    return 0;
}

void io_shutdown(void) {
    /* Nothing to free */
}

/* ----- Input State ----- */

void io_update(void) {
    /* Called once per frame — input is polled by the platform layer
     * and forwarded via io_set_button(). Nothing to do here yet. */
}

void io_set_button(int player, uint8_t button, bool pressed) {
    uint8_t *state = (player == 0) ? &s_p1_controls : &s_p2_controls;
    if (pressed) {
        *state &= ~button;  /* Active low: clear bit = pressed */
    } else {
        *state |= button;   /* Set bit = released */
    }
}

void io_insert_coin(int slot) {
    (void)slot;
    /* Momentarily clear the coin bit — the game reads this on VBlank */
    s_coin_counters &= ~(1 << slot);
}

void io_press_service(void) {
    /* Service button — active low, momentary */
}

/* ----- Register Reads ----- */

uint8_t io_read_p1cnt(void) {
    return s_p1_controls;
}

uint8_t io_read_dipsw(void) {
    return s_dipsw;
}

uint8_t io_read_systype(void) {
    /* Bit 7: test button (active low), Bits 5-4: slot count, Bit 0: system ID */
    uint8_t val = 0xFF;
    return val;
}

uint8_t io_read_status_a(void) {
    /* Coin inputs, service button, RTC data */
    return s_coin_counters;
}

uint8_t io_read_p2cnt(void) {
    return s_p2_controls;
}

uint8_t io_read_status_b(void) {
    /*
     * Bit 7: (unused)
     * Bit 6: 0 = AES, 1 = MVS
     * Bit 5: Memory card write-protected
     * Bit 4: Memory card inserted
     * Bit 3: P2 Start
     * Bit 2: P2 Select
     * Bit 1: P1 Start
     * Bit 0: P1 Select
     */
    uint8_t val = s_start_select & 0x0F;  /* Start/Select bits */
    if (s_mvs_mode) val |= 0x40;          /* MVS flag */
    val |= 0xB0;                           /* No card inserted, not write-protected */
    return val;
}

/* ----- Register Writes ----- */

void io_kick_watchdog(void) {
    timer_kick_watchdog();
}

void io_write_sysctrl(uint32_t addr) {
    /*
     * System control registers at $3A00xx.
     * These are write-only, data doesn't matter — the address bit 4
     * carries the value (0 or 1).
     *
     * $3A0001/$3A0011: Shadow off/on
     * $3A0003/$3A0013: BIOS vectors / Cart vectors
     * $3A000B/$3A001B: BIOS fix tiles / Cart fix tiles
     * $3A000D/$3A001D: SRAM lock / unlock
     * $3A000F/$3A001F: Palette bank 1 / bank 0
     */
    bool bit4 = (addr & 0x10) != 0;
    uint8_t reg = (uint8_t)(addr & 0x0F);

    switch (reg) {
        case 0x01:
            video_set_shadow(bit4);
            break;
        case 0x03:
            bus_set_vector_source(!bit4);  /* 0 = BIOS, 1 = cart */
            break;
        case 0x05:
            /* Memory card unlock/lock — not implemented */
            break;
        case 0x0B:
            video_set_fix_source(!bit4);  /* 0 = BIOS, 1 = cart */
            break;
        case 0x0D:
            /* SRAM lock/unlock */
            /* bit4 = 1 means unlock ($3A001D), bit4 = 0 means lock ($3A000D) */
            break;
        case 0x0F:
            palette_set_bank(bit4 ? 0 : 1);
            break;
        default:
            break;
    }
}

void io_set_dipsw(uint8_t value) {
    s_dipsw = value;
}
