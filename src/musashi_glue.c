/*
 * musashi_glue.c — the few Musashi internals the runtime needs, kept in
 * one file so m68kcpu.h's macro namespace (REG_PC, FLAG_Z, ...) stays out
 * of the rest of the runtime.
 */
#include "m68kcpu.h"
#include "musashi_glue.h"

int  mus_stopped(void) { return CPU_STOPPED != 0; }
void mus_clear_stopped(void) { CPU_STOPPED = 0; }
void mus_set_stopped(void) { CPU_STOPPED = STOP_LEVEL_STOP; }
int  mus_base_cycles(unsigned opcode) { return CYC_INSTRUCTION[opcode & 0xFFFF]; }
