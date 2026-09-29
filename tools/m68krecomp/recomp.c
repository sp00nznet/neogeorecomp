/*
 * recomp.c — m68krecomp: discovers the 68000 code in a Neo Geo program and
 * system ROM and writes it out as C.
 *
 * Discovery is recursive descent from seeds: the reset/exception vectors
 * of both the system ROM and the cartridge, the four cartridge header
 * entry points the BIOS calls, every static JSR/BSR/JMP target found on
 * the way, and any addresses passed with --entries (typically the list a
 * run wrote with --dump-misses, i.e. targets of computed jumps that only
 * show up at runtime).
 *
 * Each seed becomes one routine: all code reachable from it without
 * leaving through a call, return or jump. A routine can be entered at its
 * seed and at its "resume points": return addresses after calls, loop
 * heads, and the instruction after an SR write. Every such address goes in
 * the dispatch table. Code shared by several routines is emitted in each
 * of them; that costs size, never correctness, because the same address
 * always decodes to the same instructions.
 *
 * docs/recompiler.md describes the output and the execution model.
 */
#include "recomp.h"
#include "romload.h"
#include "m68k.h"
#include "musashi_glue.h"
#include <neogeorecomp/recomp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { F_SEED = 1, F_LABEL = 2 };

typedef struct {
    const char    *name;
    uint32_t       base, size;
    const uint8_t *data;           /* 68k byte order */
    uint8_t       *flags;          /* per 16-bit word */
    int32_t       *owner;          /* routine that the dispatch table maps the word to */
    insn_t       **cache;
    uint32_t      *visit;
} region_t;

static region_t s_reg[3];
static int s_nreg;

static region_t *find(uint32_t a) {
    a &= 0xFFFFFF;
    for (int i = 0; i < s_nreg; i++)
        if (a >= s_reg[i].base && a < s_reg[i].base + s_reg[i].size) return &s_reg[i];
    return NULL;
}

static uint16_t img_read16(uint32_t a, int *ok) {
    region_t *r = find(a);
    if (!r || (a & 1) || a + 1 >= r->base + r->size) { *ok = 0; return 0; }
    const uint8_t *p = r->data + (a - r->base);
    return (uint16_t)((p[0] << 8) | p[1]);
}

static const insn_t *decode_at(uint32_t a) {
    region_t *r = find(a);
    if (!r || (a & 1)) return NULL;
    uint32_t i = (a - r->base) >> 1;
    if (!r->cache[i]) {
        insn_t in;
        if (m68k_decode(a, img_read16, &in)) return NULL;
        r->cache[i] = (insn_t *)malloc(sizeof(insn_t));
        *r->cache[i] = in;
    }
    return r->cache[i];
}

/* ---- Musashi, used here only for disassembly text and cycle counts ---- */

unsigned int m68k_read_disassembler_16(unsigned int a) { int ok = 1; return img_read16(a, &ok); }
unsigned int m68k_read_disassembler_32(unsigned int a) {
    int ok = 1;
    return ((unsigned)img_read16(a, &ok) << 16) | img_read16(a + 2, &ok);
}
unsigned int m68k_read_memory_8(unsigned int a) { int ok = 1; return img_read16(a & ~1u, &ok) >> ((a & 1) ? 0 : 8) & 0xFF; }
unsigned int m68k_read_memory_16(unsigned int a) { int ok = 1; return img_read16(a, &ok); }
unsigned int m68k_read_memory_32(unsigned int a) { return m68k_read_disassembler_32(a); }
unsigned int m68k_read_immediate_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_immediate_32(unsigned int a) { return m68k_read_memory_32(a); }
unsigned int m68k_read_pcrelative_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_32(unsigned int a, unsigned int v) { (void)a; (void)v; }

int rc_base_cycles(uint16_t opcode) { return mus_base_cycles(opcode); }

/* ---- discovery ---- */

static uint32_t *s_work;
static size_t s_nwork, s_capwork;

static void add_seed(uint32_t a) {
    a &= 0xFFFFFF;
    region_t *r = find(a);
    if (!r || (a & 1)) return;
    uint8_t *f = &r->flags[(a - r->base) >> 1];
    if (*f & F_SEED) return;
    *f |= F_SEED;
    if (s_nwork == s_capwork) {
        s_capwork = s_capwork ? s_capwork * 2 : 4096;
        s_work = (uint32_t *)realloc(s_work, s_capwork * sizeof(uint32_t));
    }
    s_work[s_nwork++] = a;
}

