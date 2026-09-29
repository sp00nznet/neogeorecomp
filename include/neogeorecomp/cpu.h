/*
 * cpu.h — 68000 CPU state and the flag-correct operation helpers that
 * recompiled code is lowered to.
 *
 * One register file (ng_cpu) is shared by recompiled code and by the
 * Musashi interpreter used as fallback; exec.c copies it in and out of
 * Musashi at each hand-over. Condition codes live as separate 0/1 bytes
 * so the C compiler can drop dead flag writes inside a routine.
 *
 * The helpers reproduce Musashi's results exactly, including the
 * undocumented flag behaviour of ABCD/SBCD/NBCD, DIVx overflow and CHK,
 * because Musashi is the oracle the conformance harness compares against
 * (docs/recompiler.md, "Conformance").
 */
#ifndef NEOGEORECOMP_CPU_H
#define NEOGEORECOMP_CPU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t d[8];
    uint32_t a[8];      /* a[7] is the active stack pointer */
    uint32_t osp;       /* inactive stack pointer: USP while S=1, SSP while S=0 */
    uint32_t pc;        /* where execution resumes when control reaches the dispatcher */
    uint8_t  x, n, z, v, c;
    uint8_t  s, t, ipl;
    uint8_t  stopped;   /* STOP executed; waits for an interrupt */
} ng_cpu_t;

extern ng_cpu_t ng_cpu;
extern int64_t  ng_cycles;      /* 68k clocks since power-on */
extern int64_t  ng_next_event;  /* recompiled code yields once ng_cycles reaches this */
extern uint64_t ng_icount;      /* instructions executed natively (for --verify) */

/* Opens every recompiled instruction: its base cycle cost, and a count
 * the verifier uses to replay the same number of instructions. */
#define NG_INSN(cyc) (ng_cycles += (cyc), ng_icount++)

#define CPU ng_cpu

void ng_set_sr(uint16_t sr);    /* swaps stacks on an S change */

static inline uint16_t ng_get_sr(void) {
    return (uint16_t)((CPU.t << 15) | (CPU.s << 13) | (CPU.ipl << 8) |
                      (CPU.x << 4) | (CPU.n << 3) | (CPU.z << 2) | (CPU.v << 1) | CPU.c);
}
static inline uint8_t ng_get_ccr(void) { return (uint8_t)(ng_get_sr() & 0x1F); }
static inline void ng_set_ccr(uint16_t v) {
    CPU.x = (v >> 4) & 1; CPU.n = (v >> 3) & 1; CPU.z = (v >> 2) & 1;
    CPU.v = (v >> 1) & 1; CPU.c = v & 1;
}

/* MOVE USP: the user stack pointer is a[7] only in user mode. */
static inline uint32_t ng_get_usp(void) { return CPU.s ? CPU.osp : CPU.a[7]; }
static inline void ng_set_usp(uint32_t v) { if (CPU.s) CPU.osp = v; else CPU.a[7] = v; }

/* Raise a 68000 exception: push the group-2 frame, enter supervisor mode,
 * load the vector. `ret_pc` is the PC pushed on the frame. Execution
 * continues at CPU.pc once the caller returns to the dispatcher. */
void ng_exception(int vector, uint32_t ret_pc);

/* ---- sign/extension helpers ---- */
#define SX8(v)  ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v) ((uint32_t)(int32_t)(int16_t)(v))

/* ---- condition tests ---- */
#define CC_T  1
#define CC_F  0
#define CC_HI (!CPU.c && !CPU.z)
#define CC_LS (CPU.c || CPU.z)
#define CC_CC (!CPU.c)
#define CC_CS (CPU.c)
#define CC_NE (!CPU.z)
#define CC_EQ (CPU.z)
#define CC_VC (!CPU.v)
#define CC_VS (CPU.v)
#define CC_PL (!CPU.n)
#define CC_MI (CPU.n)
#define CC_GE (CPU.n == CPU.v)
#define CC_LT (CPU.n != CPU.v)
#define CC_GT (!CPU.z && CPU.n == CPU.v)
#define CC_LE (CPU.z || CPU.n != CPU.v)

