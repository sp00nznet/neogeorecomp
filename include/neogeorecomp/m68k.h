/*
 * m68k.h — Motorola 68000 CPU context and instruction macros.
 *
 * This file defines the runtime representation of the 68000's register
 * file and provides C macros that faithfully reproduce every 68k instruction's
 * behavior, including condition code flag updates.
 *
 * The approach: each 68k instruction becomes a macro call that operates on
 * the global CPU context (g_m68k). For example:
 *
 *   Original 68k:    add.w  d0, d1
 *   Recompiled C:    M68K_ADD16(g_m68k.d[1], g_m68k.d[0])
 *
 * The macro updates d1, then sets/clears the C, V, Z, N, X flags exactly
 * as the real 68000 would. This is critical for correct recompilation —
 * subsequent branch instructions (beq, bne, bgt, etc.) test these flags.
 *
 * Architecture note: The 68000 is big-endian with 32-bit registers. When
 * an instruction operates on a byte (.b) or word (.w), only the low 8 or
 * 16 bits of the register are affected — the upper bits are preserved.
 * The macros handle this correctly.
 *
 * Adapted from genrecomp (sp00nznet/genrecomp) for Neo Geo use.
 * The 68000 CPU is identical across Genesis, Neo Geo, CPS1/2, and System 16.
 */

#ifndef NEOGEORECOMP_M68K_H
#define NEOGEORECOMP_M68K_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- CPU Context ----- */

/*
 * The complete state of the 68000 CPU.
 *
 * D0-D7: 32-bit data registers (general purpose)
 * A0-A6: 32-bit address registers (pointer operations)
 * A7/USP: User stack pointer
 * SSP: Supervisor stack pointer (used during interrupts)
 * PC: Program counter (24-bit effective, stored as 32-bit)
 *
 * Condition code flags are stored as individual bools for fast
 * access from recompiled code. The real 68k packs them into the
 * lower byte of the status register (SR), but separate bools
 * avoid bit manipulation overhead on every instruction.
 */
typedef struct {
    uint32_t d[8];      /* Data registers D0-D7 */
    uint32_t a[8];      /* Address registers A0-A7 (A7 = active stack pointer) */
    uint32_t usp;       /* User stack pointer (saved when in supervisor mode) */
    uint32_t ssp;       /* Supervisor stack pointer */
    uint32_t pc;        /* Program counter */

    /* Condition Code Register (CCR) flags — stored individually for speed */
    bool flag_c;        /* Carry: set on unsigned overflow/borrow */
    bool flag_v;        /* Overflow: set on signed overflow */
    bool flag_z;        /* Zero: set when result is zero */
    bool flag_n;        /* Negative: set when result MSB is 1 */
    bool flag_x;        /* Extend: "sticky carry" for multi-precision arithmetic */

    /* Status register upper byte */
    bool supervisor;    /* Supervisor mode flag (S bit) */
    uint8_t int_mask;   /* Interrupt priority mask (3 bits, levels 0-7) */
    bool trace;         /* Trace mode flag (T bit) */

    /* Internal state */
    bool stopped;       /* CPU is stopped (STOP instruction executed) */
    bool halted;        /* CPU is halted (double bus fault) */
} m68k_context_t;

/*
 * Global CPU context — all recompiled code operates on this.
 *
 * We use a single global rather than passing a pointer because:
 * 1. Recompiled code is generated mechanically and benefits from simplicity
 * 2. There's only ever one 68000 in a Neo Geo (the Z80 has its own context)
 * 3. The compiler can optimize global access patterns well
 */
extern m68k_context_t g_m68k;

/* ----- Initialization ----- */

/* Reset the CPU context to power-on state. */
void m68k_init(void);

/* Load initial SSP and PC from the vector table (first 8 bytes of ROM). */
void m68k_load_vectors(const uint8_t *rom);

/* ----- Status Register Helpers ----- */

/* Pack CCR flags into the 8-bit CCR value (for MOVE to CCR, etc.) */
uint8_t m68k_get_ccr(void);

/* Unpack an 8-bit CCR value into the individual flag bools. */
void m68k_set_ccr(uint8_t ccr);

/* Pack full 16-bit SR (supervisor byte + CCR). */
uint16_t m68k_get_sr(void);

