/*
 * func_table.c — Function dispatch table implementation.
 *
 * Uses a hash map for O(1) lookup of recompiled functions by their
 * original 68k address. The table is sized to handle the typical
 * Neo Geo P ROM function count (hundreds to low thousands).
 */

#include <neogeorecomp/func_table.h>
#include <neogeorecomp/neogeorecomp.h>
#include <neogeorecomp/platform.h>
#include <neogeorecomp/debug.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Hash table entry */
typedef struct func_entry {
    uint32_t addr;
    neogeo_func_t func;
    struct func_entry *next;  /* Collision chain */
} func_entry_t;

#define FUNC_TABLE_BUCKETS 4096  /* Power of 2 for fast modulo */

static func_entry_t *s_buckets[FUNC_TABLE_BUCKETS];
static uint32_t s_count = 0;

/* Simple hash for 68k addresses (always even, so shift right 1) */
static inline uint32_t addr_hash(uint32_t addr) {
    return (addr >> 1) & (FUNC_TABLE_BUCKETS - 1);
}

int func_table_init(void) {
    memset(s_buckets, 0, sizeof(s_buckets));
    s_count = 0;
    return 0;
}

void func_table_shutdown(void) {
    for (int i = 0; i < FUNC_TABLE_BUCKETS; i++) {
        func_entry_t *e = s_buckets[i];
        while (e) {
            func_entry_t *next = e->next;
            free(e);
            e = next;
        }
        s_buckets[i] = NULL;
    }
    s_count = 0;
}

void func_table_register(uint32_t addr, neogeo_func_t func) {
    uint32_t bucket = addr_hash(addr);

    /* Check for existing entry at this address */
    func_entry_t *e = s_buckets[bucket];
    while (e) {
        if (e->addr == addr) {
            e->func = func;  /* Overwrite */
            return;
        }
        e = e->next;
    }

    /* New entry */
    e = (func_entry_t *)malloc(sizeof(func_entry_t));
    if (!e) {
        fprintf(stderr, "[func_table] Allocation failed for addr $%06X\n", addr);
        return;
    }
    e->addr = addr;
    e->func = func;
    e->next = s_buckets[bucket];
    s_buckets[bucket] = e;
    s_count++;
}

static uint32_t s_call_count = 0;
static uint32_t s_miss_count = 0;

void func_table_call(uint32_t addr) {
    neogeo_func_t func = func_table_lookup(addr);
    s_call_count++;

    /* Time-based yield: if >20ms since last frame yield, force one.
     * This catches any spin-wait pattern regardless of bus access. */
    {
        static uint64_t s_last_yield = 0;
        uint64_t now = platform_get_ticks();
        if (s_last_yield == 0) s_last_yield = now;
        if (now - s_last_yield > 17) {  /* >17ms = ~60fps */
            s_last_yield = now;
            neogeo_frame_yield();
        }
    }

    /* Trace sprite-related functions */
    if (addr == 0x012202 || addr == 0x01348E || addr == 0x007F32 ||
        addr == 0x00805C || addr == 0x008172 || addr == 0x007E1C ||
        addr == 0x012256 || addr == 0x01225C || addr == 0x012270 ||
        addr == 0x00802A || addr == 0x013546) {
        fprintf(stderr, "[TRACE] $%06X (call #%u)\n", addr, s_call_count);
        fflush(stderr);
    }

    /* Early boot logging */
    if (s_call_count <= 5000 && (s_call_count <= 10 || s_call_count % 500 == 0)) {
        fprintf(stderr, "[call #%u] $%06X %s\n", s_call_count, addr, func ? "OK" : "MISS");
        fflush(stderr);
    }

    if (func) {
        debug_trace_call(addr, NULL);
        func();
    } else {
        s_miss_count++;
        if (s_miss_count <= 20) {
            fprintf(stderr, "[func_table] WARNING: No function at $%06X (call #%u)\n", addr, s_call_count);
            fflush(stderr);
        }
    }
}

neogeo_func_t func_table_lookup(uint32_t addr) {
    uint32_t bucket = addr_hash(addr);
    func_entry_t *e = s_buckets[bucket];
    while (e) {
        if (e->addr == addr) return e->func;
        e = e->next;
    }
    return NULL;
}

uint32_t func_table_count(void) {
    return s_count;
}
