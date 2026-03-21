/*
 * func_table.h — Function dispatch table for recompiled code.
 *
 * Every recompiled 68k function is registered at its original address.
 * When recompiled code needs to call a subroutine (JSR/BSR), it calls
 * func_table_call() with the target address, which looks up and invokes
 * the corresponding C function.
 *
 * This table is the bridge between the original program's control flow
 * and the recompiled native code.
 *
 * Usage:
 *   // In the game's main.c, register all recompiled functions:
 *   func_table_register(0x000200, func_000200);
 *   func_table_register(0x000400, func_000400);
 *   ...
 *
 *   // In recompiled code, a JSR becomes:
 *   func_table_call(0x000400);  // calls func_000400()
 *
 * Functions take no arguments and return void — they operate on the
 * global CPU context (g_m68k) and the bus.
 */

#ifndef NEOGEORECOMP_FUNC_TABLE_H
#define NEOGEORECOMP_FUNC_TABLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All recompiled functions have this signature. */
typedef void (*neogeo_func_t)(void);

/*
 * Initialize the function table.
 * Must be called before registering any functions.
 */
int func_table_init(void);

/* Free the function table. */
void func_table_shutdown(void);

/*
 * Register a recompiled function at its original 68k address.
 *
 * addr: The original 68k address of the function's first instruction.
 *       Must be even (68k instructions are always word-aligned).
 * func: Pointer to the recompiled C function.
 *
 * Overwrites any previously registered function at the same address.
 */
void func_table_register(uint32_t addr, neogeo_func_t func);

/*
 * Call the recompiled function registered at the given 68k address.
 *
 * This is the equivalent of a 68k JSR/BSR instruction. If no function
 * is registered at the address, a warning is logged and the call is
 * treated as a no-op (in debug mode, this triggers a breakpoint).
 */
void func_table_call(uint32_t addr);

/*
 * Check if a function is registered at the given address.
 * Returns the function pointer, or NULL if not registered.
 */
neogeo_func_t func_table_lookup(uint32_t addr);

/*
 * Get the total number of registered functions.
 * Useful for progress tracking during development.
 */
uint32_t func_table_count(void);

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_FUNC_TABLE_H */