/* Unpack full 16-bit SR. */
void m68k_set_sr(uint16_t sr);

/* ----- Instruction Macros ----- */

/*
 * Naming convention:
 *   M68K_{INSTRUCTION}{SIZE}(dst, src)
 *
 * Sizes: 8 = byte, 16 = word, 32 = long
 *
 * All macros update the CCR flags as the real 68000 does.
 * dst is modified in place. src is read-only.
 *
 * For instructions that only set flags (CMP, TST, BTST), dst is not modified.
 */

/* --- ADD: dst = dst + src, update XNZVC --- */
#define M68K_ADD8(dst, src) do { \
    uint8_t _s = (uint8_t)(src); \
    uint8_t _d = (uint8_t)(dst); \
    uint16_t _r = (uint16_t)_d + (uint16_t)_s; \
    uint8_t _res = (uint8_t)_r; \
    g_m68k.flag_c = g_m68k.flag_x = (_r > 0xFF); \
    g_m68k.flag_v = ((_d ^ _res) & (_s ^ _res) & 0x80) != 0; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80) != 0; \
    (dst) = ((dst) & 0xFFFFFF00u) | _res; \
} while(0)

#define M68K_ADD16(dst, src) do { \
    uint16_t _s = (uint16_t)(src); \
    uint16_t _d = (uint16_t)(dst); \
    uint32_t _r = (uint32_t)_d + (uint32_t)_s; \
    uint16_t _res = (uint16_t)_r; \
    g_m68k.flag_c = g_m68k.flag_x = (_r > 0xFFFF); \
    g_m68k.flag_v = ((_d ^ _res) & (_s ^ _res) & 0x8000) != 0; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x8000) != 0; \
    (dst) = ((dst) & 0xFFFF0000u) | _res; \
} while(0)

#define M68K_ADD32(dst, src) do { \
    uint32_t _s = (uint32_t)(src); \
    uint32_t _d = (uint32_t)(dst); \
    uint64_t _r = (uint64_t)_d + (uint64_t)_s; \
    uint32_t _res = (uint32_t)_r; \
    g_m68k.flag_c = g_m68k.flag_x = (_r > 0xFFFFFFFFu); \
    g_m68k.flag_v = ((_d ^ _res) & (_s ^ _res) & 0x80000000u) != 0; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
    (dst) = _res; \
} while(0)

/* --- SUB: dst = dst - src, update XNZVC --- */
#define M68K_SUB8(dst, src) do { \
    uint8_t _s = (uint8_t)(src); \
    uint8_t _d = (uint8_t)(dst); \
    uint16_t _r = (uint16_t)_d - (uint16_t)_s; \
    uint8_t _res = (uint8_t)_r; \
    g_m68k.flag_c = g_m68k.flag_x = (_d < _s); \
    g_m68k.flag_v = ((_d ^ _s) & (_d ^ _res) & 0x80) != 0; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80) != 0; \
    (dst) = ((dst) & 0xFFFFFF00u) | _res; \
} while(0)

#define M68K_SUB16(dst, src) do { \
    uint16_t _s = (uint16_t)(src); \
    uint16_t _d = (uint16_t)(dst); \
    uint32_t _r = (uint32_t)_d - (uint32_t)_s; \
    uint16_t _res = (uint16_t)_r; \
    g_m68k.flag_c = g_m68k.flag_x = (_d < _s); \
    g_m68k.flag_v = ((_d ^ _s) & (_d ^ _res) & 0x8000) != 0; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x8000) != 0; \
    (dst) = ((dst) & 0xFFFF0000u) | _res; \
} while(0)

#define M68K_SUB32(dst, src) do { \
    uint32_t _s = (uint32_t)(src); \
    uint32_t _d = (uint32_t)(dst); \
    uint64_t _r = (uint64_t)_d - (uint64_t)_s; \
    uint32_t _res = (uint32_t)_r; \
    g_m68k.flag_c = g_m68k.flag_x = (_d < _s); \
    g_m68k.flag_v = ((_d ^ _s) & (_d ^ _res) & 0x80000000u) != 0; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
    (dst) = _res; \
} while(0)