/* ---- size-generic ALU, instantiated for 8/16/32 ---- */
#define NG_ALU(B, T, ST)                                                         \
static inline T ng_nz##B(T r) {                                                  \
    CPU.n = (uint8_t)((r >> (B - 1)) & 1); CPU.z = (r == 0); return r; }         \
static inline T ng_logic##B(T r) {                                               \
    CPU.v = 0; CPU.c = 0; return ng_nz##B(r); }                                  \
static inline T ng_add##B(T s, T d) {                                            \
    T r = (T)(d + s);                                                            \
    CPU.x = CPU.c = (r < d);                                                     \
    CPU.v = (uint8_t)((((s ^ r) & (d ^ r)) >> (B - 1)) & 1);                     \
    return ng_nz##B(r); }                                                        \
static inline T ng_sub##B(T s, T d) {                                            \
    T r = (T)(d - s);                                                            \
    CPU.x = CPU.c = (s > d);                                                     \
    CPU.v = (uint8_t)((((s ^ d) & (r ^ d)) >> (B - 1)) & 1);                     \
    return ng_nz##B(r); }                                                        \
static inline void ng_cmp##B(T s, T d) {                                         \
    T r = (T)(d - s);                                                            \
    CPU.c = (s > d);                                                             \
    CPU.v = (uint8_t)((((s ^ d) & (r ^ d)) >> (B - 1)) & 1);                     \
    ng_nz##B(r); }                                                               \
static inline T ng_addx##B(T s, T d) {                                           \
    uint64_t full = (uint64_t)d + s + CPU.x;                                     \
    T r = (T)full;                                                               \
    CPU.x = CPU.c = (uint8_t)((full >> B) & 1);                                  \
    CPU.v = (uint8_t)((((s ^ r) & (d ^ r)) >> (B - 1)) & 1);                     \
    CPU.n = (uint8_t)((r >> (B - 1)) & 1); if (r) CPU.z = 0;                     \
    return r; }                                                                  \
static inline T ng_subx##B(T s, T d) {                                           \
    T r = (T)(d - s - CPU.x);                                                    \
    CPU.x = CPU.c = ((uint64_t)s + CPU.x > (uint64_t)d);                         \
    CPU.v = (uint8_t)((((s ^ d) & (r ^ d)) >> (B - 1)) & 1);                     \
    CPU.n = (uint8_t)((r >> (B - 1)) & 1); if (r) CPU.z = 0;                     \
    return r; }                                                                  \
static inline T ng_neg##B(T d) { return ng_sub##B(d, 0); }                       \
static inline T ng_negx##B(T d) { return ng_subx##B(d, 0); }                     \
static inline T ng_lsl##B(T d, unsigned n) {                                     \
    n &= 63; T r;                                                                \
    if (n == 0) { CPU.c = 0; r = d; }                                            \
    else if (n <= B) { uint64_t w = (uint64_t)d << n; r = (T)w;                  \
        CPU.x = CPU.c = (uint8_t)((w >> B) & 1); }                               \
    else { r = 0; CPU.x = CPU.c = 0; }                                           \
    CPU.v = 0; return ng_nz##B(r); }                                             \
static inline T ng_lsr##B(T d, unsigned n) {                                     \
    n &= 63; T r;                                                                \
    if (n == 0) { CPU.c = 0; r = d; }                                            \
    else if (n <= B) { r = (T)((uint64_t)d >> n);                                \
        CPU.x = CPU.c = (uint8_t)(((uint64_t)d >> (n - 1)) & 1); }               \
    else { r = 0; CPU.x = CPU.c = 0; }                                           \
    CPU.v = 0; return ng_nz##B(r); }                                             \
