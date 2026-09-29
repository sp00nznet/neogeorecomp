/*
 * bus.c — MVS/AES 68000 memory map.
 *
 *   $000000-$0FFFFF  P ROM fixed bank ($000-$07F: BIOS or cart vectors)
 *   $100000-$1FFFFF  64 KB work RAM, mirrored
 *   $200000-$2FFFFF  P ROM bank; a write to $2FFFF0-$2FFFFF selects it
 *   $300000-$3FFFFF  I/O (io.c) and the LSPC at $3C0000 (lspc.c)
 *   $400000-$7FFFFF  palette RAM, 8 KB mirrored (video.c)
 *   $800000-$BFFFFF  memory card (not present: open bus)
 *   $C00000-$CFFFFF  128 KB system ROM, mirrored
 *   $D00000-$DFFFFF  64 KB MVS backup RAM, write-protected until unlocked
 *
 * The page table only resolves plain memory; see bus.h.
 */
#include <neogeorecomp/bus.h>
#include "ng_internal.h"
#include <string.h>
#include <stdlib.h>

uint8_t *ng_rmap[256];
uint8_t *ng_wmap[256];

static ng_board_t s_board;
static uint8_t s_wram[0x10000];
static uint8_t s_backup[0x10000];
static uint8_t s_vec_page[0x10000];   /* page 0 with the BIOS vectors swapped in */
static int s_bios_vectors = 1;
static int s_backup_locked = 1;
static uint32_t s_bank;               /* offset into prom of the $200000 window */

/* The maps the mutators below edit: the live ones, or the saved copies
 * while an access log is being recorded (the live ones are all NULL then). */
static uint8_t *s_real_r[256], *s_real_w[256];
static int s_logging;
static uint8_t **R(void) { return s_logging ? s_real_r : ng_rmap; }
static uint8_t **W(void) { return s_logging ? s_real_w : ng_wmap; }

uint8_t *bus_wram(void) { return s_wram; }
uint8_t *bus_backup_ram(void) { return s_backup; }

static uint8_t *prom_page(uint32_t page) {
    /* Fixed area: P ROMs smaller than 1 MB mirror through it. */
    size_t fixed = s_board.prom_size < 0x100000 ? s_board.prom_size : 0x100000;
    return s_board.prom + ((page << 16) % fixed);
}

static void map_bank(void) {
    for (uint32_t p = 0x20; p < 0x30; p++) {
        if (s_board.prom_size > 0x100000) {
            size_t banked = s_board.prom_size - 0x100000;
            R()[p] = s_board.prom + 0x100000 + ((s_bank + ((p - 0x20) << 16)) % banked);
        } else {
            R()[p] = prom_page(p - 0x20);
        }
    }
}

void bus_select_bios_vectors(int bios) {
    s_bios_vectors = bios;
    R()[0] = bios ? s_vec_page : prom_page(0);
}

void bus_set_backup_lock(int locked) {
    s_backup_locked = locked;
    for (uint32_t p = 0xD0; p < 0xE0; p++)
        W()[p] = (s_board.mvs && !locked) ? s_backup : NULL;
}

void bus_init(const ng_board_t *board) {
    s_board = *board;
    memset(ng_rmap, 0, sizeof(ng_rmap));
    memset(ng_wmap, 0, sizeof(ng_wmap));
    for (uint32_t p = 0x00; p < 0x10; p++) ng_rmap[p] = prom_page(p);
    for (uint32_t p = 0x10; p < 0x20; p++) ng_rmap[p] = ng_wmap[p] = s_wram;
    for (uint32_t p = 0xC0; p < 0xD0; p++) ng_rmap[p] = s_board.bios + ((p & 1) << 16);
    if (s_board.mvs)
        for (uint32_t p = 0xD0; p < 0xE0; p++) ng_rmap[p] = s_backup;
    memcpy(s_vec_page, prom_page(0), 0x10000);
    memcpy(s_vec_page, s_board.bios, 0x80);
    bus_reset();
}

void bus_reset(void) {
    s_bank = 0;
    map_bank();
    bus_select_bios_vectors(1);
    bus_set_backup_lock(1);
}

/* ---- slow path: I/O, LSPC, palette, bank switch ---- */

static int is_lspc(uint32_t a) { return (a & 0xFE0000) == 0x3C0000; }
static int is_pal(uint32_t a) { return a >= 0x400000 && a < 0x800000; }

static uint16_t dev_r16(uint32_t a) {
    if (is_lspc(a)) return lspc_read(a);
    if (is_pal(a)) return pal_read(a);
    if (a >= 0x300000 && a < 0x400000)
        return (uint16_t)((io_read8(a) << 8) | io_read8(a | 1));
    return 0xFFFF;   /* open bus: memory card slot, unmapped space */
}