/* --- CMP: compare (subtract without storing), update NZVC (not X) --- */
#define M68K_CMP8(dst, src) do { \
    uint8_t _s = (uint8_t)(src); \
    uint8_t _d = (uint8_t)(dst); \
    uint16_t _r = (uint16_t)_d - (uint16_t)_s; \
    uint8_t _res = (uint8_t)_r; \
    g_m68k.flag_c = (_d < _s); \
    g_m68k.flag_v = ((_d ^ _s) & (_d ^ _res) & 0x80) != 0; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80) != 0; \
} while(0)

#define M68K_CMP16(dst, src) do { \
    uint16_t _s = (uint16_t)(src); \
    uint16_t _d = (uint16_t)(dst); \
    uint32_t _r = (uint32_t)_d - (uint32_t)_s; \
    uint16_t _res = (uint16_t)_r; \
    g_m68k.flag_c = (_d < _s); \
    g_m68k.flag_v = ((_d ^ _s) & (_d ^ _res) & 0x8000) != 0; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x8000) != 0; \
} while(0)

#define M68K_CMP32(dst, src) do { \
    uint32_t _s = (uint32_t)(src); \
    uint32_t _d = (uint32_t)(dst); \
    uint64_t _r = (uint64_t)_d - (uint64_t)_s; \
    uint32_t _res = (uint32_t)_r; \
    g_m68k.flag_c = (_d < _s); \
    g_m68k.flag_v = ((_d ^ _s) & (_d ^ _res) & 0x80000000u) != 0; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
} while(0)

/* --- AND: dst = dst & src, update NZV (clear C, V) --- */
#define M68K_AND8(dst, src) do { \
    uint8_t _res = (uint8_t)(dst) & (uint8_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80) != 0; \
    (dst) = ((dst) & 0xFFFFFF00u) | _res; \
} while(0)

#define M68K_AND16(dst, src) do { \
    uint16_t _res = (uint16_t)(dst) & (uint16_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x8000) != 0; \
    (dst) = ((dst) & 0xFFFF0000u) | _res; \
} while(0)

#define M68K_AND32(dst, src) do { \
    uint32_t _res = (uint32_t)(dst) & (uint32_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
    (dst) = _res; \
} while(0)

/* --- OR: dst = dst | src, update NZ, clear CV --- */
#define M68K_OR8(dst, src) do { \
    uint8_t _res = (uint8_t)(dst) | (uint8_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80) != 0; \
    (dst) = ((dst) & 0xFFFFFF00u) | _res; \
} while(0)

#define M68K_OR16(dst, src) do { \
    uint16_t _res = (uint16_t)(dst) | (uint16_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x8000) != 0; \
    (dst) = ((dst) & 0xFFFF0000u) | _res; \
} while(0)

#define M68K_OR32(dst, src) do { \
    uint32_t _res = (uint32_t)(dst) | (uint32_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
    (dst) = _res; \
} while(0)

/* --- EOR: dst = dst ^ src, update NZ, clear CV --- */
#define M68K_EOR8(dst, src) do { \
    uint8_t _res = (uint8_t)(dst) ^ (uint8_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80) != 0; \
    (dst) = ((dst) & 0xFFFFFF00u) | _res; \
} while(0)

#define M68K_EOR16(dst, src) do { \
    uint16_t _res = (uint16_t)(dst) ^ (uint16_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x8000) != 0; \
    (dst) = ((dst) & 0xFFFF0000u) | _res; \
} while(0)

#define M68K_EOR32(dst, src) do { \
    uint32_t _res = (uint32_t)(dst) ^ (uint32_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
    (dst) = _res; \
} while(0)

/* --- TST: test operand, update NZ, clear CV --- */
#define M68K_TST8(val) do { \
    uint8_t _v = (uint8_t)(val); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_v == 0); \
    g_m68k.flag_n = (_v & 0x80) != 0; \
} while(0)

#define M68K_TST16(val) do { \
    uint16_t _v = (uint16_t)(val); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_v == 0); \
    g_m68k.flag_n = (_v & 0x8000) != 0; \
} while(0)

#define M68K_TST32(val) do { \
    uint32_t _v = (uint32_t)(val); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_v == 0); \
    g_m68k.flag_n = (_v & 0x80000000u) != 0; \
} while(0)

