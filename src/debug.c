/*
 * debug.c — Debug facilities implementation.
 *
 * Only compiled when NEOGEORECOMP_DEBUG is defined.
 * Provides instruction tracing, memory access logging,
 * breakpoints, and state inspection.
 */

#include <neogeorecomp/debug.h>

#ifdef NEOGEORECOMP_DEBUG

#include <neogeorecomp/m68k.h>
#include <neogeorecomp/video.h>
#include <neogeorecomp/palette.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ----- Internal State ----- */

static bool s_trace_enabled = false;
static FILE *s_trace_file = NULL;

#define MAX_BREAKPOINTS 256
static uint32_t s_breakpoints[MAX_BREAKPOINTS];
static int s_num_breakpoints = 0;

/* ----- Initialization ----- */

int debug_init(void) {
    s_trace_enabled = false;
    s_trace_file = NULL;
    s_num_breakpoints = 0;
    printf("[debug] Debug mode enabled\n");
    return 0;
}

void debug_shutdown(void) {
    if (s_trace_file) {
        fclose(s_trace_file);
        s_trace_file = NULL;
    }
}

/* ----- Tracing ----- */

void debug_trace_enable(bool enabled) {
    s_trace_enabled = enabled;
    if (enabled && !s_trace_file) {
        s_trace_file = fopen("neogeorecomp_trace.log", "w");
    }
}

void debug_trace_call(uint32_t addr, const char *name) {
    if (!s_trace_enabled) return;
    if (s_trace_file) {
        if (name) {
            fprintf(s_trace_file, "CALL $%06X (%s)\n", addr, name);
        } else {
            fprintf(s_trace_file, "CALL $%06X\n", addr);
        }
    }
}

void debug_trace_mem_read(uint32_t addr, uint32_t val, int size) {
    if (!s_trace_enabled) return;
    if (s_trace_file) {
        fprintf(s_trace_file, "READ.%c $%06X = $%0*X\n",
                size == 1 ? 'B' : size == 2 ? 'W' : 'L',
                addr, size * 2, val);
    }
}

void debug_trace_mem_write(uint32_t addr, uint32_t val, int size) {
    if (!s_trace_enabled) return;
    if (s_trace_file) {
        fprintf(s_trace_file, "WRITE.%c $%06X = $%0*X\n",
                size == 1 ? 'B' : size == 2 ? 'W' : 'L',
                addr, size * 2, val);
    }
}

/* ----- Breakpoints ----- */

void debug_add_breakpoint(uint32_t addr) {
    if (s_num_breakpoints < MAX_BREAKPOINTS) {
        s_breakpoints[s_num_breakpoints++] = addr;
    }
}

void debug_remove_breakpoint(uint32_t addr) {
    for (int i = 0; i < s_num_breakpoints; i++) {
        if (s_breakpoints[i] == addr) {
            s_breakpoints[i] = s_breakpoints[--s_num_breakpoints];
            return;
        }
    }
}

bool debug_check_breakpoint(uint32_t addr) {
    for (int i = 0; i < s_num_breakpoints; i++) {
        if (s_breakpoints[i] == addr) return true;
    }
    return false;
}

/* ----- State Inspection ----- */

void debug_dump_cpu_state(void) {
    printf("=== 68000 CPU State ===\n");
    printf("PC=$%06X  SR=$%04X\n", g_m68k.pc, m68k_get_sr());
    for (int i = 0; i < 8; i++) {
        printf("D%d=$%08X  A%d=$%08X\n", i, g_m68k.d[i], i, g_m68k.a[i]);
    }
    printf("USP=$%08X  SSP=$%08X\n", g_m68k.usp, g_m68k.ssp);
    printf("Flags: %c%c%c%c%c\n",
           g_m68k.flag_x ? 'X' : '-',
           g_m68k.flag_n ? 'N' : '-',
           g_m68k.flag_z ? 'Z' : '-',
           g_m68k.flag_v ? 'V' : '-',
           g_m68k.flag_c ? 'C' : '-');
}

void debug_dump_vram(uint16_t start, uint16_t count) {
    const uint16_t *vram = video_get_vram_ptr();
    printf("=== VRAM $%04X - $%04X ===\n", start, start + count - 1);
    for (uint16_t i = 0; i < count; i++) {
        if (i % 8 == 0) printf("$%04X: ", start + i);
        printf("%04X ", vram[start + i]);
        if (i % 8 == 7) printf("\n");
    }
    if (count % 8 != 0) printf("\n");
}

void debug_dump_palette(int bank) {
    printf("=== Palette Bank %d ===\n", bank);
    /* Just dump first few palettes */
    for (int pal = 0; pal < 4; pal++) {
        printf("Palette %d: ", pal);
        for (int col = 0; col < 16; col++) {
            printf("%04X ", 0);  /* TODO: read from palette RAM */
        }
        printf("\n");
    }
}

/* ----- Logging ----- */

void debug_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

#endif /* NEOGEORECOMP_DEBUG */