static inline T ng_asr##B(T d, unsigned n) {                                     \
    n &= 63; T r; int64_t sd = (ST)d;                                            \
    if (n == 0) { CPU.c = 0; r = d; }                                            \
    else if (n < B) { r = (T)(sd >> n);                                          \
        CPU.x = CPU.c = (uint8_t)((sd >> (n - 1)) & 1); }                        \
    else { r = (T)(sd < 0 ? -1 : 0); CPU.x = CPU.c = (uint8_t)(sd < 0); }        \
    CPU.v = 0; return ng_nz##B(r); }                                             \
static inline T ng_asl##B(T d, unsigned n) {                                     \
    n &= 63; T r;                                                                \
    if (n == 0) { CPU.c = 0; CPU.v = 0; r = d; }                                 \
    else if (n < B) {                                                            \
        uint64_t w = (uint64_t)d << n; r = (T)w;                                 \
        CPU.x = CPU.c = (uint8_t)((w >> B) & 1);                                 \
        /* V: the top n+1 bits of the source were not all equal */              \
        T m = (T)~(T)(((uint64_t)1 << (B - n - 1)) - 1);                         \
        T top = d & m; CPU.v = (top != 0 && top != m); }                         \
    else { r = 0; CPU.x = CPU.c = (n == B) ? (uint8_t)(d & 1) : 0;               \
        CPU.v = (d != 0); }                                                      \
    return ng_nz##B(r); }                                                        \
static inline T ng_rol##B(T d, unsigned n) {                                     \
    n &= 63; T r = d;                                                            \
    if (n) { unsigned k = n % B; if (k) r = (T)((d << k) | (d >> (B - k)));      \
        CPU.c = (uint8_t)(r & 1); } else CPU.c = 0;                              \
    CPU.v = 0; return ng_nz##B(r); }                                             \
static inline T ng_ror##B(T d, unsigned n) {                                     \
    n &= 63; T r = d;                                                            \
    if (n) { unsigned k = n % B; if (k) r = (T)((d >> k) | (d << (B - k)));      \
        CPU.c = (uint8_t)((r >> (B - 1)) & 1); } else CPU.c = 0;                 \
    CPU.v = 0; return ng_nz##B(r); }                                             \
static inline T ng_roxl##B(T d, unsigned n) {                                    \
    n &= 63; T r = d;                                                            \
    if (n) { unsigned k = n % (B + 1);                                           \
        if (k) { uint64_t w = ((uint64_t)CPU.x << B) | d;                        \
            w = ((w << k) | (w >> (B + 1 - k))) & ((((uint64_t)1) << (B + 1)) - 1); \
            r = (T)w; CPU.x = (uint8_t)((w >> B) & 1); } }                       \
    CPU.c = CPU.x; CPU.v = 0; return ng_nz##B(r); }                              \
static inline T ng_roxr##B(T d, unsigned n) {                                    \
    n &= 63; T r = d;                                                            \
    if (n) { unsigned k = n % (B + 1);                                           \
        if (k) { uint64_t w = ((uint64_t)CPU.x << B) | d;                        \
            w = ((w >> k) | (w << (B + 1 - k))) & ((((uint64_t)1) << (B + 1)) - 1); \
            r = (T)w; CPU.x = (uint8_t)((w >> B) & 1); } }                       \
    CPU.c = CPU.x; CPU.v = 0; return ng_nz##B(r); }

NG_ALU(8, uint8_t, int8_t)
NG_ALU(16, uint16_t, int16_t)
NG_ALU(32, uint32_t, int32_t)
#undef NG_ALU