/* --- NEG: dst = 0 - dst, update XNZVC --- */
#define M68K_NEG8(dst) do { \
    uint8_t _d = (uint8_t)(dst); \
    uint8_t _res = (uint8_t)(-(int8_t)_d); \
    g_m68k.flag_c = g_m68k.flag_x = (_d != 0); \
    g_m68k.flag_v = (_d == 0x80); \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80) != 0; \
    (dst) = ((dst) & 0xFFFFFF00u) | _res; \
} while(0)

#define M68K_NEG16(dst) do { \
    uint16_t _d = (uint16_t)(dst); \
    uint16_t _res = (uint16_t)(-(int16_t)_d); \
    g_m68k.flag_c = g_m68k.flag_x = (_d != 0); \
    g_m68k.flag_v = (_d == 0x8000); \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x8000) != 0; \
    (dst) = ((dst) & 0xFFFF0000u) | _res; \
} while(0)

#define M68K_NEG32(dst) do { \
    uint32_t _d = (uint32_t)(dst); \
    uint32_t _res = (uint32_t)(-(int32_t)_d); \
    g_m68k.flag_c = g_m68k.flag_x = (_d != 0); \
    g_m68k.flag_v = (_d == 0x80000000u); \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
    (dst) = _res; \
} while(0)

/* --- NOT: dst = ~dst, update NZ, clear CV --- */
#define M68K_NOT8(dst) do { \
    uint8_t _res = ~(uint8_t)(dst); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80) != 0; \
    (dst) = ((dst) & 0xFFFFFF00u) | _res; \
} while(0)

#define M68K_NOT16(dst) do { \
    uint16_t _res = ~(uint16_t)(dst); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x8000) != 0; \
    (dst) = ((dst) & 0xFFFF0000u) | _res; \
} while(0)

#define M68K_NOT32(dst) do { \
    uint32_t _res = ~(uint32_t)(dst); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
    (dst) = _res; \
} while(0)

/* --- MULU: unsigned 16x16 -> 32, update NZ, clear CV --- */
#define M68K_MULU(dst, src) do { \
    uint32_t _res = (uint32_t)(uint16_t)(dst) * (uint32_t)(uint16_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
    (dst) = _res; \
} while(0)

/* --- MULS: signed 16x16 -> 32, update NZ, clear CV --- */
#define M68K_MULS(dst, src) do { \
    int32_t _res = (int32_t)(int16_t)(dst) * (int32_t)(int16_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_res == 0); \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
    (dst) = (uint32_t)_res; \
} while(0)

/* --- DIVU: unsigned 32/16 -> 16q:16r, update NZV, clear C --- */
#define M68K_DIVU(dst, src) do { \
    uint16_t _divisor = (uint16_t)(src); \
    if (_divisor != 0) { \
        uint32_t _dividend = (uint32_t)(dst); \
        uint32_t _quotient = _dividend / _divisor; \
        if (_quotient > 0xFFFF) { \
            g_m68k.flag_v = true; \
            g_m68k.flag_c = false; \
        } else { \
            uint16_t _remainder = (uint16_t)(_dividend % _divisor); \
            (dst) = ((uint32_t)_remainder << 16) | (uint16_t)_quotient; \
            g_m68k.flag_v = false; \
            g_m68k.flag_c = false; \
            g_m68k.flag_z = ((uint16_t)_quotient == 0); \
            g_m68k.flag_n = ((uint16_t)_quotient & 0x8000) != 0; \
        } \
    } \
    /* Division by zero triggers a trap — handled by the runtime */ \
} while(0)

/* --- DIVS: signed 32/16 -> 16q:16r --- */
#define M68K_DIVS(dst, src) do { \
    int16_t _divisor = (int16_t)(src); \
    if (_divisor != 0) { \
        int32_t _dividend = (int32_t)(dst); \
        int32_t _quotient = _dividend / _divisor; \
        if (_quotient < -32768 || _quotient > 32767) { \
            g_m68k.flag_v = true; \
            g_m68k.flag_c = false; \
        } else { \
            int16_t _remainder = (int16_t)(_dividend % _divisor); \
            (dst) = ((uint32_t)(uint16_t)_remainder << 16) | (uint16_t)(int16_t)_quotient; \
            g_m68k.flag_v = false; \
            g_m68k.flag_c = false; \
            g_m68k.flag_z = ((int16_t)_quotient == 0); \
            g_m68k.flag_n = ((int16_t)_quotient < 0); \
        } \
    } \
} while(0)

