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

uint8_t  ng_irq_pending;

uint64_t exec_native_blocks, exec_interp_instrs;

/* Entry tables for the three regions code runs from, indexed by addr/2. */
static ng_func_t *s_tab_fixed;     /* $000000-$0FFFFF */
static ng_func_t *s_tab_bank;      /* $200000-$2FFFFF */
static ng_func_t *s_tab_bios;      /* $C00000-$C1FFFF */
static int s_interp_only;
static int s_in_musashi;

/* --verify: see run_verified(). */
static int s_verify;
static uint64_t s_verify_blocks, s_verify_fails;
static int s_replay, s_rp_bad;
static const ng_bus_event_t *s_rp;
static size_t s_rp_n, s_rp_i;
static char s_rp_msg[256];
#define ILOG 64
static ng_bus_event_t s_ilog[ILOG];
static size_t s_nilog;
static void ilog(int w, int size, uint32_t a, uint32_t v) {
    if (s_nilog < ILOG) {
        s_ilog[s_nilog].write = (uint8_t)w; s_ilog[s_nilog].size = (uint8_t)size;
        s_ilog[s_nilog].addr = a; s_ilog[s_nilog].value = v;
    }
    s_nilog++;
}

void exec_set_verify(int on) { s_verify = on; }

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
    s_miss_seen = (uint8_t *)calloc(0x110000 / 8, 1);   /* one bit per word of the three regions */
    s_interp_only = interp_only;
    for (size_t i = 0; i < nfuncs; i++) {
        ng_func_t *s = slot(funcs[i].addr);
        if (s) *s = funcs[i].fn;
    }
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
}

/* ---- interrupts ---- */


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

/* A routine was entered at an address its switch does not cover: the
 * table and the generated code disagree, which is a recompiler bug. */
void ng_bad_dispatch(uint32_t pc) {
    fprintf(stderr, "fatal: recompiled routine entered at $%06X, which it does not contain\n", pc);
    abort();
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

/* Where interpretation starts, weighted by how long it runs before
 * reaching native code: the recompiler's biggest coverage gaps first.
 * Printed by exec_report() when NG_PROFILE is set. */
#define PROF_SLOTS 4096
static struct { uint32_t pc; uint64_t n; } s_prof[PROF_SLOTS];

static void profile_add(uint32_t pc, uint64_t n) {
    uint32_t h = (pc * 2654435761u) % PROF_SLOTS;
    for (int i = 0; i < PROF_SLOTS; i++, h = (h + 1) % PROF_SLOTS) {
        if (s_prof[h].n == 0 || s_prof[h].pc == pc) { s_prof[h].pc = pc; s_prof[h].n += n; return; }
    }
}

static void profile_print(void) {
    for (int k = 0; k < 20; k++) {
        int best = -1;
        for (int i = 0; i < PROF_SLOTS; i++)
            if (s_prof[i].n && (best < 0 || s_prof[i].n > s_prof[best].n)) best = i;
        if (best < 0) break;
        char buf[128];
        m68k_disassemble(buf, s_prof[best].pc, M68K_CPU_TYPE_68000);
        fprintf(stderr, "[profile] %10llu interpreted from $%06X  %s\n",
                (unsigned long long)s_prof[best].n, s_prof[best].pc, buf);
        s_prof[best].n = 0;
    }
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
        uint32_t prev = CPU.pc, start = CPU.pc;
        uint64_t before = exec_interp_instrs;
        while (ng_cycles < ng_next_event && !mus_stopped()) {
            ng_cycles += m68k_execute(1);
            exec_interp_instrs++;
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
            if (lookup(pc)) break;
            /* A landing more than one instruction away is a jump, call,
             * return or exception: remember it as a routine entry. */
            if (pc < prev || pc > prev + 10) note_miss(pc);
            prev = pc;
        }
        profile_add(start, exec_interp_instrs - before);
    }
    from_musashi();
}

/* ---- --verify: every native block is re-run in Musashi and compared ----
 *
 * The block runs natively with the bus logging every access to mutable
 * memory and I/O. Then Musashi starts from the same registers and runs
 * the same number of instructions, reading ROM directly and taking every
 * other read from the log (so I/O and RAM look exactly as they did);
 * its writes are checked against the log instead of performed. Registers,
 * flags and the write sequence must match exactly. Timing plays no part,
 * so any mismatch is a translation bug in that block. */

#define VERIFY_TRACE 48
static uint32_t s_trace[VERIFY_TRACE];

