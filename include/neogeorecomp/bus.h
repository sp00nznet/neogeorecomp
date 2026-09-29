/*
 * bus.h — the 68000 address space of an MVS/AES board.
 *
 * Reads and writes go through a 256-entry page table of 64 KB pages.
 * ROM, work RAM and backup RAM resolve to a host pointer and are accessed
 * inline; everything with side effects (I/O, LSPC, palette, bank switch)
 * leaves the page NULL and takes the slow path in bus.c.
 *
 * All memory is held big-endian, exactly as the 68000 sees it.
 */
#ifndef NEOGEORECOMP_BUS_H
#define NEOGEORECOMP_BUS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t *ng_rmap[256];
extern uint8_t *ng_wmap[256];

uint8_t  ng_r8_slow(uint32_t a);
uint16_t ng_r16_slow(uint32_t a);
void     ng_w8_slow(uint32_t a, uint8_t v);
void     ng_w16_slow(uint32_t a, uint16_t v);

static inline uint8_t ng_r8(uint32_t a) {
    a &= 0xFFFFFF;
    uint8_t *p = ng_rmap[a >> 16];
    return p ? p[a & 0xFFFF] : ng_r8_slow(a);
}
static inline uint16_t ng_r16(uint32_t a) {
    a &= 0xFFFFFE;
    uint8_t *p = ng_rmap[a >> 16];
    if (p) { p += a & 0xFFFF; return (uint16_t)((p[0] << 8) | p[1]); }
    return ng_r16_slow(a);
}
static inline uint32_t ng_r32(uint32_t a) {
    return ((uint32_t)ng_r16(a) << 16) | ng_r16(a + 2);
}
static inline void ng_w8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    uint8_t *p = ng_wmap[a >> 16];
    if (p) p[a & 0xFFFF] = v; else ng_w8_slow(a, v);
}
static inline void ng_w16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFE;
    uint8_t *p = ng_wmap[a >> 16];
    if (p) { p += a & 0xFFFF; p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
    else ng_w16_slow(a, v);
}
static inline void ng_w32(uint32_t a, uint32_t v) {
    ng_w16(a, (uint16_t)(v >> 16));
    ng_w16(a + 2, (uint16_t)v);
}

/* Stack helpers used by recompiled JSR/RTS/exception code. */
static inline void ng_push16(uint16_t v);
static inline void ng_push32(uint32_t v);
static inline uint16_t ng_pop16(void);
static inline uint32_t ng_pop32(void);

/* ---- access log for --verify ----
 * While logging, every access takes the slow path and every data read and
 * write is recorded in order, so the interpreter can replay the same block
 * against the same values (instruction fetches are not data reads). */
typedef struct {
    uint8_t  write, size;
    uint32_t addr, value;
} ng_bus_event_t;

void bus_log_begin(void);
size_t bus_log_end(const ng_bus_event_t **events);
uint16_t bus_peek16(uint32_t a);             /* plain-memory read, no side effects, no log */

/* ---- board setup (called by the loader) ---- */
typedef struct {
    uint8_t *prom;          /* P ROM, 68k byte order; fixed bank first */
    size_t   prom_size;
    uint8_t *bios;          /* 128 KB system ROM, 68k byte order */
    int      mvs;           /* 1 = MVS (backup RAM, coin slots), 0 = AES */
} ng_board_t;

void bus_init(const ng_board_t *board);
void bus_reset(void);
uint8_t *bus_wram(void);            /* 64 KB work RAM */
uint8_t *bus_backup_ram(void);      /* 64 KB MVS backup RAM */
void bus_select_bios_vectors(int bios);
void bus_set_backup_lock(int locked);

#ifdef __cplusplus
}
#endif

#include "cpu.h"

static inline void ng_push16(uint16_t v) { CPU.a[7] -= 2; ng_w16(CPU.a[7], v); }
static inline void ng_push32(uint32_t v) { CPU.a[7] -= 4; ng_w32(CPU.a[7], v); }
static inline uint16_t ng_pop16(void) { uint16_t v = ng_r16(CPU.a[7]); CPU.a[7] += 2; return v; }
static inline uint32_t ng_pop32(void) { uint32_t v = ng_r32(CPU.a[7]); CPU.a[7] += 4; return v; }

#endif