/* --- Shift/Rotate operations --- */

/* LSL: logical shift left */
#define M68K_LSL8(dst, count) do { \
    uint8_t _cnt = (uint8_t)(count) & 63; \
    uint8_t _d = (uint8_t)(dst); \
    if (_cnt > 0) { \
        if (_cnt <= 8) { \
            g_m68k.flag_c = g_m68k.flag_x = ((_d >> (8 - _cnt)) & 1) != 0; \
            _d <<= _cnt; \
        } else { \
            g_m68k.flag_c = g_m68k.flag_x = false; \
            _d = 0; \
        } \
    } else { \
        g_m68k.flag_c = false; \
    } \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_d == 0); \
    g_m68k.flag_n = (_d & 0x80) != 0; \
    (dst) = ((dst) & 0xFFFFFF00u) | _d; \
} while(0)

#define M68K_LSL16(dst, count) do { \
    uint8_t _cnt = (uint8_t)(count) & 63; \
    uint16_t _d = (uint16_t)(dst); \
    if (_cnt > 0) { \
        if (_cnt <= 16) { \
            g_m68k.flag_c = g_m68k.flag_x = ((_d >> (16 - _cnt)) & 1) != 0; \
            _d <<= _cnt; \
        } else { \
            g_m68k.flag_c = g_m68k.flag_x = false; \
            _d = 0; \
        } \
    } else { \
        g_m68k.flag_c = false; \
    } \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_d == 0); \
    g_m68k.flag_n = (_d & 0x8000) != 0; \
    (dst) = ((dst) & 0xFFFF0000u) | _d; \
} while(0)

#define M68K_LSL32(dst, count) do { \
    uint8_t _cnt = (uint8_t)(count) & 63; \
    uint32_t _d = (uint32_t)(dst); \
    if (_cnt > 0) { \
        if (_cnt <= 32) { \
            g_m68k.flag_c = g_m68k.flag_x = ((_d >> (32 - _cnt)) & 1) != 0; \
            _d = (_cnt < 32) ? (_d << _cnt) : 0; \
        } else { \
            g_m68k.flag_c = g_m68k.flag_x = false; \
            _d = 0; \
        } \
    } else { \
        g_m68k.flag_c = false; \
    } \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_d == 0); \
    g_m68k.flag_n = (_d & 0x80000000u) != 0; \
    (dst) = _d; \
} while(0)

/* LSR: logical shift right */
#define M68K_LSR16(dst, count) do { \
    uint8_t _cnt = (uint8_t)(count) & 63; \
    uint16_t _d = (uint16_t)(dst); \
    if (_cnt > 0) { \
        if (_cnt <= 16) { \
            g_m68k.flag_c = g_m68k.flag_x = ((_d >> (_cnt - 1)) & 1) != 0; \
            _d >>= _cnt; \
        } else { \
            g_m68k.flag_c = g_m68k.flag_x = false; \
            _d = 0; \
        } \
    } else { \
        g_m68k.flag_c = false; \
    } \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_d == 0); \
    g_m68k.flag_n = false; \
    (dst) = ((dst) & 0xFFFF0000u) | _d; \
} while(0)

/* ASR: arithmetic shift right (preserves sign) */
#define M68K_ASR16(dst, count) do { \
    uint8_t _cnt = (uint8_t)(count) & 63; \
    int16_t _d = (int16_t)(dst); \
    if (_cnt > 0) { \
        if (_cnt <= 16) { \
            g_m68k.flag_c = g_m68k.flag_x = ((_d >> (_cnt - 1)) & 1) != 0; \
            _d >>= _cnt; \
        } else { \
            g_m68k.flag_c = g_m68k.flag_x = (_d < 0); \
            _d = (_d < 0) ? -1 : 0; \
        } \
    } else { \
        g_m68k.flag_c = false; \
    } \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = ((uint16_t)_d == 0); \
    g_m68k.flag_n = (_d < 0); \
    (dst) = ((dst) & 0xFFFF0000u) | (uint16_t)_d; \
} while(0)

