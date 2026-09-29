/*
 * exec.c — the dispatcher that runs the 68000 program.
 *
 * Control flow between recompiled routines is flat: a routine runs until
 * control leaves it (JSR, RTS, JMP, an exception, or a loop back-edge
 * once ng_cycles reaches ng_next_event), stores the next address in
 * CPU.pc and returns here. The dispatcher then takes any pending
 * interrupt the mask allows and continues at CPU.pc: natively when the
 * address has a recompiled entry, otherwise in Musashi.
 *
 * The flat model is what makes the recompiled code exact rather than
 * "usually right": return addresses live on the emulated stack, so code
 * that pops or rewrites them behaves as on hardware, and interrupts are
 * taken as real 68000 exceptions. docs/recompiler.md has the full story.
 *
 * Musashi handles interrupts itself while it runs (it checks them when
 * the SR changes), so interpreted code sees them at the exact instruction.
 */
#include "ng_internal.h"
#include <neogeorecomp/bus.h>
#include "m68k.h"
#include "musashi_glue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ng_cpu_t ng_cpu;
int64_t  ng_cycles;
int64_t  ng_next_event;
uint8_t  ng_irq_pending;

uint64_t exec_native_blocks, exec_interp_instrs;

/* Entry tables for the three regions code runs from, indexed by addr/2. */
static ng_func_t *s_tab_fixed;     /* $000000-$0FFFFF */
static ng_func_t *s_tab_bank;      /* $200000-$2FFFFF */
static ng_func_t *s_tab_bios;      /* $C00000-$C1FFFF */
static int s_interp_only;
static int s_in_musashi;

/* Addresses the interpreter ran that have no recompiled entry: fed back to
 * the recompiler as extra entry points (--dump-misses). */
#define MISS_MAX 65536
static uint32_t s_miss[MISS_MAX];
static size_t s_nmiss;
static uint8_t *s_miss_seen;       /* bitmap over the same regions */

static ng_func_t *slot(uint32_t a) {
    a &= 0xFFFFFE;
    if (a < 0x100000) return &s_tab_fixed[a >> 1];
    if (a >= 0x200000 && a < 0x300000) return &s_tab_bank[(a - 0x200000) >> 1];
    if (a >= 0xC00000 && a < 0xD00000) return &s_tab_bios[(a & 0x1FFFF) >> 1];
    return NULL;
}

static inline ng_func_t lookup(uint32_t a) {
    ng_func_t *s = slot(a);
    return s ? *s : NULL;
}

void exec_init(const ng_func_entry_t *funcs, size_t nfuncs, int interp_only) {
    s_tab_fixed = (ng_func_t *)calloc(0x80000, sizeof(ng_func_t));
    s_tab_bank = (ng_func_t *)calloc(0x80000, sizeof(ng_func_t));
    s_tab_bios = (ng_func_t *)calloc(0x10000, sizeof(ng_func_t));
    s_miss_seen = (uint8_t *)calloc(0x120000 / 16, 1);
    s_interp_only = interp_only;
    for (size_t i = 0; i < nfuncs; i++) {
        ng_func_t *s = slot(funcs[i].addr);
        if (s) *s = funcs[i].fn;
    }
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
}

/* ---- status register and exceptions ---- */

void ng_set_sr(uint16_t sr) {
    uint8_t s = (sr >> 13) & 1;
    if (s != CPU.s) {
        uint32_t t = CPU.a[7]; CPU.a[7] = CPU.osp; CPU.osp = t;
        CPU.s = s;
    }
    CPU.t = (sr >> 15) & 1;
    CPU.ipl = (sr >> 8) & 7;
    ng_set_ccr(sr);
}

void ng_exception(int vector, uint32_t ret_pc) {
    uint16_t old = ng_get_sr();
    ng_set_sr((uint16_t)((old | 0x2000) & ~0x8000));
    ng_push32(ret_pc);
    ng_push16(old);
    CPU.pc = ng_r32((uint32_t)vector * 4);
    ng_cycles += 34;
}

static void take_irq(int level) {
    uint16_t old = ng_get_sr();
    CPU.stopped = 0;
    ng_set_sr((uint16_t)(((old | 0x2000) & ~0x8700) | (level << 8)));
    ng_push32(CPU.pc);
    ng_push16(old);
    CPU.pc = ng_r32(0x60 + (uint32_t)level * 4);
    ng_cycles += 44;
}

/* Called by io/lspc when an IRQ line changes while Musashi is running. */
void exec_irq_changed(void) {
    if (s_in_musashi) m68k_set_irq((unsigned)ng_irq_level());
}