static void report(uint32_t start, uint64_t n, const ng_cpu_t *nat, const ng_cpu_t *ref, size_t steps) {
    s_verify_fails++;
    if (s_verify_fails > 20) return;
    fprintf(stderr, "[verify] MISMATCH in block entered at $%06X (%llu instructions)\n",
            start, (unsigned long long)n);
    if (s_rp_bad) {
        fprintf(stderr, "  bus: %s\n", s_rp_msg);
        size_t m = s_rp_n > s_nilog ? s_rp_n : s_nilog;
        for (size_t i = 0; i < m && i < ILOG; i++) {
            char a[48] = "", b[48] = "";
            if (i < s_rp_n)
                snprintf(a, sizeof(a), "%s%d $%06X=%X", s_rp[i].write ? "W" : "R", s_rp[i].size * 8,
                         s_rp[i].addr, s_rp[i].value);
            if (i < s_nilog)
                snprintf(b, sizeof(b), "%s%d $%06X=%X", s_ilog[i].write ? "W" : "R", s_ilog[i].size * 8,
                         s_ilog[i].addr, s_ilog[i].value);
            fprintf(stderr, "    native %-24s interp %s\n", a, b);
        }
    }
    for (int i = 0; i < 8; i++) {
        if (nat->d[i] != ref->d[i]) fprintf(stderr, "  D%d native %08X interp %08X\n", i, nat->d[i], ref->d[i]);
        if (nat->a[i] != ref->a[i]) fprintf(stderr, "  A%d native %08X interp %08X\n", i, nat->a[i], ref->a[i]);
    }
    if (nat->pc != ref->pc) fprintf(stderr, "  PC native %06X interp %06X\n", nat->pc, ref->pc);
    if (nat->osp != ref->osp) fprintf(stderr, "  other SP native %08X interp %08X\n", nat->osp, ref->osp);
    uint16_t sn, sr;
    { ng_cpu_t t = CPU; CPU = *nat; sn = ng_get_sr(); CPU = *ref; sr = ng_get_sr(); CPU = t; }
    if (sn != sr) fprintf(stderr, "  SR native %04X interp %04X (XNZVC)\n", sn, sr);
    size_t first = steps > VERIFY_TRACE ? steps - VERIFY_TRACE : 0;
    for (size_t i = first; i < steps; i++) {
        char buf[128];
        m68k_disassemble(buf, s_trace[i % VERIFY_TRACE], M68K_CPU_TYPE_68000);
        fprintf(stderr, "    %06X  %s\n", s_trace[i % VERIFY_TRACE], buf);
    }
}

static void run_verified(ng_func_t fn) {
    ng_cpu_t before = CPU;
    uint64_t i0 = ng_icount;
    bus_log_begin();
    fn();
    const ng_bus_event_t *ev;
    size_t nev = bus_log_end(&ev);
    ng_cpu_t after = CPU;
    uint64_t n = ng_icount - i0;
    int64_t cycles = ng_cycles;

    CPU = before;
    to_musashi();
    m68k_set_irq(0);
    s_rp = ev; s_rp_n = nev; s_rp_i = 0; s_rp_bad = 0; s_replay = 1; s_nilog = 0;
    size_t steps = 0;
    for (uint64_t k = 0; k < n && !mus_stopped(); k++) {
        s_trace[steps++ % VERIFY_TRACE] = m68k_get_reg(NULL, M68K_REG_PC);
        m68k_execute(1);
    }
    s_replay = 0;
    from_musashi();
    ng_cpu_t ref = CPU;
    if (!s_rp_bad && s_rp_i != s_rp_n) {
        s_rp_bad = 1;
        snprintf(s_rp_msg, sizeof(s_rp_msg), "interpreter made %zu accesses, native %zu", s_rp_i, s_rp_n);
    }
    ref.stopped = after.stopped;   /* STOP is modelled differently, not a translation result */
    if (s_rp_bad || memcmp(ref.d, after.d, sizeof(ref.d)) || memcmp(ref.a, after.a, sizeof(ref.a)) ||
        ((ref.pc ^ after.pc) & 0xFFFFFF) || ref.osp != after.osp || ref.x != after.x || ref.n != after.n ||
        ref.z != after.z || ref.v != after.v || ref.c != after.c || ref.s != after.s || ref.ipl != after.ipl)
        report(before.pc, n, &after, &ref, steps);
    s_verify_blocks++;
    CPU = after;
    ng_cycles = cycles;
}

static uint32_t replay_read(uint32_t a, int size) {
    a &= 0xFFFFFF;
    ilog(0, size, a, 0);
    if (s_rp_i < s_rp_n && !s_rp[s_rp_i].write && s_rp[s_rp_i].addr == a && s_rp[s_rp_i].size == size)
        return s_rp[s_rp_i++].value;
    if (!s_rp_bad) {
        s_rp_bad = 1;
        if (s_rp_i < s_rp_n)
            snprintf(s_rp_msg, sizeof(s_rp_msg), "interpreter read%d $%06X where native did %s%d $%06X",
                     size * 8, a, s_rp[s_rp_i].write ? "write" : "read", s_rp[s_rp_i].size * 8, s_rp[s_rp_i].addr);
        else
            snprintf(s_rp_msg, sizeof(s_rp_msg), "interpreter read%d $%06X after native's last access", size * 8, a);
    }
    return 0;
}

