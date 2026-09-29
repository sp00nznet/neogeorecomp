/*
 * recomp.h — shared between the recompiler's analysis (recomp.c) and its
 * C emitter (emit.c).
 */
#ifndef M68KRECOMP_RECOMP_H
#define M68KRECOMP_RECOMP_H

#include <stdio.h>
#include "decode.h"

/* One routine being emitted: its instructions, sorted by address. */
typedef struct {
    uint32_t  seed;
    uint32_t *addrs;
    int       n;
} rc_ctx_t;

int  rc_in_routine(const rc_ctx_t *ctx, uint32_t addr);
int  rc_base_cycles(uint16_t opcode);
void rc_emit_insn(FILE *o, const rc_ctx_t *ctx, const insn_t *in);
int  rc_raw_main(int argc, char **argv);

#endif
