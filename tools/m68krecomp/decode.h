/*
 * decode.h — 68000 instruction decoder for the recompiler.
 *
 * Decodes one instruction into an operation, a size and up to two
 * effective-address operands with their extension words resolved. PC-
 * relative operands are turned into absolute addresses here, so the
 * emitter never has to think about where the PC was.
 */
#ifndef M68KRECOMP_DECODE_H
#define M68KRECOMP_DECODE_H

#include <stdint.h>

typedef enum {
    EA_NONE, EA_DN, EA_AN, EA_IND, EA_POSTINC, EA_PREDEC, EA_D16, EA_IDX,
    EA_ABS, EA_PCD16, EA_PCIDX, EA_IMM,
} ea_mode_t;

typedef struct {
    ea_mode_t mode;
    int      reg;        /* Dn/An number; base An for (d16,An)/(d8,An,Xn) */
    int32_t  disp;       /* displacement */
    int      xreg;       /* index register 0-15 (8-15 = A0-A7) */
    int      xlong;      /* index is .l */
    uint32_t value;      /* immediate, absolute address, or PC-relative base */
} ea_t;

typedef enum {
    OP_ILLEGAL, OP_LINEA, OP_LINEF,
    OP_ORI, OP_ANDI, OP_SUBI, OP_ADDI, OP_EORI, OP_CMPI,
    OP_ORI_CCR, OP_ANDI_CCR, OP_EORI_CCR, OP_ORI_SR, OP_ANDI_SR, OP_EORI_SR,
    OP_BTST, OP_BCHG, OP_BCLR, OP_BSET, OP_MOVEP,
    OP_MOVE, OP_MOVEA,
    OP_MOVE_FROM_SR, OP_MOVE_TO_CCR, OP_MOVE_TO_SR,
    OP_NEGX, OP_CLR, OP_NEG, OP_NOT, OP_EXT, OP_NBCD, OP_SWAP, OP_PEA,
    OP_MOVEM_TO_MEM, OP_MOVEM_TO_REG, OP_TST, OP_TAS,
    OP_TRAP, OP_LINK, OP_UNLK, OP_MOVE_TO_USP, OP_MOVE_FROM_USP,
    OP_RESET, OP_NOP, OP_STOP, OP_RTE, OP_RTS, OP_TRAPV, OP_RTR,
    OP_JSR, OP_JMP, OP_CHK, OP_LEA,
    OP_ADDQ, OP_SUBQ, OP_SCC, OP_DBCC,
    OP_BRA, OP_BSR, OP_BCC,
    OP_MOVEQ,
    OP_OR, OP_DIVU, OP_DIVS, OP_SBCD,
    OP_SUB, OP_SUBA, OP_SUBX,
    OP_CMP, OP_CMPA, OP_EOR, OP_CMPM,
    OP_AND, OP_MULU, OP_MULS, OP_ABCD, OP_EXG,
    OP_ADD, OP_ADDA, OP_ADDX,
    OP_ASL, OP_ASR, OP_LSL, OP_LSR, OP_ROXL, OP_ROXR, OP_ROL, OP_ROR,
    OP_COUNT
} op_t;

typedef struct {
    uint32_t addr;
    int      len;          /* bytes */
    uint16_t opcode;
    op_t     op;
    int      size;         /* 1, 2, 4; 0 when not applicable */
    ea_t     src, dst;
    int      cond;         /* Bcc/Scc/DBcc condition 0-15 */
    uint32_t target;       /* branch target (Bcc/BRA/BSR/DBcc) */
    uint16_t reglist;      /* MOVEM mask as encoded */
    int      shift_reg;    /* shifts: count comes from Dn (src.reg) */
    int      data;         /* ADDQ/SUBQ/MOVEQ/TRAP/shift immediate, STOP/LINK operand */
} insn_t;

/* Reads a big-endian word of the image; returns 0 when outside it. */
typedef uint16_t (*read16_fn)(uint32_t addr, int *ok);

/* Returns 0 on success, -1 if the words run off the image. */
int m68k_decode(uint32_t addr, read16_fn rd, insn_t *out);

const char *op_name(op_t op);

#endif