static void mark_label(uint32_t a) {
    region_t *r = find(a);
    if (r && !(a & 1)) r->flags[(a - r->base) >> 1] |= F_LABEL;
}

typedef struct { uint32_t seed; uint32_t *addrs; int n; } routine_t;
static routine_t *s_rt;
static int s_nrt, s_caprt;
static uint32_t s_gen;

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int static_target(const ea_t *e, uint32_t *t) {
    if (e->mode == EA_ABS || e->mode == EA_PCD16) { *t = e->value & 0xFFFFFF; return 1; }
    return 0;
}

static void explore(uint32_t seed) {
    s_gen++;
    uint32_t *stack = NULL, *list = NULL;
    size_t nstack = 0, capstack = 0, nlist = 0, caplist = 0;
#define PUSH(v) do { if (nstack == capstack) { capstack = capstack ? capstack * 2 : 256; \
    stack = (uint32_t *)realloc(stack, capstack * 4); } stack[nstack++] = (v); } while (0)
    PUSH(seed);
    while (nstack) {
        uint32_t a = stack[--nstack];
        region_t *r = find(a);
        if (!r || (a & 1)) continue;
        uint32_t w = (a - r->base) >> 1;
        if (r->visit[w] == s_gen) continue;
        const insn_t *in = decode_at(a);
        if (!in) continue;
        r->visit[w] = s_gen;
        if (nlist == caplist) { caplist = caplist ? caplist * 2 : 256; list = (uint32_t *)realloc(list, caplist * 4); }
        list[nlist++] = a;
        uint32_t next = a + (uint32_t)in->len, t;
        switch (in->op) {
        case OP_BRA:
            if (in->target <= a) mark_label(in->target);
            PUSH(in->target);
            break;
        case OP_BCC: case OP_DBCC:
            if (in->target <= a) mark_label(in->target);
            PUSH(in->target);
            PUSH(next);
            break;
        case OP_BSR:
            add_seed(in->target);
            mark_label(next); PUSH(next);
            break;
        case OP_JSR:
            if (static_target(&in->src, &t)) add_seed(t);
            mark_label(next); PUSH(next);
            break;
        case OP_JMP:
            if (static_target(&in->src, &t)) add_seed(t);
            break;
        case OP_RTS: case OP_RTE: case OP_RTR: case OP_ILLEGAL: case OP_LINEA: case OP_LINEF:
            break;
        case OP_TRAP: case OP_TRAPV: case OP_CHK: case OP_DIVU: case OP_DIVS:
        case OP_MOVE_TO_SR: case OP_ANDI_SR: case OP_ORI_SR: case OP_EORI_SR: case OP_STOP:
            mark_label(next); PUSH(next);
            break;
        default:
            PUSH(next);
            break;
        }
    }
#undef PUSH
    free(stack);
    qsort(list, nlist, sizeof(uint32_t), cmp_u32);
    if (s_nrt == s_caprt) { s_caprt = s_caprt ? s_caprt * 2 : 1024; s_rt = (routine_t *)realloc(s_rt, (size_t)s_caprt * sizeof(routine_t)); }
    s_rt[s_nrt].seed = seed;
    s_rt[s_nrt].addrs = list;
    s_rt[s_nrt].n = (int)nlist;
    s_nrt++;
}

int rc_in_routine(const rc_ctx_t *ctx, uint32_t a) {
    int lo = 0, hi = ctx->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ctx->addrs[mid] == a) return 1;
        if (ctx->addrs[mid] < a) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int falls_through(op_t op) {
    switch (op) {
    case OP_BRA: case OP_JMP: case OP_JSR: case OP_BSR: case OP_RTS: case OP_RTE: case OP_RTR:
    case OP_ILLEGAL: case OP_LINEA: case OP_LINEF: case OP_TRAP: case OP_STOP:
    case OP_MOVE_TO_SR: case OP_ANDI_SR: case OP_ORI_SR: case OP_EORI_SR:
        return 0;
    default: return 1;
    }
}

static int32_t *owner_slot(uint32_t a) {
    region_t *r = find(a);
    return r ? &r->owner[(a - r->base) >> 1] : NULL;
}
static uint8_t flags_at(uint32_t a) {
    region_t *r = find(a);
    return r ? r->flags[(a - r->base) >> 1] : 0;
}