/* ROL: rotate left */
#define M68K_ROL16(dst, count) do { \
    uint8_t _cnt = (uint8_t)(count) & 63; \
    uint16_t _d = (uint16_t)(dst); \
    if (_cnt > 0) { \
        _cnt %= 16; \
        _d = (_d << _cnt) | (_d >> (16 - _cnt)); \
        g_m68k.flag_c = (_d & 1) != 0; \
    } else { \
        g_m68k.flag_c = false; \
    } \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_d == 0); \
    g_m68k.flag_n = (_d & 0x8000) != 0; \
    (dst) = ((dst) & 0xFFFF0000u) | _d; \
} while(0)

/* --- SWAP: swap upper and lower words of data register --- */
#define M68K_SWAP(dst) do { \
    uint32_t _d = (uint32_t)(dst); \
    _d = (_d >> 16) | (_d << 16); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_d == 0); \
    g_m68k.flag_n = (_d & 0x80000000u) != 0; \
    (dst) = _d; \
} while(0)

/* --- EXT: sign-extend byte->word or word->long --- */
#define M68K_EXT16(dst) do { \
    int16_t _res = (int8_t)(dst); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = ((uint16_t)_res == 0); \
    g_m68k.flag_n = (_res < 0); \
    (dst) = ((dst) & 0xFFFF0000u) | (uint16_t)_res; \
} while(0)

#define M68K_EXT32(dst) do { \
    int32_t _res = (int16_t)(dst); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = ((uint32_t)_res == 0); \
    g_m68k.flag_n = (_res < 0); \
    (dst) = (uint32_t)_res; \
} while(0)

/* --- CLR: clear destination, set Z, clear NVC --- */
#define M68K_CLR8(dst) do { \
    (dst) = (dst) & 0xFFFFFF00u; \
    g_m68k.flag_c = false; g_m68k.flag_v = false; \
    g_m68k.flag_z = true; g_m68k.flag_n = false; \
} while(0)

#define M68K_CLR16(dst) do { \
    (dst) = (dst) & 0xFFFF0000u; \
    g_m68k.flag_c = false; g_m68k.flag_v = false; \
    g_m68k.flag_z = true; g_m68k.flag_n = false; \
} while(0)

#define M68K_CLR32(dst) do { \
    (dst) = 0; \
    g_m68k.flag_c = false; g_m68k.flag_v = false; \
    g_m68k.flag_z = true; g_m68k.flag_n = false; \
} while(0)

/* --- BTST: test a bit, set Z if bit is zero --- */
#define M68K_BTST(val, bit) do { \
    g_m68k.flag_z = (((uint32_t)(val) >> ((bit) & 31)) & 1) == 0; \
} while(0)

/* --- BSET/BCLR/BCHG: test and modify a bit --- */
#define M68K_BSET(dst, bit) do { \
    uint32_t _bit = (bit) & 31; \
    g_m68k.flag_z = (((uint32_t)(dst) >> _bit) & 1) == 0; \
    (dst) |= (1u << _bit); \
} while(0)

#define M68K_BCLR(dst, bit) do { \
    uint32_t _bit = (bit) & 31; \
    g_m68k.flag_z = (((uint32_t)(dst) >> _bit) & 1) == 0; \
    (dst) &= ~(1u << _bit); \
} while(0)

#define M68K_BCHG(dst, bit) do { \
    uint32_t _bit = (bit) & 31; \
    g_m68k.flag_z = (((uint32_t)(dst) >> _bit) & 1) == 0; \
    (dst) ^= (1u << _bit); \
} while(0)

/* --- ADDX/SUBX: add/subtract with extend (for multi-precision) --- */
#define M68K_ADDX32(dst, src) do { \
    uint32_t _s = (uint32_t)(src); \
    uint32_t _d = (uint32_t)(dst); \
    uint64_t _r = (uint64_t)_d + (uint64_t)_s + (g_m68k.flag_x ? 1u : 0u); \
    uint32_t _res = (uint32_t)_r; \
    g_m68k.flag_c = g_m68k.flag_x = (_r > 0xFFFFFFFFu); \
    g_m68k.flag_v = ((_d ^ _res) & (_s ^ _res) & 0x80000000u) != 0; \
    if (_res != 0) g_m68k.flag_z = false; /* Z is only cleared, never set */ \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
    (dst) = _res; \
} while(0)

