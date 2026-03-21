/*
 * m68k.c — Motorola 68000 CPU context implementation.
 *
 * The instruction macros live in m68k.h (they're macros, not functions,
 * for performance). This file provides the context initialization,
 * vector loading, and status register pack/unpack functions.
 */

#include <neogeorecomp/m68k.h>
#include <string.h>

/* Global CPU context — all recompiled code operates on this */
m68k_context_t g_m68k;

void m68k_init(void) {
    memset(&g_m68k, 0, sizeof(m68k_context_t));
    g_m68k.supervisor = true;   /* 68k starts in supervisor mode */
    g_m68k.int_mask = 7;        /* All interrupts masked on reset */
}

void m68k_load_vectors(const uint8_t *rom) {
    /*
     * The first 8 bytes of the 68k ROM contain:
     *   $000000-$000003: Initial Supervisor Stack Pointer (SSP)
     *   $000004-$000007: Initial Program Counter (PC)
     *
     * Both are 32-bit big-endian values.
     */
    g_m68k.ssp = ((uint32_t)rom[0] << 24) | ((uint32_t)rom[1] << 16) |
                 ((uint32_t)rom[2] << 8)  | (uint32_t)rom[3];
    g_m68k.pc  = ((uint32_t)rom[4] << 24) | ((uint32_t)rom[5] << 16) |
                 ((uint32_t)rom[6] << 8)  | (uint32_t)rom[7];

    /* A7 (stack pointer) = SSP in supervisor mode */
    g_m68k.a[7] = g_m68k.ssp;
}

uint8_t m68k_get_ccr(void) {
    return (g_m68k.flag_x ? 0x10 : 0) |
           (g_m68k.flag_n ? 0x08 : 0) |
           (g_m68k.flag_z ? 0x04 : 0) |
           (g_m68k.flag_v ? 0x02 : 0) |
           (g_m68k.flag_c ? 0x01 : 0);
}

void m68k_set_ccr(uint8_t ccr) {
    g_m68k.flag_x = (ccr & 0x10) != 0;
    g_m68k.flag_n = (ccr & 0x08) != 0;
    g_m68k.flag_z = (ccr & 0x04) != 0;
    g_m68k.flag_v = (ccr & 0x02) != 0;
    g_m68k.flag_c = (ccr & 0x01) != 0;
}

uint16_t m68k_get_sr(void) {
    uint16_t sr = m68k_get_ccr();
    if (g_m68k.supervisor) sr |= 0x2000;
    sr |= (uint16_t)(g_m68k.int_mask & 7) << 8;
    if (g_m68k.trace) sr |= 0x8000;
    return sr;
}

void m68k_set_sr(uint16_t sr) {
    m68k_set_ccr((uint8_t)(sr & 0xFF));
    g_m68k.supervisor = (sr & 0x2000) != 0;
    g_m68k.int_mask = (uint8_t)((sr >> 8) & 7);
    g_m68k.trace = (sr & 0x8000) != 0;
}