/* ---- multiply / divide (word forms, the only ones on a 68000) ---- */
static inline uint32_t ng_mulu(uint16_t s, uint16_t d) {
    return ng_logic32((uint32_t)s * d);
}
static inline uint32_t ng_muls(uint16_t s, uint16_t d) {
    return ng_logic32((uint32_t)((int32_t)(int16_t)s * (int32_t)(int16_t)d));
}
/* DIVU/DIVS return false on divide-by-zero; the caller raises vector 5. */
static inline bool ng_divu(uint16_t s, uint32_t *dreg) {
    if (s == 0) return false;
    uint32_t q = *dreg / s, rem = *dreg % s;
    if (q < 0x10000) {
        CPU.z = (q == 0); CPU.n = (uint8_t)((q >> 15) & 1); CPU.v = 0; CPU.c = 0;
        *dreg = (q & 0xFFFF) | (rem << 16);
    } else {
        CPU.v = 1;
    }
    return true;
}
static inline bool ng_divs(uint16_t s16, uint32_t *dreg) {
    int32_t s = (int16_t)s16;
    if (s == 0) return false;
    if (*dreg == 0x80000000u && s == -1) {
        CPU.z = 1; CPU.n = 0; CPU.v = 0; CPU.c = 0; *dreg = 0; return true;
    }
    int32_t q = (int32_t)*dreg / s, rem = (int32_t)*dreg % s;
    if (q == (int16_t)q) {
        CPU.z = (q == 0); CPU.n = (uint8_t)((q >> 15) & 1); CPU.v = 0; CPU.c = 0;
        *dreg = ((uint32_t)q & 0xFFFF) | ((uint32_t)rem << 16);
    } else {
        CPU.v = 1;
    }
    return true;
}

/* ---- BCD, matching Musashi's undocumented N/V results ---- */
static inline uint8_t ng_abcd(uint8_t s, uint8_t d) {
    uint32_t res = (s & 0x0F) + (d & 0x0F) + CPU.x;
    uint32_t v = ~res;
    if (res > 9) res += 6;
    res += (s & 0xF0) + (d & 0xF0);
    CPU.x = CPU.c = (res > 0x99);
    if (CPU.c) res -= 0xA0;
    CPU.v = (uint8_t)(((v & res) >> 7) & 1);
    CPU.n = (uint8_t)((res >> 7) & 1);
    res &= 0xFF;
    if (res) CPU.z = 0;
    return (uint8_t)res;
}
static inline uint8_t ng_sbcd(uint8_t s, uint8_t d) {
    uint32_t res = (d & 0x0F) - (s & 0x0F) - CPU.x;
    uint32_t v = ~res;
    if (res > 9) res -= 6;
    res += (d & 0xF0) - (s & 0xF0);
    CPU.x = CPU.c = (res > 0x99);
    if (CPU.c) res += 0xA0;
    res &= 0xFF;
    CPU.v = (uint8_t)(((v & res) >> 7) & 1);
    CPU.n = (uint8_t)((res >> 7) & 1);
    if (res) CPU.z = 0;
    return (uint8_t)res;
}
static inline uint8_t ng_nbcd(uint8_t d) {
    uint32_t res = (0x9A - d - CPU.x) & 0xFF;
    if (res != 0x9A) {
        uint32_t v = ~res;
        if ((res & 0x0F) == 0x0A) res = (res & 0xF0) + 0x10;
        res &= 0xFF;
        CPU.v = (uint8_t)(((v & res) >> 7) & 1);
        if (res) CPU.z = 0;
        CPU.c = CPU.x = 1;
    } else {
        CPU.v = 0; CPU.c = 0; CPU.x = 0;
    }
    CPU.n = (uint8_t)((res >> 7) & 1);
    return (uint8_t)res;
}

/* CHK: returns true when the bound check fails (caller raises vector 6). */
static inline bool ng_chk(uint16_t bound16, uint16_t v16) {
    int32_t v = (int16_t)v16, bound = (int16_t)bound16;
    CPU.z = (v16 == 0); CPU.v = 0; CPU.c = 0;
    if (v >= 0 && v <= bound) return false;
    CPU.n = (v < 0);
    return true;
}

#ifdef __cplusplus
}
#endif
#endif
