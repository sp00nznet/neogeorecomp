/*
 * z80.c — Z80 audio CPU stub implementation.
 *
 * TODO: Integrate a Z80 emulator core (e.g., z80ex, or a custom
 * interpreter). For now, this stubs out the communication interface
 * and does not execute any Z80 code.
 *
 * The Z80's role on the Neo Geo is straightforward: receive sound
 * commands from the 68k via NMI, then program the YM2610 accordingly.
 * Most games use variants of SNK's standard sound driver.
 *
 * Integration options:
 *   1. Interpreted Z80 emulation (most accurate)
 *   2. HLE of the sound driver (game-specific but simpler)
 *   3. Static recompilation of the Z80 code (ambitious but possible)
 *
 * We'll start with option 1 using a proven Z80 core.
 */

#include <neogeorecomp/z80.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ----- Internal State ----- */

static uint8_t *s_mrom = NULL;       /* M ROM (Z80 program) */
static uint32_t s_mrom_size = 0;
static uint8_t s_z80_ram[2048];      /* Z80 Work RAM (2 KB, $F800-$FFFF) */

static uint8_t s_cmd_latch = 0;      /* Command from 68k */
static uint8_t s_reply_latch = 0;    /* Reply to 68k */
static bool s_nmi_enabled = true;
static bool s_nmi_pending = false;

/* ----- Initialization ----- */

int z80_init(void) {
    memset(s_z80_ram, 0, sizeof(s_z80_ram));
    s_cmd_latch = 0;
    s_reply_latch = 0;
    s_nmi_enabled = true;
    s_nmi_pending = false;
    return 0;
}

void z80_shutdown(void) {
    free(s_mrom);
    s_mrom = NULL;
    s_mrom_size = 0;
}

/* ----- M ROM Loading ----- */

int z80_load_mrom(const char *mrom_path) {
    FILE *f = fopen(mrom_path, "rb");
    if (!f) {
        fprintf(stderr, "[z80] Failed to open M ROM: %s\n", mrom_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    s_mrom_size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    s_mrom = (uint8_t *)malloc(s_mrom_size);
    if (!s_mrom) { fclose(f); return -1; }
    fread(s_mrom, 1, s_mrom_size, f);
    fclose(f);
    printf("[z80] Loaded M ROM: %u bytes\n", s_mrom_size);
    return 0;
}

/* ----- Execution ----- */

void z80_execute(int cycles) {
    /*
     * TODO: Run the Z80 emulator core for the given number of cycles.
     * For now, this is a no-op — no audio processing occurs.
     */
    (void)cycles;

    /* If an NMI is pending and enabled, the Z80 would handle the command */
    if (s_nmi_pending && s_nmi_enabled) {
        /* In the real implementation, this would trigger the Z80's NMI handler.
         * The handler reads the command byte from port $00, processes it,
         * and writes a reply to port $0C. */
        s_reply_latch = s_cmd_latch | 0x80;  /* Stub: echo with bit 7 set */
        s_nmi_pending = false;
    }
}

/* ----- 68k <-> Z80 Communication ----- */

void z80_send_command(uint8_t cmd) {
    s_cmd_latch = cmd;
    if (s_nmi_enabled) {
        s_nmi_pending = true;
    }
}

uint8_t z80_read_reply(void) {
    return s_reply_latch;
}

/* ----- NMI Control ----- */

void z80_set_nmi_enabled(bool enabled) {
    s_nmi_enabled = enabled;
}

/* ----- Reset ----- */

void z80_reset(void) {
    memset(s_z80_ram, 0, sizeof(s_z80_ram));
    s_cmd_latch = 0;
    s_reply_latch = 0;
    s_nmi_enabled = true;
    s_nmi_pending = false;
}