/* Called when an event is scheduled earlier than the current slice end. */
void exec_event_changed(int64_t at) {
    if (at < ng_next_event) {
        ng_next_event = at;
        if (s_in_musashi) m68k_end_timeslice();
    }
}

/* ---- Musashi hand-over ---- */

static void to_musashi(void) {
    m68k_set_reg(M68K_REG_SR, ng_get_sr());
    for (int i = 0; i < 8; i++) m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), CPU.d[i]);
    for (int i = 0; i < 7; i++) m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), CPU.a[i]);
    m68k_set_reg(M68K_REG_A7, CPU.a[7]);
    m68k_set_reg(CPU.s ? M68K_REG_USP : M68K_REG_ISP, CPU.osp);
    m68k_set_reg(M68K_REG_PC, CPU.pc);
    if (CPU.stopped) mus_set_stopped(); else mus_clear_stopped();
    s_in_musashi = 1;
    m68k_set_irq((unsigned)ng_irq_level());
}

static void from_musashi(void) {
    s_in_musashi = 0;
    uint16_t sr = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);
    CPU.s = (sr >> 13) & 1; CPU.t = (sr >> 15) & 1; CPU.ipl = (sr >> 8) & 7;
    ng_set_ccr(sr);
    for (int i = 0; i < 8; i++) CPU.d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
    for (int i = 0; i < 8; i++) CPU.a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
    CPU.osp = m68k_get_reg(NULL, CPU.s ? M68K_REG_USP : M68K_REG_ISP);
    CPU.pc = m68k_get_reg(NULL, M68K_REG_PC);
    CPU.stopped = (uint8_t)mus_stopped();
}

static void note_miss(uint32_t a) {
    ng_func_t *s = slot(a);
    if (!s) return;
    size_t bit = (size_t)(s - s_tab_fixed);
    if (s >= s_tab_bank && s < s_tab_bank + 0x80000) bit = 0x80000 + (size_t)(s - s_tab_bank);
    if (s >= s_tab_bios && s < s_tab_bios + 0x10000) bit = 0x100000 + (size_t)(s - s_tab_bios);
    if (s_miss_seen[bit >> 3] & (1 << (bit & 7))) return;
    s_miss_seen[bit >> 3] |= (uint8_t)(1 << (bit & 7));
    if (s_nmiss < MISS_MAX) s_miss[s_nmiss++] = a & 0xFFFFFE;
}

static void interpret(void) {
    to_musashi();
    if (s_interp_only) {
        while (ng_cycles < ng_next_event && !mus_stopped()) {
            int64_t budget = ng_next_event - ng_cycles;
            if (budget > 100000) budget = 100000;
            ng_cycles += m68k_execute((int)budget);
        }
    } else {
        /* Step until control reaches code that has a native entry. */
        note_miss(CPU.pc);
        while (ng_cycles < ng_next_event && !mus_stopped()) {
            ng_cycles += m68k_execute(1);
            exec_interp_instrs++;
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
            if (lookup(pc)) break;
        }
    }
    from_musashi();
}

void exec_run(int64_t until) {
    ng_next_event = until;
    while (ng_cycles < ng_next_event) {
        int level = ng_irq_level();
        if (level > CPU.ipl || level == 7) take_irq(level);
        if (CPU.stopped) { ng_cycles = ng_next_event; break; }
        ng_func_t fn = s_interp_only ? NULL : lookup(CPU.pc);
        if (fn) { fn(); exec_native_blocks++; }
        else interpret();
    }
}

void exec_reset(void) {
    memset(&ng_cpu, 0, sizeof(ng_cpu));
    ng_cpu.s = 1;
    ng_cpu.ipl = 7;
    ng_cpu.a[7] = ng_r32(0);
    ng_cpu.pc = ng_r32(4);
}

void exec_report(void) {
    uint64_t total = exec_native_blocks + exec_interp_instrs;
    fprintf(stderr, "[exec] native blocks %llu, interpreted instructions %llu, %zu uncovered entry points\n",
            (unsigned long long)exec_native_blocks, (unsigned long long)exec_interp_instrs, s_nmiss);
    (void)total;
}

int exec_dump_misses(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (size_t i = 0; i < s_nmiss; i++) fprintf(f, "%06X\n", s_miss[i]);
    fclose(f);
    return 0;
}

/* ---- Musashi memory callbacks ---- */

unsigned int m68k_read_memory_8(unsigned int a) { return ng_r8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return ng_r16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return ng_r32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { ng_w8(a, (uint8_t)v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { ng_w16(a, (uint16_t)v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { ng_w32(a, v); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return ng_r16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return ng_r32(a); }
