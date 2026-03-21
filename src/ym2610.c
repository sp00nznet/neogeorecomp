/*
 * ym2610.c — YM2610 (OPNB) sound chip stub implementation.
 *
 * TODO: Integrate ymfm or write a custom YM2610 emulation core.
 * For now, this provides the register interface and generates silence.
 *
 * The YM2610 has 14 channels:
 *   4 FM channels (4 operators each, frequency modulation synthesis)
 *   3 SSG channels (square wave + noise, via AY-3-8910 compatible core)
 *   6 ADPCM-A channels (short sample playback, typically SFX)
 *   1 ADPCM-B channel (longer streaming sample, typically music/voice)
 */

#include <neogeorecomp/ym2610.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ----- Internal State ----- */

static uint8_t s_regs_a[256];      /* Register bank A (SSG, ADPCM-B, FM ch1-2) */
static uint8_t s_regs_b[256];      /* Register bank B (ADPCM-A, FM ch3-4) */
static uint8_t s_addr_a = 0;       /* Latched address for bank A */
static uint8_t s_addr_b = 0;       /* Latched address for bank B */

static uint8_t *s_vrom = NULL;     /* ADPCM sample data (V ROMs) */
static uint32_t s_vrom_size = 0;
static int s_sample_rate = 48000;

/* ----- Initialization ----- */

int ym2610_init(int sample_rate) {
    memset(s_regs_a, 0, sizeof(s_regs_a));
    memset(s_regs_b, 0, sizeof(s_regs_b));
    s_addr_a = 0;
    s_addr_b = 0;
    s_sample_rate = sample_rate;
    printf("[ym2610] Initialized at %d Hz\n", sample_rate);
    return 0;
}

void ym2610_shutdown(void) {
    free(s_vrom);
    s_vrom = NULL;
    s_vrom_size = 0;
}

/* ----- V ROM Loading ----- */

int ym2610_load_vrom(const char **vrom_paths, int num_vroms) {
    /* Calculate total size */
    uint32_t total = 0;
    for (int i = 0; i < num_vroms; i++) {
        FILE *f = fopen(vrom_paths[i], "rb");
        if (!f) {
            fprintf(stderr, "[ym2610] Failed to open V ROM: %s\n", vrom_paths[i]);
            return -1;
        }
        fseek(f, 0, SEEK_END);
        total += (uint32_t)ftell(f);
        fclose(f);
    }

    s_vrom = (uint8_t *)malloc(total);
    if (!s_vrom) return -1;
    s_vrom_size = total;

    /* Load sequentially */
    uint32_t offset = 0;
    for (int i = 0; i < num_vroms; i++) {
        FILE *f = fopen(vrom_paths[i], "rb");
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        fread(s_vrom + offset, 1, (size_t)size, f);
        fclose(f);
        offset += (uint32_t)size;
    }

    printf("[ym2610] Loaded %d V ROMs: %u bytes total\n", num_vroms, s_vrom_size);
    return 0;
}

/* ----- Register Access ----- */

void ym2610_write(uint8_t port, uint8_t addr, uint8_t data) {
    /*
     * port 0: Address latch for bank A (Z80 port $04)
     * port 1: Data write for bank A (Z80 port $05)
     * port 2: Address latch for bank B (Z80 port $06)
     * port 3: Data write for bank B (Z80 port $07)
     */
    switch (port) {
        case 0: s_addr_a = addr; break;
        case 1: s_regs_a[s_addr_a] = data; break;
        case 2: s_addr_b = addr; break;
        case 3: s_regs_b[s_addr_b] = data; break;
    }
}

uint8_t ym2610_read(uint8_t port) {
    switch (port) {
        case 0: return 0x00;  /* Status register A (busy + timer flags) */
        case 2: return 0x00;  /* Status register B */
        default: return 0xFF;
    }
}

/* ----- Audio Generation ----- */

void ym2610_generate(int16_t *buffer, int num_samples) {
    /* TODO: Actual audio synthesis. For now, output silence. */
    memset(buffer, 0, (size_t)num_samples * 2 * sizeof(int16_t));
}

/* ----- Timer ----- */

void ym2610_tick_timers(int cycles) {
    /* TODO: Implement timer A and timer B countdown and IRQ generation */
    (void)cycles;
}

int ym2610_irq_pending(void) {
    return 0;
}

/* ----- Reset ----- */

void ym2610_reset(void) {
    memset(s_regs_a, 0, sizeof(s_regs_a));
    memset(s_regs_b, 0, sizeof(s_regs_b));
    s_addr_a = 0;
    s_addr_b = 0;
}
