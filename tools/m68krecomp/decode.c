/*
 * decode.c — 68000 instruction decoder (all 68000 opcodes, no 68010+).
 *
 * Encodings that the 68000 rejects (An with byte size, stores to PC-
 * relative or immediate operands, reserved modes) decode as OP_ILLEGAL,
 * which is what the CPU does with them: vector 4.
 */
#include "decode.h"
#include <string.h>

typedef struct {
    uint32_t pc;          /* address of the next extension word */
    read16_fn rd;
    int ok;
} cursor_t;

static uint16_t next16(cursor_t *c) {
    int ok = 1;
    uint16_t w = c->rd(c->pc, &ok);
    if (!ok) c->ok = 0;
    c->pc += 2;
    return w;
}

static uint32_t next32(cursor_t *c) {
    uint32_t hi = next16(c);
    return (hi << 16) | next16(c);
}

/* Decodes mode/reg into ea; returns 0 if the combination does not exist. */
static int ea_decode(cursor_t *c, int mode, int reg, int size, ea_t *ea) {
    memset(ea, 0, sizeof(*ea));
    ea->reg = reg;
    switch (mode) {
    case 0: ea->mode = EA_DN; return 1;
    case 1: ea->mode = EA_AN; return 1;
    case 2: ea->mode = EA_IND; return 1;
    case 3: ea->mode = EA_POSTINC; return 1;
    case 4: ea->mode = EA_PREDEC; return 1;
    case 5: ea->mode = EA_D16; ea->disp = (int16_t)next16(c); return 1;
    case 6: {
        uint16_t ext = next16(c);
        ea->mode = EA_IDX;
        ea->xreg = (ext >> 12) & 15;
        ea->xlong = (ext >> 11) & 1;
        ea->disp = (int8_t)(ext & 0xFF);
        return 1;
    }
    default:
        switch (reg) {
        case 0: ea->mode = EA_ABS; ea->value = (uint32_t)(int32_t)(int16_t)next16(c); return 1;
        case 1: ea->mode = EA_ABS; ea->value = next32(c); return 1;
        case 2: {
            uint32_t base = c->pc;
            ea->mode = EA_PCD16;
            ea->value = base + (uint32_t)(int32_t)(int16_t)next16(c);
            return 1;
        }
        case 3: {
            uint32_t base = c->pc;
            uint16_t ext = next16(c);
            ea->mode = EA_PCIDX;
            ea->xreg = (ext >> 12) & 15;
            ea->xlong = (ext >> 11) & 1;
            ea->value = base + (uint32_t)(int32_t)(int8_t)(ext & 0xFF);
            return 1;
        }
        case 4:
            ea->mode = EA_IMM;
            if (size == 1) ea->value = next16(c) & 0xFF;
            else if (size == 2) ea->value = next16(c);
            else ea->value = next32(c);
            return 1;
        default: return 0;
        }
    }
}

/* Operand classes, as in the 68000 manual. */
static int is_data_alterable(const ea_t *e) {
    return e->mode != EA_AN && e->mode != EA_PCD16 && e->mode != EA_PCIDX && e->mode != EA_IMM;
}
static int is_mem_alterable(const ea_t *e) {
    return is_data_alterable(e) && e->mode != EA_DN;
}
static int is_control(const ea_t *e) {
    return e->mode == EA_IND || e->mode == EA_D16 || e->mode == EA_IDX || e->mode == EA_ABS ||
           e->mode == EA_PCD16 || e->mode == EA_PCIDX;
}

static const int k_size2[4] = {1, 2, 4, 0};   /* 00 byte, 01 word, 10 long */