/* ---- output ---- */

static void emit_routine(FILE *o, int idx) {
    const routine_t *rt = &s_rt[idx];
    rc_ctx_t ctx = {rt->seed, rt->addrs, rt->n};
    fprintf(o, "\n/* routine $%06X: %d instructions */\nvoid r_%06X(void) {\n    switch (CPU.pc) {\n",
            rt->seed, rt->n, rt->seed);
    for (int i = 0; i < rt->n; i++) {
        int32_t *ow = owner_slot(rt->addrs[i]);
        if (ow && *ow == idx) fprintf(o, "    case 0x%06Xu: goto L_%06X;\n", rt->addrs[i], rt->addrs[i]);
    }
    fprintf(o, "    default: ng_bad_dispatch(CPU.pc); return;\n    }\n");
    for (int i = 0; i < rt->n; i++) {
        uint32_t a = rt->addrs[i];
        const insn_t *in = decode_at(a);
        char dis[128];
        m68k_disassemble(dis, a, M68K_CPU_TYPE_68000);
        for (char *p = dis; *p; p++) if (*p == '*' && p[1] == '/') *p = '+';
        fprintf(o, "L_%06X: /* %06X: %s */\n    ", a, a, dis);
        rc_emit_insn(o, &ctx, in);
        uint32_t next = a + (uint32_t)in->len;
        if (falls_through(in->op) && (i + 1 >= rt->n || rt->addrs[i + 1] != next)) {
            if (rc_in_routine(&ctx, next)) fprintf(o, "    goto L_%06X;\n", next);
            else fprintf(o, "    CPU.pc = 0x%06Xu; return;\n", next);
        }
    }
    fprintf(o, "}\n");
}

static FILE *open_out(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) fprintf(stderr, "error: cannot write %s\n", path);
    return f;
}

static const char k_banner[] =
    "/* Generated by m68krecomp from your own ROM dump. Derived from copyrighted\n"
    " * code: never commit or distribute this file. */\n";

static int write_output(const char *dir, int nfiles, const char *game) {
    FILE *h = open_out(dir, "recomp_common.h");
    if (!h) return -1;
    fprintf(h, "%s#pragma once\n#include <neogeorecomp/bus.h>\n#include <neogeorecomp/cpu.h>\n"
               "void ng_bad_dispatch(uint32_t pc);\n"
               "#ifdef _MSC_VER\n#pragma warning(disable: 4102 4189 4244 4146 4018)\n#endif\n"
               "#if defined(__GNUC__)\n#pragma GCC diagnostic ignored \"-Wunused-label\"\n"
               "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n#endif\n", k_banner);
    fclose(h);

    for (int f = 0; f < nfiles; f++) {
        char name[64];
        snprintf(name, sizeof(name), "recomp_%02d.c", f);
        FILE *o = open_out(dir, name);
        if (!o) return -1;
        fprintf(o, "%s/* %s, part %d of %d */\n#include \"recomp_common.h\"\n", k_banner, game, f + 1, nfiles);
        for (int i = f; i < s_nrt; i += nfiles) emit_routine(o, i);
        fclose(o);
    }

    FILE *t = open_out(dir, "recomp_table.c");
    if (!t) return -1;
    fprintf(t, "%s#include <neogeorecomp/neogeo.h>\n\n", k_banner);
    for (int i = 0; i < s_nrt; i++) fprintf(t, "void r_%06X(void);\n", s_rt[i].seed);
    fprintf(t, "\nconst ng_func_entry_t ng_recomp_table[] = {\n");
    size_t count = 0;
    for (int ri = 0; ri < s_nreg; ri++) {
        region_t *r = &s_reg[ri];
        for (uint32_t w = 0; w < r->size / 2; w++) {
            if (r->owner[w] < 0) continue;
            fprintf(t, "    {0x%06Xu, r_%06X},\n", r->base + w * 2, s_rt[r->owner[w]].seed);
            count++;
        }
    }
    fprintf(t, "};\nconst size_t ng_recomp_count = %zu;\n", count);
    fclose(t);
    fprintf(stderr, "[m68krecomp] %d routines, %zu dispatch entries -> %s\n", s_nrt, count, dir);
    return 0;
}

