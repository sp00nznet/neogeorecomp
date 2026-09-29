/*
 * emit.c — lowers decoded 68000 instructions to C.
 *
 * Every instruction becomes one C block: cycle accounting, operand
 * fetches in the 68000's order (source EA before destination EA, so
 * (An)+/-(An) side effects land exactly as on hardware), the operation
 * through the flag-correct helpers in <neogeorecomp/cpu.h>, then the
 * store. Control flow that leaves the routine sets CPU.pc and returns to
 * the dispatcher (see src/exec.c for why control flow is flat).
 */
#include "recomp.h"
#include <stdio.h>
#include <string.h>

static const char *k_cc[16] = {
    "CC_T", "CC_F", "CC_HI", "CC_LS", "CC_CC", "CC_CS", "CC_NE", "CC_EQ",
    "CC_VC", "CC_VS", "CC_PL", "CC_MI", "CC_GE", "CC_LT", "CC_GT", "CC_LE",
};

static const char *utype(int size) { return size == 1 ? "uint8_t" : size == 2 ? "uint16_t" : "uint32_t"; }
static int bits(int size) { return size * 8; }

static void xreg_expr(char *buf, size_t n, const ea_t *e) {
    const char *r = e->xreg < 8 ? "CPU.d" : "CPU.a";
    int idx = e->xreg & 7;
    if (e->xlong) snprintf(buf, n, "%s[%d]", r, idx);
    else snprintf(buf, n, "SX16(%s[%d])", r, idx);
}

/* Emits `uint32_t var = <address>;`, applying (An)+ / -(An) side effects.
 * Byte accesses through A7 step by 2 to keep the stack word-aligned. */
static void ea_addr(FILE *o, const ea_t *e, int size, const char *var) {
    int step = (size == 1 && e->reg == 7) ? 2 : size;
    char x[48];
    switch (e->mode) {
    case EA_IND: fprintf(o, "uint32_t %s = CPU.a[%d]; ", var, e->reg); break;
    case EA_POSTINC: fprintf(o, "uint32_t %s = CPU.a[%d]; CPU.a[%d] += %d; ", var, e->reg, e->reg, step); break;
    case EA_PREDEC: fprintf(o, "CPU.a[%d] -= %d; uint32_t %s = CPU.a[%d]; ", e->reg, step, var, e->reg); break;
    case EA_D16: fprintf(o, "uint32_t %s = CPU.a[%d] + (uint32_t)(%d); ", var, e->reg, e->disp); break;
    case EA_IDX:
        xreg_expr(x, sizeof(x), e);
        fprintf(o, "uint32_t %s = CPU.a[%d] + (uint32_t)(%d) + %s; ", var, e->reg, e->disp, x);
        break;
    /* Keep all 32 bits: LEA/PEA of a sign-extended .w address must see
     * $FFFFxxxx; the bus masks to 24 bits on access. */
    case EA_ABS: case EA_PCD16: fprintf(o, "uint32_t %s = 0x%08Xu; ", var, e->value); break;
    case EA_PCIDX:
        xreg_expr(x, sizeof(x), e);
        fprintf(o, "uint32_t %s = 0x%08Xu + %s; ", var, e->value, x);
        break;
    default: fprintf(o, "uint32_t %s = 0; /* bad ea */ ", var); break;
    }
}

static int is_mem(const ea_t *e) {
    return e->mode != EA_DN && e->mode != EA_AN && e->mode != EA_IMM && e->mode != EA_NONE;
}

/* Emits `T var = <value>;`; for memory operands the address goes to `avar`. */
static void ea_read(FILE *o, const ea_t *e, int size, const char *var, const char *avar) {
    const char *t = utype(size);
    switch (e->mode) {
    case EA_DN: fprintf(o, "%s %s = (%s)CPU.d[%d]; ", t, var, t, e->reg); return;
    case EA_AN: fprintf(o, "%s %s = (%s)CPU.a[%d]; ", t, var, t, e->reg); return;
    case EA_IMM: fprintf(o, "%s %s = (%s)0x%Xu; ", t, var, t, e->value); return;
    default:
        ea_addr(o, e, size, avar);
        fprintf(o, "%s %s = ng_r%d(%s); ", t, var, bits(size), avar);
    }
}

