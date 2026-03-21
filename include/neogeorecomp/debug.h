/*
 * debug.h — Debug and diagnostic facilities.
 *
 * Compile-time gated via NEOGEORECOMP_DEBUG. When disabled,
 * all debug calls compile to no-ops with zero overhead.
 *
 * Features:
 *   - Instruction tracing (log every recompiled function call)
 *   - Memory access tracing (log bus reads/writes)
 *   - Breakpoints on 68k addresses
 *   - VRAM inspection
 *   - CPU state dumping
 */

#ifndef NEOGEORECOMP_DEBUG_H
#define NEOGEORECOMP_DEBUG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NEOGEORECOMP_DEBUG

int debug_init(void);
void debug_shutdown(void);

/* ----- Tracing ----- */

void debug_trace_enable(bool enabled);
void debug_trace_call(uint32_t addr, const char *name);
void debug_trace_mem_read(uint32_t addr, uint32_t val, int size);
void debug_trace_mem_write(uint32_t addr, uint32_t val, int size);

/* ----- Breakpoints ----- */

void debug_add_breakpoint(uint32_t addr);
void debug_remove_breakpoint(uint32_t addr);
bool debug_check_breakpoint(uint32_t addr);

/* ----- State Inspection ----- */

void debug_dump_cpu_state(void);
void debug_dump_vram(uint16_t start, uint16_t count);
void debug_dump_palette(int bank);

/* ----- Logging ----- */

void debug_log(const char *fmt, ...);

#else /* !NEOGEORECOMP_DEBUG */

/* No-op stubs when debug is disabled */
#define debug_init() (0)
#define debug_shutdown() ((void)0)
#define debug_trace_enable(e) ((void)0)
#define debug_trace_call(a, n) ((void)0)
#define debug_trace_mem_read(a, v, s) ((void)0)
#define debug_trace_mem_write(a, v, s) ((void)0)
#define debug_add_breakpoint(a) ((void)0)
#define debug_remove_breakpoint(a) ((void)0)
#define debug_check_breakpoint(a) (false)
#define debug_dump_cpu_state() ((void)0)
#define debug_dump_vram(s, c) ((void)0)
#define debug_dump_palette(b) ((void)0)
#define debug_log(...) ((void)0)

#endif /* NEOGEORECOMP_DEBUG */

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_DEBUG_H */