#define M68K_SUBX32(dst, src) do { \
    uint32_t _s = (uint32_t)(src); \
    uint32_t _d = (uint32_t)(dst); \
    uint64_t _r = (uint64_t)_d - (uint64_t)_s - (g_m68k.flag_x ? 1u : 0u); \
    uint32_t _res = (uint32_t)_r; \
    g_m68k.flag_c = g_m68k.flag_x = (_r > 0xFFFFFFFFu); \
    g_m68k.flag_v = ((_d ^ _s) & (_d ^ _res) & 0x80000000u) != 0; \
    if (_res != 0) g_m68k.flag_z = false; \
    g_m68k.flag_n = (_res & 0x80000000u) != 0; \
    (dst) = _res; \
} while(0)

/* --- MOVE: dst = src, update NZ, clear CV (for data moves only) --- */
#define M68K_MOVE8(dst, src) do { \
    uint8_t _v = (uint8_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_v == 0); \
    g_m68k.flag_n = (_v & 0x80) != 0; \
    (dst) = ((dst) & 0xFFFFFF00u) | _v; \
} while(0)

#define M68K_MOVE16(dst, src) do { \
    uint16_t _v = (uint16_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_v == 0); \
    g_m68k.flag_n = (_v & 0x8000) != 0; \
    (dst) = ((dst) & 0xFFFF0000u) | _v; \
} while(0)

#define M68K_MOVE32(dst, src) do { \
    uint32_t _v = (uint32_t)(src); \
    g_m68k.flag_c = false; \
    g_m68k.flag_v = false; \
    g_m68k.flag_z = (_v == 0); \
    g_m68k.flag_n = (_v & 0x80000000u) != 0; \
    (dst) = _v; \
} while(0)

/* MOVEA: move to address register — NO flag updates */
#define M68K_MOVEA16(dst, src) do { \
    (dst) = (uint32_t)(int32_t)(int16_t)(src); \
} while(0)

#define M68K_MOVEA32(dst, src) do { \
    (dst) = (uint32_t)(src); \
} while(0)

/* --- Condition Code Tests (for Bcc, Scc, DBcc) --- */
/*
 * Each macro evaluates to a bool. Use in recompiled branch code:
 *   if (M68K_CC_EQ) func_table_call(target);
 */

#define M68K_CC_T    (true)                                          /* Always true */
#define M68K_CC_F    (false)                                         /* Always false */
#define M68K_CC_HI   (!g_m68k.flag_c && !g_m68k.flag_z)             /* Higher (unsigned) */
#define M68K_CC_LS   (g_m68k.flag_c || g_m68k.flag_z)               /* Lower or Same */
#define M68K_CC_CC   (!g_m68k.flag_c)                                /* Carry Clear */
#define M68K_CC_CS   (g_m68k.flag_c)                                 /* Carry Set */
#define M68K_CC_NE   (!g_m68k.flag_z)                                /* Not Equal */
#define M68K_CC_EQ   (g_m68k.flag_z)                                 /* Equal */
#define M68K_CC_VC   (!g_m68k.flag_v)                                /* Overflow Clear */
#define M68K_CC_VS   (g_m68k.flag_v)                                 /* Overflow Set */
#define M68K_CC_PL   (!g_m68k.flag_n)                                /* Plus (positive) */
#define M68K_CC_MI   (g_m68k.flag_n)                                 /* Minus (negative) */
#define M68K_CC_GE   (g_m68k.flag_n == g_m68k.flag_v)               /* Greater or Equal (signed) */
#define M68K_CC_LT   (g_m68k.flag_n != g_m68k.flag_v)               /* Less Than (signed) */
#define M68K_CC_GT   (!g_m68k.flag_z && (g_m68k.flag_n == g_m68k.flag_v)) /* Greater Than (signed) */
#define M68K_CC_LE   (g_m68k.flag_z || (g_m68k.flag_n != g_m68k.flag_v))  /* Less or Equal (signed) */

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_M68K_H */