static uint8_t dev_r8(uint32_t a) {
    if (is_lspc(a) || is_pal(a)) {
        uint16_t w = dev_r16(a & ~1u);
        return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
    }
    if (a >= 0x300000 && a < 0x400000) return io_read8(a);
    return 0xFF;
}

static void bank_write(uint32_t a, uint16_t v) {
    if (a >= 0x2FFFF0 && s_board.prom_size > 0x100000) {
        s_bank = ((uint32_t)v & 7) << 20;
        map_bank();
    }
}

static void dev_w16(uint32_t a, uint16_t v) {
    if (is_lspc(a)) { lspc_write(a, v); return; }
    if (is_pal(a)) { pal_write(a, v); return; }
    if (a >= 0x300000 && a < 0x400000) {
        io_write8(a, (uint8_t)(v >> 8));
        io_write8(a | 1, (uint8_t)v);
        return;
    }
    if (a >= 0x200000 && a < 0x300000) bank_write(a, v);
}

static void dev_w8(uint32_t a, uint8_t v) {
    if (is_lspc(a)) {
        /* The LSPC ignores the byte strobes; the 68000 drives the byte on
         * both halves of the data bus, so a byte write stores it twice. */
        lspc_write(a & ~1u, (uint16_t)(v * 0x0101));
        return;
    }
    if (is_pal(a)) {
        uint16_t w = pal_read(a & ~1u);
        w = (a & 1) ? (uint16_t)((w & 0xFF00) | v) : (uint16_t)((w & 0x00FF) | (v << 8));
        pal_write(a & ~1u, w);
        return;
    }
    if (a >= 0x300000 && a < 0x400000) { io_write8(a, v); return; }
    if (a >= 0x200000 && a < 0x300000) bank_write(a, v);
}

/* ---- slow path entry points, with the --verify access log ---- */

static ng_bus_event_t *s_log;
static size_t s_nlog, s_caplog;

static void log_event(int write, int size, uint32_t a, uint32_t v) {
    if (s_nlog == s_caplog) {
        s_caplog = s_caplog ? s_caplog * 2 : 4096;
        s_log = (ng_bus_event_t *)realloc(s_log, s_caplog * sizeof(ng_bus_event_t));
    }
    s_log[s_nlog].write = (uint8_t)write;
    s_log[s_nlog].size = (uint8_t)size;
    s_log[s_nlog].addr = a;
    s_log[s_nlog].value = v;
    s_nlog++;
}


uint16_t bus_peek16(uint32_t a) {
    uint8_t *p = R()[(a >> 16) & 0xFF];
    if (!p) return 0xFFFF;
    p += a & 0xFFFE;
    return (uint16_t)((p[0] << 8) | p[1]);
}

void bus_log_begin(void) {
    memcpy(s_real_r, ng_rmap, sizeof(s_real_r));
    memcpy(s_real_w, ng_wmap, sizeof(s_real_w));
    memset(ng_rmap, 0, sizeof(ng_rmap));
    memset(ng_wmap, 0, sizeof(ng_wmap));
    s_nlog = 0;
    s_logging = 1;
}

size_t bus_log_end(const ng_bus_event_t **events) {
    s_logging = 0;
    memcpy(ng_rmap, s_real_r, sizeof(s_real_r));
    memcpy(ng_wmap, s_real_w, sizeof(s_real_w));
    *events = s_log;
    return s_nlog;
}

uint8_t ng_r8_slow(uint32_t a) {
    if (!s_logging) return dev_r8(a);
    uint8_t *p = s_real_r[a >> 16];
    uint8_t v = p ? p[a & 0xFFFF] : dev_r8(a);
    log_event(0, 1, a, v);
    return v;
}

uint16_t ng_r16_slow(uint32_t a) {
    if (!s_logging) return dev_r16(a);
    uint8_t *p = s_real_r[a >> 16];
    uint16_t v = p ? (uint16_t)((p[a & 0xFFFF] << 8) | p[(a & 0xFFFF) + 1]) : dev_r16(a);
    log_event(0, 2, a, v);
    return v;
}

void ng_w8_slow(uint32_t a, uint8_t v) {
    if (!s_logging) { dev_w8(a, v); return; }
    log_event(1, 1, a, v);
    uint8_t *p = s_real_w[a >> 16];
    if (p) p[a & 0xFFFF] = v; else dev_w8(a, v);
}

void ng_w16_slow(uint32_t a, uint16_t v) {
    if (!s_logging) { dev_w16(a, v); return; }
    log_event(1, 2, a, v);
    uint8_t *p = s_real_w[a >> 16];
    if (p) { p[a & 0xFFFF] = (uint8_t)(v >> 8); p[(a & 0xFFFF) + 1] = (uint8_t)v; }
    else dev_w16(a, v);
}