int m68k_decode(uint32_t addr, read16_fn rd, insn_t *in) {
    cursor_t c = {addr, rd, 1};
    memset(in, 0, sizeof(*in));
    in->addr = addr;
    uint16_t op = next16(&c);
    in->opcode = op;
    in->op = OP_ILLEGAL;
    int mode = (op >> 3) & 7, reg = op & 7;
    int rx = (op >> 9) & 7;
    int sz = k_size2[(op >> 6) & 3];

#define EA(dst, m, r, s) do { if (!ea_decode(&c, (m), (r), (s), (dst))) goto illegal; } while (0)

    switch (op >> 12) {
    case 0x0: {
        if ((op & 0x0138) == 0x0108) {
            /* MOVEP: 0000 ddd1 oo001 aaa */
            in->op = OP_MOVEP;
            in->size = (op & 0x0040) ? 4 : 2;
            in->data = (op >> 7) & 1;           /* 1 = register to memory */
            in->src.mode = EA_DN; in->src.reg = rx;
            in->dst.mode = EA_D16; in->dst.reg = reg; in->dst.disp = (int16_t)next16(&c);
            break;
        }
        if (op & 0x0100) {
            /* dynamic bit op: 0000 rrr1 tt eeeeee */
            static const op_t bops[4] = {OP_BTST, OP_BCHG, OP_BCLR, OP_BSET};
            in->op = bops[(op >> 6) & 3];
            in->src.mode = EA_DN; in->src.reg = rx;
            EA(&in->dst, mode, reg, 1);
            if (in->dst.mode == EA_AN) goto illegal;
            if (in->op != OP_BTST && !is_data_alterable(&in->dst)) goto illegal;
            in->size = in->dst.mode == EA_DN ? 4 : 1;
            break;
        }
        if ((op & 0x0F00) == 0x0800) {
            static const op_t bops[4] = {OP_BTST, OP_BCHG, OP_BCLR, OP_BSET};
            in->op = bops[(op >> 6) & 3];
            in->src.mode = EA_IMM; in->src.value = next16(&c) & 0xFF;
            EA(&in->dst, mode, reg, 1);
            if (in->dst.mode == EA_AN || in->dst.mode == EA_IMM) goto illegal;
            if (in->op != OP_BTST && !is_data_alterable(&in->dst)) goto illegal;
            in->size = in->dst.mode == EA_DN ? 4 : 1;
            break;
        }
        /* immediate ops */
        static const op_t iops[8] = {OP_ORI, OP_ANDI, OP_SUBI, OP_ADDI, OP_ILLEGAL, OP_EORI, OP_CMPI, OP_ILLEGAL};
        op_t o = iops[(op >> 9) & 7];
        if (o == OP_ILLEGAL) goto illegal;
        if ((op & 0x00FF) == 0x003C && (o == OP_ORI || o == OP_ANDI || o == OP_EORI)) {
            in->op = o == OP_ORI ? OP_ORI_CCR : o == OP_ANDI ? OP_ANDI_CCR : OP_EORI_CCR;
            in->src.mode = EA_IMM; in->src.value = next16(&c) & 0xFF; in->size = 1;
            break;
        }
        if ((op & 0x00FF) == 0x007C && (o == OP_ORI || o == OP_ANDI || o == OP_EORI)) {
            in->op = o == OP_ORI ? OP_ORI_SR : o == OP_ANDI ? OP_ANDI_SR : OP_EORI_SR;
            in->src.mode = EA_IMM; in->src.value = next16(&c); in->size = 2;
            break;
        }
        if (!sz) goto illegal;
        in->op = o; in->size = sz;
        EA(&in->src, 7, 4, sz);
        EA(&in->dst, mode, reg, sz);
        if (!is_data_alterable(&in->dst)) goto illegal;
        break;
    }
    case 0x1: case 0x2: case 0x3: {
        static const int msz[4] = {0, 1, 4, 2};
        int s = msz[op >> 12];
        EA(&in->src, mode, reg, s);
        if (s == 1 && in->src.mode == EA_AN) goto illegal;
        int dmode = (op >> 6) & 7;
        EA(&in->dst, dmode, rx, s);
        in->size = s;
        if (in->dst.mode == EA_AN) {
            if (s == 1) goto illegal;
            in->op = OP_MOVEA;
        } else {
            if (!is_data_alterable(&in->dst)) goto illegal;
            in->op = OP_MOVE;
        }
        break;
    }
    case 0x4: {
        if (op == 0x4AFC) goto illegal;
        if (op == 0x4E70) { in->op = OP_RESET; break; }
        if (op == 0x4E71) { in->op = OP_NOP; break; }
        if (op == 0x4E72) { in->op = OP_STOP; in->data = next16(&c); break; }
        if (op == 0x4E73) { in->op = OP_RTE; break; }
        if (op == 0x4E75) { in->op = OP_RTS; break; }
        if (op == 0x4E76) { in->op = OP_TRAPV; break; }
        if (op == 0x4E77) { in->op = OP_RTR; break; }
        if ((op & 0xFFF0) == 0x4E40) { in->op = OP_TRAP; in->data = op & 15; break; }
        if ((op & 0xFFF8) == 0x4E50) { in->op = OP_LINK; in->src.mode = EA_AN; in->src.reg = reg;
                                       in->data = (int16_t)next16(&c); break; }
        if ((op & 0xFFF8) == 0x4E58) { in->op = OP_UNLK; in->src.mode = EA_AN; in->src.reg = reg; break; }
        if ((op & 0xFFF8) == 0x4E60) { in->op = OP_MOVE_TO_USP; in->src.mode = EA_AN; in->src.reg = reg; break; }
        if ((op & 0xFFF8) == 0x4E68) { in->op = OP_MOVE_FROM_USP; in->dst.mode = EA_AN; in->dst.reg = reg; break; }
        if ((op & 0xFFC0) == 0x4E80 || (op & 0xFFC0) == 0x4EC0) {
            in->op = (op & 0x40) ? OP_JMP : OP_JSR;
            EA(&in->src, mode, reg, 4);
            if (!is_control(&in->src)) goto illegal;
            break;
        }
        if ((op & 0x01C0) == 0x01C0) {       /* LEA */
            in->op = OP_LEA; in->size = 4;
            EA(&in->src, mode, reg, 4);
            if (!is_control(&in->src)) goto illegal;
            in->dst.mode = EA_AN; in->dst.reg = rx;
            break;
        }
        if ((op & 0x01C0) == 0x0180) {       /* CHK.W */
            in->op = OP_CHK; in->size = 2;
            EA(&in->src, mode, reg, 2);
            if (in->src.mode == EA_AN) goto illegal;
            in->dst.mode = EA_DN; in->dst.reg = rx;
            break;
        }
        if ((op & 0x0100)) goto illegal;
        switch ((op >> 8) & 0xF) {
        case 0x0:
            if (sz) { in->op = OP_NEGX; in->size = sz; EA(&in->dst, mode, reg, sz);
                      if (!is_data_alterable(&in->dst)) goto illegal; break; }
            in->op = OP_MOVE_FROM_SR; in->size = 2; EA(&in->dst, mode, reg, 2);
            if (!is_data_alterable(&in->dst)) goto illegal;
            break;
        case 0x2:
            if (!sz) goto illegal;
            in->op = OP_CLR; in->size = sz; EA(&in->dst, mode, reg, sz);
            if (!is_data_alterable(&in->dst)) goto illegal;
            break;
        case 0x4:
            if (sz) { in->op = OP_NEG; in->size = sz; EA(&in->dst, mode, reg, sz);
                      if (!is_data_alterable(&in->dst)) goto illegal; break; }
            in->op = OP_MOVE_TO_CCR; in->size = 2; EA(&in->src, mode, reg, 2);
            if (in->src.mode == EA_AN) goto illegal;
            break;
        case 0x6:
            if (sz) { in->op = OP_NOT; in->size = sz; EA(&in->dst, mode, reg, sz);
                      if (!is_data_alterable(&in->dst)) goto illegal; break; }
            in->op = OP_MOVE_TO_SR; in->size = 2; EA(&in->src, mode, reg, 2);
            if (in->src.mode == EA_AN) goto illegal;
            break;
        case 0x8: {
            int k = (op >> 6) & 3;
            if (k == 0) { in->op = OP_NBCD; in->size = 1; EA(&in->dst, mode, reg, 1);
                          if (!is_data_alterable(&in->dst)) goto illegal; break; }
            if (k == 1) {
                if (mode == 0) { in->op = OP_SWAP; in->dst.mode = EA_DN; in->dst.reg = reg; in->size = 4; break; }
                in->op = OP_PEA; EA(&in->src, mode, reg, 4);
                if (!is_control(&in->src)) goto illegal;
                break;
            }
            if (mode == 0) { in->op = OP_EXT; in->size = k == 2 ? 2 : 4; in->dst.mode = EA_DN; in->dst.reg = reg; break; }
            in->op = OP_MOVEM_TO_MEM; in->size = k == 2 ? 2 : 4;
            in->reglist = next16(&c);
            EA(&in->dst, mode, reg, in->size);
            if (!(is_control(&in->dst) && in->dst.mode != EA_PCD16 && in->dst.mode != EA_PCIDX) &&
                in->dst.mode != EA_PREDEC) goto illegal;
            break;
        }
        case 0xA:
            if (sz) { in->op = OP_TST; in->size = sz; EA(&in->dst, mode, reg, sz);
                      if (!is_data_alterable(&in->dst)) goto illegal; break; }
            in->op = OP_TAS; in->size = 1; EA(&in->dst, mode, reg, 1);
            if (!is_data_alterable(&in->dst)) goto illegal;
            break;
        case 0xC: {
            int k = (op >> 6) & 3;
            if (k < 2) goto illegal;
            in->op = OP_MOVEM_TO_REG; in->size = k == 2 ? 2 : 4;
            in->reglist = next16(&c);
            EA(&in->src, mode, reg, in->size);
            if (!is_control(&in->src) && in->src.mode != EA_POSTINC) goto illegal;
            break;
        }
        default: goto illegal;
        }
        break;
    }
    case 0x5: {
        if ((op & 0xC0) == 0xC0) {
            in->cond = (op >> 8) & 15;
            if (mode == 1) {
                in->op = OP_DBCC; in->dst.mode = EA_DN; in->dst.reg = reg;
                uint32_t base = c.pc;
                in->target = base + (uint32_t)(int32_t)(int16_t)next16(&c);
                break;
            }
            in->op = OP_SCC; in->size = 1; EA(&in->dst, mode, reg, 1);
            if (!is_data_alterable(&in->dst)) goto illegal;
            break;
        }
        in->op = (op & 0x0100) ? OP_SUBQ : OP_ADDQ;
        in->data = rx ? rx : 8;
        in->size = sz;
        EA(&in->dst, mode, reg, sz);
        if (in->dst.mode == EA_AN && sz == 1) goto illegal;
        if (in->dst.mode != EA_AN && !is_data_alterable(&in->dst)) goto illegal;
        break;
    }
    case 0x6: {
        int cond = (op >> 8) & 15;
        uint32_t base = c.pc;
        int32_t d = (int8_t)(op & 0xFF);
        if (d == 0) d = (int16_t)next16(&c);
        in->target = base + (uint32_t)d;
        in->op = cond == 0 ? OP_BRA : cond == 1 ? OP_BSR : OP_BCC;
        in->cond = cond;
        break;
    }
    case 0x7:
        if (op & 0x0100) goto illegal;
        in->op = OP_MOVEQ; in->size = 4; in->data = (int8_t)(op & 0xFF);
        in->dst.mode = EA_DN; in->dst.reg = rx;
        break;
    case 0x8: case 0xC: {
        int opm = (op >> 6) & 7;
        int is_and = (op >> 12) == 0xC;
        if (opm == 3 || opm == 7) {
            in->op = is_and ? (opm == 3 ? OP_MULU : OP_MULS) : (opm == 3 ? OP_DIVU : OP_DIVS);
            in->size = 2;
            EA(&in->src, mode, reg, 2);
            if (in->src.mode == EA_AN) goto illegal;
            in->dst.mode = EA_DN; in->dst.reg = rx;
            break;
        }
        if (opm == 4 && mode < 2) {
            in->op = is_and ? OP_ABCD : OP_SBCD; in->size = 1;
            in->src.mode = mode ? EA_PREDEC : EA_DN; in->src.reg = reg;
            in->dst.mode = mode ? EA_PREDEC : EA_DN; in->dst.reg = rx;
            break;
        }
        if (is_and && (opm == 5 || opm == 6) && mode < 2) {
            in->op = OP_EXG;
            if (opm == 5 && mode == 0) { in->src.mode = EA_DN; in->dst.mode = EA_DN; }
            else if (opm == 5 && mode == 1) { in->src.mode = EA_AN; in->dst.mode = EA_AN; }
            else if (opm == 6 && mode == 1) { in->src.mode = EA_DN; in->dst.mode = EA_AN; }
            else goto illegal;
            in->src.reg = rx; in->dst.reg = reg;
            break;
        }
        in->op = is_and ? OP_AND : OP_OR;
        in->size = k_size2[opm & 3];
        if (opm < 4) {
            EA(&in->src, mode, reg, in->size);
            if (in->src.mode == EA_AN) goto illegal;
            in->dst.mode = EA_DN; in->dst.reg = rx;
        } else {
            in->src.mode = EA_DN; in->src.reg = rx;
            EA(&in->dst, mode, reg, in->size);
            if (!is_mem_alterable(&in->dst)) goto illegal;
        }
        break;
    }
    case 0x9: case 0xD: {
        int add = (op >> 12) == 0xD;
        int opm = (op >> 6) & 7;
        if (opm == 3 || opm == 7) {
            in->op = add ? OP_ADDA : OP_SUBA; in->size = opm == 3 ? 2 : 4;
            EA(&in->src, mode, reg, in->size);
            in->dst.mode = EA_AN; in->dst.reg = rx;
            break;
        }
        in->size = k_size2[opm & 3];
        if (opm >= 4 && mode < 2) {
            in->op = add ? OP_ADDX : OP_SUBX;
            in->src.mode = mode ? EA_PREDEC : EA_DN; in->src.reg = reg;
            in->dst.mode = mode ? EA_PREDEC : EA_DN; in->dst.reg = rx;
            break;
        }
        in->op = add ? OP_ADD : OP_SUB;
        if (opm < 4) {
            EA(&in->src, mode, reg, in->size);
            if (in->src.mode == EA_AN && in->size == 1) goto illegal;
            in->dst.mode = EA_DN; in->dst.reg = rx;
        } else {
            in->src.mode = EA_DN; in->src.reg = rx;
            EA(&in->dst, mode, reg, in->size);
            if (!is_mem_alterable(&in->dst)) goto illegal;
        }
        break;
    }
    case 0xA: in->op = OP_LINEA; break;
    case 0xB: {
        int opm = (op >> 6) & 7;
        if (opm == 3 || opm == 7) {
            in->op = OP_CMPA; in->size = opm == 3 ? 2 : 4;
            EA(&in->src, mode, reg, in->size);
            in->dst.mode = EA_AN; in->dst.reg = rx;
            break;
        }
        in->size = k_size2[opm & 3];
        if (opm < 3) {
            in->op = OP_CMP;
            EA(&in->src, mode, reg, in->size);
            if (in->src.mode == EA_AN && in->size == 1) goto illegal;
            in->dst.mode = EA_DN; in->dst.reg = rx;
            break;
        }
        if (mode == 1) {
            in->op = OP_CMPM;
            in->src.mode = EA_POSTINC; in->src.reg = reg;
            in->dst.mode = EA_POSTINC; in->dst.reg = rx;
            break;
        }
        in->op = OP_EOR;
        in->src.mode = EA_DN; in->src.reg = rx;
        EA(&in->dst, mode, reg, in->size);
        if (!is_data_alterable(&in->dst)) goto illegal;
        break;
    }
    case 0xE: {
        static const op_t right[4] = {OP_ASR, OP_LSR, OP_ROXR, OP_ROR};
        static const op_t left[4] = {OP_ASL, OP_LSL, OP_ROXL, OP_ROL};
        if ((op & 0xC0) == 0xC0) {
            if (op & 0x0800) goto illegal;
            int t = (op >> 9) & 3;
            in->op = (op & 0x0100) ? left[t] : right[t];
            in->size = 2; in->data = 1;
            EA(&in->dst, mode, reg, 2);
            if (!is_mem_alterable(&in->dst)) goto illegal;
            break;
        }
        int t = (op >> 3) & 3;
        in->op = (op & 0x0100) ? left[t] : right[t];
        in->size = sz;
        in->dst.mode = EA_DN; in->dst.reg = reg;
        if (op & 0x20) { in->shift_reg = 1; in->src.mode = EA_DN; in->src.reg = rx; }
        else in->data = rx ? rx : 8;
        break;
    }
    case 0xF: in->op = OP_LINEF; break;
    }
#undef EA
    if (!c.ok) return -1;
    in->len = (int)(c.pc - addr);
    return 0;

illegal:
    if (!c.ok) return -1;
    memset(&in->src, 0, sizeof(in->src));
    memset(&in->dst, 0, sizeof(in->dst));
    in->op = OP_ILLEGAL;
    in->size = 0;
    in->len = 2;
    return 0;
}

