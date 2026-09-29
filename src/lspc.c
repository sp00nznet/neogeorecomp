/*
 * lspc.c — the LSPC video controller's CPU-facing side: VRAM access
 * ports, the raster line counter, auto-animation and the timer IRQ.
 *
 *   $3C0000 VRAMADDR   (w) set address; (r) VRAM data
 *   $3C0002 VRAMRW     (rw) data; a write advances the address by MOD
 *   $3C0004 VRAMMOD    (rw) signed address increment
 *   $3C0006 LSPCMODE   (w) anim speed / timer control; (r) line counter
 *   $3C0008 TIMERHIGH, $3C000A TIMERLOW, $3C000C IRQACK, $3C000E TIMERSTOP
 *
 * VRAM is 32K words of slow VRAM (sprite tilemaps, fix map) plus 2K words
 * of fast VRAM at $8000 (SCB2-4, sprite lists). The address increment
 * wraps inside the half the address is in, keeping bit 15.
 *
 * The timer counts pixel clocks (6 MHz = two 68k clocks). It fires
 * reload+1 pixels after being loaded; the control bits decide whether it
 * raises IRQ2 and whether it reloads on TIMERLOW writes, at VBlank, or
 * when it expires.
 */
#include "ng_internal.h"
#include <neogeorecomp/cpu.h>
#include <string.h>

uint16_t lspc_vram[0x10000];
uint8_t  lspc_anim_counter;

static uint16_t s_addr, s_mod;
static uint8_t  s_anim_speed, s_anim_frames, s_anim_disabled;
static uint8_t  s_timer_ctrl;
static uint32_t s_timer_reload;
static int64_t  s_timer_deadline = INT64_MAX;

enum {
    TMR_IRQ_ENABLE   = 0x10,
    TMR_LOAD_ON_LOW  = 0x20,
    TMR_LOAD_VBLANK  = 0x40,
    TMR_LOAD_REPEAT  = 0x80,
};

static uint32_t vram_index(uint16_t a) {
    return (a & 0x8000) ? (0x8000u | (a & 0x07FFu)) : a;
}

void lspc_reset(void) {
    memset(lspc_vram, 0, sizeof(lspc_vram));
    s_addr = 0; s_mod = 1;
    s_anim_speed = 0; s_anim_frames = 0; s_anim_disabled = 0; lspc_anim_counter = 0;
    s_timer_ctrl = 0; s_timer_reload = 0; s_timer_deadline = INT64_MAX;
}

static void timer_load(int64_t from) {
    if (s_timer_reload == 0xFFFFFFFFu) { s_timer_deadline = INT64_MAX; return; }
    s_timer_deadline = from + ((int64_t)s_timer_reload + 1) * 2;
    exec_event_changed(s_timer_deadline);
}

int64_t lspc_timer_deadline(void) { return s_timer_deadline; }

void lspc_timer_fire(void) {
    int64_t at = s_timer_deadline;
    s_timer_deadline = INT64_MAX;
    if (s_timer_ctrl & TMR_IRQ_ENABLE) ng_irq_pending |= 2;
    if (s_timer_ctrl & TMR_LOAD_REPEAT) timer_load(at);
}

void lspc_vblank(void) {
    ng_irq_pending |= 1;
    if (!s_anim_disabled) {
        if (s_anim_frames == 0) {
            s_anim_frames = s_anim_speed;
            lspc_anim_counter++;
        } else {
            s_anim_frames--;
        }
    }
    if (s_timer_ctrl & TMR_LOAD_VBLANK) timer_load(ng_cycles);
}

uint16_t lspc_read(uint32_t addr) {
    switch (addr & 0x6) {
    case 0x0:
    case 0x2: return lspc_vram[vram_index(s_addr)];
    case 0x4: return s_mod;
    default: {
        /* Line counter: $F8 during the top border, counting up to $1FF at
         * the bottom of the frame; low bits carry the anim counter. */
        int v = ng_vpos() + 0x100;
        if (v >= 0x200) v -= NG_LINES_PER_FRAME;
        return (uint16_t)((v << 7) | (lspc_anim_counter & 7));
    }
    }
}

void lspc_write(uint32_t addr, uint16_t v) {
    switch (addr & 0xE) {
    case 0x0: s_addr = v; break;
    case 0x2:
        lspc_vram[vram_index(s_addr)] = v;
        s_addr = (uint16_t)((s_addr & 0x8000) | ((s_addr + s_mod) & 0x7FFF));
        break;
    case 0x4: s_mod = v; break;
    case 0x6:
        s_anim_speed = (uint8_t)(v >> 8);
        s_anim_disabled = (v & 0x08) != 0;
        s_timer_ctrl = (uint8_t)(v & 0xF0);
        break;
    case 0x8: s_timer_reload = (s_timer_reload & 0x0000FFFFu) | ((uint32_t)v << 16); break;
    case 0xA:
        s_timer_reload = (s_timer_reload & 0xFFFF0000u) | v;
        if (s_timer_ctrl & TMR_LOAD_ON_LOW) timer_load(ng_cycles);
        break;
    case 0xC:
        if (v & 1) ng_irq_pending &= (uint8_t)~4;
        if (v & 2) ng_irq_pending &= (uint8_t)~2;
        if (v & 4) ng_irq_pending &= (uint8_t)~1;
        exec_irq_changed();
        break;
    case 0xE: break;   /* TIMERSTOP only matters on PAL boards */
    }
}