static void write_dn(FILE *o, int reg, int size, const char *val) {
    if (size == 4) fprintf(o, "CPU.d[%d] = %s; ", reg, val);
    else if (size == 2) fprintf(o, "CPU.d[%d] = (CPU.d[%d] & 0xFFFF0000u) | (uint16_t)(%s); ", reg, reg, val);
    else fprintf(o, "CPU.d[%d] = (CPU.d[%d] & 0xFFFFFF00u) | (uint8_t)(%s); ", reg, reg, val);
}

/* Stores to an operand whose address (if memory) is already in `avar`. */
static void ea_store(FILE *o, const ea_t *e, int size, const char *val, const char *avar) {
    if (e->mode == EA_DN) write_dn(o, e->reg, size, val);
    else if (e->mode == EA_AN) fprintf(o, "CPU.a[%d] = %s; ", e->reg, val);
    else fprintf(o, "ng_w%d(%s, %s); ", bits(size), avar, val);
}

/* Computes the address then stores (destination-only operands). */
static void ea_write(FILE *o, const ea_t *e, int size, const char *val, const char *avar) {
    if (is_mem(e)) ea_addr(o, e, size, avar);
    ea_store(o, e, size, val, avar);
}

static void exit_to(FILE *o, uint32_t pc) { fprintf(o, "CPU.pc = 0x%06Xu; return; ", pc & 0xFFFFFF); }

/* A branch inside the routine. Back-edges yield to the scheduler once an
 * event is due, resuming at the target (which is always a table entry). */
static void branch(FILE *o, const rc_ctx_t *ctx, const insn_t *in, uint32_t target) {
    if (!rc_in_routine(ctx, target)) { exit_to(o, target); return; }
    if (target <= in->addr)
        fprintf(o, "if (ng_cycles >= ng_next_event) { CPU.pc = 0x%06Xu; return; } ", target);
    fprintf(o, "goto L_%06X; ", target);
}

static const char *alu_name(op_t op) {
    switch (op) {
    case OP_ADD: case OP_ADDI: case OP_ADDQ: return "add";
    case OP_SUB: case OP_SUBI: case OP_SUBQ: return "sub";
    case OP_AND: case OP_ANDI: return "and";
    case OP_OR: case OP_ORI: return "or";
    case OP_EOR: case OP_EORI: return "eor";
    case OP_ASL: return "asl"; case OP_ASR: return "asr";
    case OP_LSL: return "lsl"; case OP_LSR: return "lsr";
    case OP_ROL: return "rol"; case OP_ROR: return "ror";
    case OP_ROXL: return "roxl"; case OP_ROXR: return "roxr";
    default: return "?";
    }
}

/* s OP d for the logical ops, via the flag helper. */
static void logic(FILE *o, op_t op, int size, const char *s, const char *d, const char *res) {
    const char *c = op == OP_AND || op == OP_ANDI ? "&" : op == OP_OR || op == OP_ORI ? "|" : "^";
    fprintf(o, "%s %s = ng_logic%d((%s)(%s %s %s)); ", utype(size), res, bits(size), utype(size), d, c, s);
}

/* The body of a two-operand ALU instruction: src into dst (RMW). */
static void alu_rmw(FILE *o, const insn_t *in, op_t op, const char *src_expr) {
    int sz = in->size, b = bits(sz);
    ea_read(o, &in->dst, sz, "d", "ad");
    if (op == OP_AND || op == OP_ANDI || op == OP_OR || op == OP_ORI || op == OP_EOR || op == OP_EORI)
        logic(o, op, sz, src_expr, "d", "r");
    else
        fprintf(o, "%s r = ng_%s%d(%s, d); ", utype(sz), alu_name(op), b, src_expr);
    ea_store(o, &in->dst, sz, "r", "ad");
}

static int movem_count(uint16_t m) { int n = 0; while (m) { n += m & 1; m >>= 1; } return n; }