const char *op_name(op_t op) {
    static const char *names[OP_COUNT] = {
        "illegal", "linea", "linef", "ori", "andi", "subi", "addi", "eori", "cmpi",
        "ori_ccr", "andi_ccr", "eori_ccr", "ori_sr", "andi_sr", "eori_sr",
        "btst", "bchg", "bclr", "bset", "movep", "move", "movea",
        "move_from_sr", "move_to_ccr", "move_to_sr",
        "negx", "clr", "neg", "not", "ext", "nbcd", "swap", "pea",
        "movem_to_mem", "movem_to_reg", "tst", "tas",
        "trap", "link", "unlk", "move_to_usp", "move_from_usp",
        "reset", "nop", "stop", "rte", "rts", "trapv", "rtr",
        "jsr", "jmp", "chk", "lea", "addq", "subq", "scc", "dbcc",
        "bra", "bsr", "bcc", "moveq", "or", "divu", "divs", "sbcd",
        "sub", "suba", "subx", "cmp", "cmpa", "eor", "cmpm",
        "and", "mulu", "muls", "abcd", "exg", "add", "adda", "addx",
        "asl", "asr", "lsl", "lsr", "roxl", "roxr", "rol", "ror",
    };
    return op < OP_COUNT ? names[op] : "?";
}