static void add_region(const char *name, uint32_t base, uint32_t size, const uint8_t *data) {
    region_t *r = &s_reg[s_nreg++];
    r->name = name; r->base = base; r->size = size; r->data = data;
    r->flags = (uint8_t *)calloc(size / 2, 1);
    r->owner = (int32_t *)malloc(size / 2 * sizeof(int32_t));
    for (uint32_t i = 0; i < size / 2; i++) r->owner[i] = -1;
    r->cache = (insn_t **)calloc(size / 2, sizeof(insn_t *));
    r->visit = (uint32_t *)calloc(size / 2, sizeof(uint32_t));
}

static uint32_t rd32(uint32_t a) {
    int ok = 1;
    return ((uint32_t)img_read16(a, &ok) << 16) | img_read16(a + 2, &ok);
}

static int load_entries(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return -1; }
    char line[64];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char *end;
        unsigned long v = strtoul(line, &end, 16);
        if (end != line) { add_seed((uint32_t)v); n++; }
    }
    fclose(f);
    fprintf(stderr, "[m68krecomp] %d extra entry points from %s\n", n, path);
    return 0;
}

int ng_recomp_main(int argc, char **argv, const ng_game_t *game) {
    const char *rom_path = "roms", *bios_path = NULL, *bios_name = "sp-s2.sp1", *out = "generated";
    const char *entries[16];
    int nentries = 0, nfiles = 16;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom-path") && next) { rom_path = next; i++; }
        else if (!strcmp(a, "--bios-path") && next) { bios_path = next; i++; }
        else if (!strcmp(a, "--bios") && next) { bios_name = next; i++; }
        else if (!strcmp(a, "--out") && next) { out = next; i++; }
        else if (!strcmp(a, "--files") && next) { nfiles = atoi(next); i++; }
        else if (!strcmp(a, "--entries") && next && nentries < 16) { entries[nentries++] = next; i++; }
        else {
            fprintf(stderr, "usage: %s [--rom-path DIR] [--bios-path DIR] [--bios FILE] [--out DIR]\n"
                            "          [--files N] [--entries FILE]...\n", argv[0]);
            return 2;
        }
    }
    if (!bios_path) bios_path = rom_path;
    if (nfiles < 1) nfiles = 1;

    uint8_t *prom = ng_load_prom(rom_path, game);
    size_t bios_size;
    uint8_t *bios = ng_read_file(bios_path, bios_name, &bios_size);
    if (!prom || !bios || bios_size < 0x20000) { fprintf(stderr, "error: missing ROM files\n"); return 1; }
    ng_swap16(bios, 0x20000);

    uint32_t fixed = game->prom_size < 0x100000 ? game->prom_size : 0x100000;
    add_region("prom", 0x000000, fixed, prom);
    if (game->prom_size > 0x100000) {
        uint32_t banked = game->prom_size - 0x100000;
        add_region("bank", 0x200000, banked > 0x100000 ? 0x100000 : banked, prom + 0x100000);
    }
    add_region("bios", 0xC00000, 0x20000, bios);

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);

    /* Vectors 1-63 of both tables (vector 0 is the stack pointer). */
    for (int v = 1; v < 64; v++) { add_seed(rd32(0xC00000 + v * 4)); add_seed(rd32(v * 4)); }
    /* Cartridge header entry points the BIOS calls: USER, PLAYER_START,
     * DEMO_END, COIN_SOUND. */
    add_seed(0x122); add_seed(0x128); add_seed(0x12E); add_seed(0x134);
    for (int i = 0; i < nentries; i++) if (load_entries(entries[i])) return 1;

    while (s_nwork) explore(s_work[--s_nwork]);

    /* Seeds own themselves; other entry points go to the first routine
     * that contains them. */
    for (int i = 0; i < s_nrt; i++) {
        int32_t *ow = owner_slot(s_rt[i].seed);
        if (ow && s_rt[i].n > 0) *ow = i;
    }
    for (int i = 0; i < s_nrt; i++)
        for (int k = 0; k < s_rt[i].n; k++) {
            uint32_t a = s_rt[i].addrs[k];
            int32_t *ow = owner_slot(a);
            if (ow && *ow < 0 && (flags_at(a) & (F_SEED | F_LABEL))) *ow = i;
        }

    size_t insns = 0;
    for (int i = 0; i < s_nrt; i++) insns += (size_t)s_rt[i].n;
    fprintf(stderr, "[m68krecomp] %s: %zu instructions emitted across routines\n", game->name, insns);
    return write_output(out, nfiles, game->name) ? 1 : 0;
}