void rc_emit_insn(FILE *o, const rc_ctx_t *ctx, const insn_t *in) {
    uint32_t next = in->addr + (uint32_t)in->len;
    int sz = in->size, b = bits(sz);
    const char *t = utype(sz);
    int cyc = rc_base_cycles(in->opcode);

    fprintf(o, "{ NG_INSN(%d); ", cyc);
    switch (in->op) {
    case OP_ILLEGAL: fprintf(o, "ng_exception(4, 0x%06Xu); return; ", in->addr); break;
    case OP_LINEA: fprintf(o, "ng_exception(10, 0x%06Xu); return; ", in->addr); break;
    case OP_LINEF: fprintf(o, "ng_exception(11, 0x%06Xu); return; ", in->addr); break;

    case OP_MOVE:
        ea_read(o, &in->src, sz, "v", "as");
        fprintf(o, "ng_logic%d(v); ", b);
        if (sz == 4 && in->dst.mode == EA_PREDEC) {
            /* MOVE.L to -(An) stores the low word first on a 68000. */
            ea_addr(o, &in->dst, 4, "ad");
            fprintf(o, "ng_w16(ad + 2, (uint16_t)v); ng_w16(ad, (uint16_t)(v >> 16)); ");
        } else {
            ea_write(o, &in->dst, sz, "v", "ad");
        }
        break;
    case OP_MOVEA:
        ea_read(o, &in->src, sz, "v", "as");
        fprintf(o, "CPU.a[%d] = %s; ", in->dst.reg, sz == 2 ? "SX16(v)" : "v");
        break;
    case OP_MOVEQ:
        fprintf(o, "CPU.d[%d] = ng_logic32(0x%08Xu); ", in->dst.reg, (uint32_t)in->data);
        break;
    case OP_LEA:
        ea_addr(o, &in->src, 4, "a");
        fprintf(o, "CPU.a[%d] = a; ", in->dst.reg);
        break;
    case OP_PEA:
        ea_addr(o, &in->src, 4, "a");
        fprintf(o, "ng_push32(a); ");
        break;
    case OP_CLR:
        fprintf(o, "CPU.n = 0; CPU.z = 1; CPU.v = 0; CPU.c = 0; ");
        ea_write(o, &in->dst, sz, "0", "ad");
        break;
    case OP_EXT:
        if (sz == 2) fprintf(o, "uint16_t r = ng_logic16((uint16_t)SX8(CPU.d[%d])); ", in->dst.reg);
        else fprintf(o, "uint32_t r = ng_logic32(SX16(CPU.d[%d])); ", in->dst.reg);
        write_dn(o, in->dst.reg, sz, "r");
        break;
    case OP_SWAP:
        fprintf(o, "CPU.d[%d] = ng_logic32((CPU.d[%d] >> 16) | (CPU.d[%d] << 16)); ",
                in->dst.reg, in->dst.reg, in->dst.reg);
        break;
    case OP_EXG: {
        const char *a = in->src.mode == EA_DN ? "CPU.d" : "CPU.a";
        const char *c = in->dst.mode == EA_DN ? "CPU.d" : "CPU.a";
        fprintf(o, "uint32_t x = %s[%d]; %s[%d] = %s[%d]; %s[%d] = x; ",
                a, in->src.reg, a, in->src.reg, c, in->dst.reg, c, in->dst.reg);
        break;
    }
    case OP_LINK:
        if (in->src.reg == 7) fprintf(o, "CPU.a[7] -= 4; ng_w32(CPU.a[7], CPU.a[7]); ");
        else fprintf(o, "ng_push32(CPU.a[%d]); CPU.a[%d] = CPU.a[7]; ", in->src.reg, in->src.reg);
        fprintf(o, "CPU.a[7] += (uint32_t)(%d); ", in->data);
        break;
    case OP_UNLK:
        fprintf(o, "CPU.a[7] = CPU.a[%d]; CPU.a[%d] = ng_pop32(); ", in->src.reg, in->src.reg);
        break;
    case OP_MOVE_TO_USP: fprintf(o, "ng_set_usp(CPU.a[%d]); ", in->src.reg); break;
    case OP_MOVE_FROM_USP: fprintf(o, "CPU.a[%d] = ng_get_usp(); ", in->dst.reg); break;

    case OP_MOVE_FROM_SR:
        ea_write(o, &in->dst, 2, "ng_get_sr()", "ad");
        break;
    case OP_MOVE_TO_CCR:
        ea_read(o, &in->src, 2, "v", "as");
        fprintf(o, "ng_set_ccr(v); ");
        break;
    case OP_MOVE_TO_SR:
        ea_read(o, &in->src, 2, "v", "as");
        fprintf(o, "ng_set_sr(v); ");
        exit_to(o, next);          /* the mask may have dropped: let the dispatcher look */
        break;
    case OP_ORI_CCR: fprintf(o, "ng_set_ccr(ng_get_ccr() | 0x%02Xu); ", in->src.value & 0x1F); break;
    case OP_ANDI_CCR: fprintf(o, "ng_set_ccr(ng_get_ccr() & 0x%02Xu); ", in->src.value & 0xFF); break;
    case OP_EORI_CCR: fprintf(o, "ng_set_ccr(ng_get_ccr() ^ 0x%02Xu); ", in->src.value & 0x1F); break;
    case OP_ORI_SR: fprintf(o, "ng_set_sr((uint16_t)(ng_get_sr() | 0x%04Xu)); ", in->src.value); exit_to(o, next); break;
    case OP_ANDI_SR: fprintf(o, "ng_set_sr((uint16_t)(ng_get_sr() & 0x%04Xu)); ", in->src.value); exit_to(o, next); break;
    case OP_EORI_SR: fprintf(o, "ng_set_sr((uint16_t)(ng_get_sr() ^ 0x%04Xu)); ", in->src.value); exit_to(o, next); break;

    case OP_ADD: case OP_SUB: case OP_AND: case OP_OR:
        if (in->dst.mode == EA_DN) {          /* <ea>,Dn */
            ea_read(o, &in->src, sz, "s", "as");
            alu_rmw(o, in, in->op, "s");
        } else {                              /* Dn,<ea> */
            fprintf(o, "%s s = (%s)CPU.d[%d]; ", t, t, in->src.reg);
            alu_rmw(o, in, in->op, "s");
        }
        break;
    case OP_EOR:
        fprintf(o, "%s s = (%s)CPU.d[%d]; ", t, t, in->src.reg);
        alu_rmw(o, in, in->op, "s");
        break;
    case OP_ADDI: case OP_SUBI: case OP_ANDI: case OP_ORI: case OP_EORI: {
        char imm[32];
        snprintf(imm, sizeof(imm), "(%s)0x%Xu", t, in->src.value);
        alu_rmw(o, in, in->op, imm);
        break;
    }
    case OP_ADDQ: case OP_SUBQ:
        if (in->dst.mode == EA_AN) {
            fprintf(o, "CPU.a[%d] %s= %du; ", in->dst.reg, in->op == OP_ADDQ ? "+" : "-", in->data);
        } else {
            char imm[32];
            snprintf(imm, sizeof(imm), "(%s)%d", t, in->data);
            alu_rmw(o, in, in->op, imm);
        }
        break;
    case OP_ADDA: case OP_SUBA:
        ea_read(o, &in->src, sz, "s", "as");
        fprintf(o, "CPU.a[%d] %s= %s; ", in->dst.reg, in->op == OP_ADDA ? "+" : "-", sz == 2 ? "SX16(s)" : "s");
        break;
    case OP_CMP:
        ea_read(o, &in->src, sz, "s", "as");
        fprintf(o, "ng_cmp%d(s, (%s)CPU.d[%d]); ", b, t, in->dst.reg);
        break;
    case OP_CMPA:
        ea_read(o, &in->src, sz, "s", "as");
        fprintf(o, "ng_cmp32(%s, CPU.a[%d]); ", sz == 2 ? "SX16(s)" : "s", in->dst.reg);
        break;
    case OP_CMPI:
        ea_read(o, &in->dst, sz, "d", "ad");
        fprintf(o, "ng_cmp%d((%s)0x%Xu, d); ", b, t, in->src.value);
        break;
    case OP_CMPM:
        ea_read(o, &in->src, sz, "s", "as");
        ea_read(o, &in->dst, sz, "d", "ad");
        fprintf(o, "ng_cmp%d(s, d); ", b);
        break;
    case OP_ADDX: case OP_SUBX:
        ea_read(o, &in->src, sz, "s", "as");
        ea_read(o, &in->dst, sz, "d", "ad");
        fprintf(o, "%s r = ng_%s%d(s, d); ", t, in->op == OP_ADDX ? "addx" : "subx", b);
        ea_store(o, &in->dst, sz, "r", "ad");
        break;
    case OP_NEG: case OP_NEGX: case OP_NOT:
        ea_read(o, &in->dst, sz, "d", "ad");
        if (in->op == OP_NOT) fprintf(o, "%s r = ng_logic%d((%s)~d); ", t, b, t);
        else fprintf(o, "%s r = ng_%s%d(d); ", t, in->op == OP_NEG ? "neg" : "negx", b);
        ea_store(o, &in->dst, sz, "r", "ad");
        break;
    case OP_TST:
        ea_read(o, &in->dst, sz, "d", "ad");
        fprintf(o, "ng_logic%d(d); ", b);
        break;
    case OP_TAS:
        ea_read(o, &in->dst, 1, "d", "ad");
        fprintf(o, "ng_logic8(d); ");
        ea_store(o, &in->dst, 1, "(uint8_t)(d | 0x80)", "ad");
        break;
    case OP_MULU: case OP_MULS:
        ea_read(o, &in->src, 2, "s", "as");
        fprintf(o, "CPU.d[%d] = ng_%s(s, (uint16_t)CPU.d[%d]); ", in->dst.reg,
                in->op == OP_MULU ? "mulu" : "muls", in->dst.reg);
        break;
    case OP_DIVU: case OP_DIVS:
        ea_read(o, &in->src, 2, "s", "as");
        fprintf(o, "if (!ng_%s(s, &CPU.d[%d])) { ng_exception(5, 0x%06Xu); return; } ",
                in->op == OP_DIVU ? "divu" : "divs", in->dst.reg, next);
        break;
    case OP_CHK:
        ea_read(o, &in->src, 2, "s", "as");
        fprintf(o, "if (ng_chk(s, (uint16_t)CPU.d[%d])) { ng_exception(6, 0x%06Xu); return; } ",
                in->dst.reg, next);
        break;
    case OP_ABCD: case OP_SBCD:
        ea_read(o, &in->src, 1, "s", "as");
        ea_read(o, &in->dst, 1, "d", "ad");
        fprintf(o, "uint8_t r = ng_%s(s, d); ", in->op == OP_ABCD ? "abcd" : "sbcd");
        ea_store(o, &in->dst, 1, "r", "ad");
        break;
    case OP_NBCD:
        ea_read(o, &in->dst, 1, "d", "ad");
        fprintf(o, "uint8_t r; if (ng_nbcd(d, &r)) { ");
        ea_store(o, &in->dst, 1, "r", "ad");
        fprintf(o, "} ");
        break;

    case OP_ASL: case OP_ASR: case OP_LSL: case OP_LSR:
    case OP_ROL: case OP_ROR: case OP_ROXL: case OP_ROXR:
        if (is_mem(&in->dst)) {
            ea_read(o, &in->dst, 2, "d", "ad");
            fprintf(o, "uint16_t r = ng_%s16(d, 1); ", alu_name(in->op));
            ea_store(o, &in->dst, 2, "r", "ad");
        } else if (in->shift_reg) {
            fprintf(o, "unsigned n = CPU.d[%d] & 63; ng_cycles += 2 * n; ", in->src.reg);
            fprintf(o, "%s r = ng_%s%d((%s)CPU.d[%d], n); ", t, alu_name(in->op), b, t, in->dst.reg);
            write_dn(o, in->dst.reg, sz, "r");
        } else {
            fprintf(o, "ng_cycles += %d; ", 2 * in->data);
            fprintf(o, "%s r = ng_%s%d((%s)CPU.d[%d], %d); ", t, alu_name(in->op), b, t, in->dst.reg, in->data);
            write_dn(o, in->dst.reg, sz, "r");
        }
        break;

    case OP_BTST: case OP_BCHG: case OP_BCLR: case OP_BSET: {
        int mask = in->dst.mode == EA_DN ? 31 : 7;
        if (in->src.mode == EA_DN) fprintf(o, "unsigned bit = CPU.d[%d] & %d; ", in->src.reg, mask);
        else fprintf(o, "unsigned bit = %u; ", in->src.value & (unsigned)mask);
        int s = in->dst.mode == EA_DN ? 4 : 1;
        ea_read(o, &in->dst, s, "d", "ad");
        fprintf(o, "CPU.z = !((d >> bit) & 1); ");
        if (in->op != OP_BTST) {
            const char *f = in->op == OP_BCHG ? "d ^ (1u << bit)" : in->op == OP_BCLR ? "d & ~(1u << bit)" : "d | (1u << bit)";
            char val[64];
            snprintf(val, sizeof(val), "(%s)(%s)", utype(s), f);
            ea_store(o, &in->dst, s, val, "ad");
        }
        break;
    }
    case OP_MOVEP:
        fprintf(o, "uint32_t a = CPU.a[%d] + (uint32_t)(%d); ", in->dst.reg, in->dst.disp);
        if (in->data) {       /* register to memory */
            if (sz == 4) fprintf(o, "ng_w8(a, (uint8_t)(CPU.d[%d] >> 24)); ng_w8(a + 2, (uint8_t)(CPU.d[%d] >> 16)); "
                                    "ng_w8(a + 4, (uint8_t)(CPU.d[%d] >> 8)); ng_w8(a + 6, (uint8_t)CPU.d[%d]); ",
                                 in->src.reg, in->src.reg, in->src.reg, in->src.reg);
            else fprintf(o, "ng_w8(a, (uint8_t)(CPU.d[%d] >> 8)); ng_w8(a + 2, (uint8_t)CPU.d[%d]); ",
                         in->src.reg, in->src.reg);
        } else {
            if (sz == 4) fprintf(o, "CPU.d[%d] = ((uint32_t)ng_r8(a) << 24) | ((uint32_t)ng_r8(a + 2) << 16) | "
                                    "((uint32_t)ng_r8(a + 4) << 8) | ng_r8(a + 6); ", in->src.reg);
            else fprintf(o, "CPU.d[%d] = (CPU.d[%d] & 0xFFFF0000u) | ((uint32_t)ng_r8(a) << 8) | ng_r8(a + 2); ",
                         in->src.reg, in->src.reg);
        }
        break;

    case OP_MOVEM_TO_MEM: {
        int n = movem_count(in->reglist), step = sz;
        fprintf(o, "ng_cycles += %d; ", n * (sz == 2 ? 4 : 8));
        if (in->dst.mode == EA_PREDEC) {
            /* Mask bit 0 is A7 here; registers are stored from A7 down to D0
             * and An is written back once, so a stored An is its old value. */
            fprintf(o, "uint32_t a = CPU.a[%d]; ", in->dst.reg);
            for (int i = 0; i < 16; i++) {
                if (!(in->reglist & (1 << i))) continue;
                int r = 15 - i;
                char c = r < 8 ? 'd' : 'a';
                if (sz == 4)   /* the 68000 stores the low word first in this mode */
                    fprintf(o, "a -= 4; ng_w16(a + 2, (uint16_t)CPU.%c[%d]); ng_w16(a, (uint16_t)(CPU.%c[%d] >> 16)); ",
                            c, r & 7, c, r & 7);
                else
                    fprintf(o, "a -= 2; ng_w16(a, (uint16_t)CPU.%c[%d]); ", c, r & 7);
            }
            fprintf(o, "CPU.a[%d] = a; ", in->dst.reg);
        } else {
            ea_addr(o, &in->dst, sz, "a");
            for (int i = 0; i < 16; i++) {
                if (!(in->reglist & (1 << i))) continue;
                fprintf(o, "ng_w%d(a, (%s)CPU.%c[%d]); a += %d; ", b, t, i < 8 ? 'd' : 'a', i & 7, step);
            }
        }
        break;
    }
    case OP_MOVEM_TO_REG: {
        int n = movem_count(in->reglist);
        fprintf(o, "ng_cycles += %d; ", n * (sz == 2 ? 4 : 8));
        if (in->src.mode == EA_POSTINC) fprintf(o, "uint32_t a = CPU.a[%d]; ", in->src.reg);
        else ea_addr(o, &in->src, sz, "a");
        for (int i = 0; i < 16; i++) {
            if (!(in->reglist & (1 << i))) continue;
            fprintf(o, "CPU.%c[%d] = %s; a += %d; ", i < 8 ? 'd' : 'a', i & 7,
                    sz == 2 ? "SX16(ng_r16(a))" : "ng_r32(a)", sz);
        }
        if (in->src.mode == EA_POSTINC) fprintf(o, "CPU.a[%d] = a; ", in->src.reg);
        break;
    }

    case OP_SCC: {
        char val[48];
        snprintf(val, sizeof(val), "(uint8_t)(%s ? 0xFF : 0x00)", k_cc[in->cond]);
        ea_write(o, &in->dst, 1, val, "ad");
        break;
    }

    case OP_BRA:
        branch(o, ctx, in, in->target);
        break;
    case OP_BCC:
        fprintf(o, "if (%s) { ", k_cc[in->cond]);
        branch(o, ctx, in, in->target);
        fprintf(o, "} ng_cycles += %d; ", (in->opcode & 0xFF) ? -2 : 2);
        break;
    case OP_DBCC:
        fprintf(o, "if (!%s) { uint16_t cnt = (uint16_t)(CPU.d[%d] - 1); ", k_cc[in->cond], in->dst.reg);
        write_dn(o, in->dst.reg, 2, "cnt");
        fprintf(o, "if (cnt != 0xFFFF) { ng_cycles -= 2; ");
        branch(o, ctx, in, in->target);
        fprintf(o, "} } ng_cycles += 2; ");
        break;
    case OP_BSR:
        fprintf(o, "ng_push32(0x%06Xu); ", next);
        exit_to(o, in->target);
        break;
    case OP_JSR:
        ea_addr(o, &in->src, 4, "t");
        fprintf(o, "ng_push32(0x%06Xu); CPU.pc = t; return; ", next);
        break;
    case OP_JMP:
        ea_addr(o, &in->src, 4, "t");
        fprintf(o, "CPU.pc = t; return; ");
        break;
    case OP_RTS: fprintf(o, "CPU.pc = ng_pop32(); return; "); break;
    case OP_RTR: fprintf(o, "ng_set_ccr(ng_pop16()); CPU.pc = ng_pop32(); return; "); break;
    case OP_RTE: fprintf(o, "uint16_t sr = ng_pop16(); CPU.pc = ng_pop32(); ng_set_sr(sr); return; "); break;
    case OP_TRAP: fprintf(o, "ng_exception(%d, 0x%06Xu); return; ", 32 + in->data, next); break;
    case OP_TRAPV: fprintf(o, "if (CPU.v) { ng_exception(7, 0x%06Xu); return; } ", next); break;
    case OP_STOP:
        fprintf(o, "ng_set_sr(0x%04Xu); CPU.stopped = 1; ", (unsigned)in->data & 0xFFFF);
        exit_to(o, next);
        break;
    case OP_RESET: fprintf(o, "ng_cycles += 128; "); break;
    case OP_NOP: break;
    default: fprintf(o, "/* unhandled */ CPU.pc = 0x%06Xu; return; ", in->addr); break;
    }
    fprintf(o, "}\n");
}