static void replay_write(uint32_t a, int size, uint32_t v) {
    a &= 0xFFFFFF;
    ilog(1, size, a, v);
    if (s_rp_i < s_rp_n && s_rp[s_rp_i].write && s_rp[s_rp_i].addr == a && s_rp[s_rp_i].size == size &&
        s_rp[s_rp_i].value == v) { s_rp_i++; return; }
    if (!s_rp_bad) {
        s_rp_bad = 1;
        if (s_rp_i < s_rp_n)
            snprintf(s_rp_msg, sizeof(s_rp_msg), "interpreter write%d $%06X=%X where native did %s%d $%06X=%X",
                     size * 8, a, v, s_rp[s_rp_i].write ? "write" : "read", s_rp[s_rp_i].size * 8,
                     s_rp[s_rp_i].addr, s_rp[s_rp_i].value);
        else
            snprintf(s_rp_msg, sizeof(s_rp_msg), "interpreter write%d $%06X=%X after native's last access", size * 8, a, v);
    }
}

void exec_run(int64_t until) {
    ng_next_event = until;
    while (ng_cycles < ng_next_event) {
        int level = ng_irq_level();
        if (level > CPU.ipl || level == 7) take_irq(level);
        if (CPU.stopped) { ng_cycles = ng_next_event; break; }
        ng_func_t fn = s_interp_only ? NULL : lookup(CPU.pc);
        if (fn) { if (s_verify) run_verified(fn); else fn(); exec_native_blocks++; }
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
    if (s_verify)
        fprintf(stderr, "[verify] %llu blocks checked, %llu mismatches\n",
                (unsigned long long)s_verify_blocks, (unsigned long long)s_verify_fails);
    if (getenv("NG_PROFILE")) profile_print();
    fprintf(stderr, "[exec] native blocks %llu, interpreted instructions %llu, %zu uncovered entry points\n",
            (unsigned long long)exec_native_blocks, (unsigned long long)exec_interp_instrs, s_nmiss);
    (void)total;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* Merges this run's misses into `path`: earlier runs' entries are now
 * recompiled and no longer miss, so overwriting would drop them. */
int exec_dump_misses(const char *path) {
    size_t cap = s_nmiss + 4096, n = 0;
    uint32_t *all = (uint32_t *)malloc(cap * sizeof(uint32_t));
    FILE *f = fopen(path, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            char *end;
            unsigned long v = strtoul(line, &end, 16);
            if (end == line) continue;
            if (n == cap) { cap *= 2; all = (uint32_t *)realloc(all, cap * sizeof(uint32_t)); }
            all[n++] = (uint32_t)v;
        }
        fclose(f);
    }
    for (size_t i = 0; i < s_nmiss; i++) {
        if (n == cap) { cap *= 2; all = (uint32_t *)realloc(all, cap * sizeof(uint32_t)); }
        all[n++] = s_miss[i];
    }
    qsort(all, n, sizeof(uint32_t), cmp_u32);
    f = fopen(path, "w");
    if (!f) { free(all); return -1; }
    size_t written = 0;
    for (size_t i = 0; i < n; i++)
        if (i == 0 || all[i] != all[i - 1]) { fprintf(f, "%06X\n", all[i]); written++; }
    fclose(f);
    free(all);
    fprintf(stderr, "[exec] %zu new entry points; %s now lists %zu\n", s_nmiss, path, written);
    return 0;
}

/* ---- Musashi memory callbacks ---- */

unsigned int m68k_read_memory_8(unsigned int a) { return s_replay ? replay_read(a, 1) : ng_r8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return s_replay ? replay_read(a, 2) : ng_r16(a); }
unsigned int m68k_read_memory_32(unsigned int a) {
    if (!s_replay) return ng_r32(a);
    uint32_t hi = replay_read(a, 2);
    return (hi << 16) | replay_read(a + 2, 2);
}
void m68k_write_memory_8(unsigned int a, unsigned int v) {
    if (s_replay) replay_write(a, 1, v & 0xFF); else ng_w8(a, (uint8_t)v);
}
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    if (s_replay) replay_write(a, 2, v & 0xFFFF); else ng_w16(a, (uint16_t)v);
}
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    if (!s_replay) { ng_w32(a, v); return; }
    replay_write(a, 2, v >> 16);
    replay_write(a + 2, 2, v & 0xFFFF);
}
/* Opcode and extension-word fetches: never logged, never replayed. */
unsigned int m68k_read_immediate_16(unsigned int a) { return s_replay ? bus_peek16(a) : ng_r16(a); }
unsigned int m68k_read_immediate_32(unsigned int a) {
    return (m68k_read_immediate_16(a) << 16) | m68k_read_immediate_16(a + 2);
}
/* PC-relative operands are data reads, same as the native side's. */
unsigned int m68k_read_pcrelative_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_memory_32(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return ng_r16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return ng_r32(a); }
