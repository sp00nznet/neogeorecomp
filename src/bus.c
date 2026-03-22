/*
 * bus.c — Neo Geo memory bus implementation.
 *
 * Routes all 68k memory accesses to the correct hardware subsystem.
 * Handles big-endian to little-endian conversion transparently.
 *
 * The address decoder follows the Neo Geo memory map exactly:
 *   $000000-$0FFFFF  P ROM bank 1
 *   $100000-$1FFFFF  Work RAM (64 KB at $100000-$10FFFF, mirrored)
 *   $200000-$2FFFFF  P ROM bank 2 (bankswitchable)
 *   $300000-$3FFFFF  I/O and video registers
 *   $400000-$7FFFFF  Palette RAM (8 KB at $400000-$401FFF, mirrored)
 *   $800000-$BFFFFF  Memory card (not implemented)
 *   $C00000-$CFFFFF  System ROM / BIOS (128 KB, mirrored)
 *   $D00000-$DFFFFF  Backup RAM (64 KB, mirrored, MVS only)
 */

#include <neogeorecomp/bus.h>
#include <neogeorecomp/neogeorecomp.h>
#include <neogeorecomp/video.h>
#include <neogeorecomp/palette.h>
#include <neogeorecomp/io.h>
#include <neogeorecomp/timer.h>
#include <neogeorecomp/z80.h>
#include <neogeorecomp/debug.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ----- Internal State ----- */

static uint8_t *s_prom = NULL;       /* P ROM data (up to 8 MB) */
static uint32_t s_prom_size = 0;
static uint8_t *s_bios = NULL;       /* BIOS ROM (128 KB) */
static uint8_t *s_wram = NULL;       /* Work RAM (64 KB) */
static uint8_t *s_backup_ram = NULL; /* Backup RAM (64 KB, MVS only) */

static uint8_t s_prom_bank = 0;     /* Current P ROM bank for $200000-$2FFFFF */
static bool s_use_bios_vectors = true; /* Vector table source */
static bool s_sram_locked = true;   /* Backup RAM write protection */

/* ----- Initialization ----- */

int bus_init(void) {
    s_wram = (uint8_t *)calloc(1, 0x10000);  /* 64 KB */
    if (!s_wram) return -1;

    s_backup_ram = (uint8_t *)calloc(1, 0x10000);  /* 64 KB */
    if (!s_backup_ram) { free(s_wram); return -1; }

    s_prom_bank = 0;
    s_use_bios_vectors = true;
    s_sram_locked = true;

    return 0;
}

void bus_shutdown(void) {
    free(s_prom);    s_prom = NULL;
    free(s_bios);    s_bios = NULL;
    free(s_wram);    s_wram = NULL;
    free(s_backup_ram); s_backup_ram = NULL;
    s_prom_size = 0;
}

/* ----- ROM Loading ----- */

int bus_load_prom(const char *p1_path, const char *p2_path) {
    FILE *f = fopen(p1_path, "rb");
    if (!f) {
        fprintf(stderr, "[bus] Failed to open P ROM: %s\n", p1_path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long p1_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    long p2_size = 0;
    FILE *f2 = NULL;
    if (p2_path) {
        f2 = fopen(p2_path, "rb");
        if (f2) {
            fseek(f2, 0, SEEK_END);
            p2_size = ftell(f2);
            fseek(f2, 0, SEEK_SET);
        }
    }

    s_prom_size = (uint32_t)(p1_size + p2_size);
    s_prom = (uint8_t *)malloc(s_prom_size);
    if (!s_prom) { fclose(f); if (f2) fclose(f2); return -1; }

    /*
     * Neo Geo P ROM mapping for 2 MB ROMs:
     * The mapping is REVERSED — the second 1 MB goes to $000000,
     * the first 1 MB goes to $200000. This is handled here at load time
     * so that runtime address calculation stays simple.
     */
    if (p1_size == 0x200000 && !f2) {
        /* 2 MB single P ROM: swap halves */
        fread(s_prom + 0x100000, 1, 0x100000, f);  /* First MB -> offset 1 MB */
        fread(s_prom, 1, 0x100000, f);              /* Second MB -> offset 0 */
    } else {
        fread(s_prom, 1, (size_t)p1_size, f);
        if (f2) {
            fread(s_prom + p1_size, 1, (size_t)p2_size, f2);
        }
    }

    fclose(f);
    if (f2) fclose(f2);

    /*
     * Neo Geo P ROMs are stored byteswapped — every pair of bytes
     * is swapped in the file. Undo this so the 68000 code is in
     * the correct big-endian byte order.
     */
    for (uint32_t i = 0; i + 1 < s_prom_size; i += 2) {
        uint8_t tmp = s_prom[i];
        s_prom[i] = s_prom[i + 1];
        s_prom[i + 1] = tmp;
    }

    /* Verify: first bytes should now be a valid SSP (typically $0010F300) */
    if (s_prom_size >= 8) {
        uint32_t ssp = ((uint32_t)s_prom[0] << 24) | ((uint32_t)s_prom[1] << 16) |
                       ((uint32_t)s_prom[2] << 8)  | s_prom[3];
        uint32_t pc  = ((uint32_t)s_prom[4] << 24) | ((uint32_t)s_prom[5] << 16) |
                       ((uint32_t)s_prom[6] << 8)  | s_prom[7];
        printf("[bus] Loaded P ROM: %u bytes (SSP=$%08X PC=$%06X)\n", s_prom_size, ssp, pc);
    } else {
        printf("[bus] Loaded P ROM: %u bytes\n", s_prom_size);
    }
    return 0;
}

int bus_load_bios(const char *bios_path) {
    FILE *f = fopen(bios_path, "rb");
    if (!f) {
        fprintf(stderr, "[bus] Failed to open BIOS: %s\n", bios_path);
        return -1;
    }

    s_bios = (uint8_t *)malloc(0x20000);  /* 128 KB */
    if (!s_bios) { fclose(f); return -1; }

    fread(s_bios, 1, 0x20000, f);
    fclose(f);

    printf("[bus] Loaded BIOS: 128 KB\n");
    return 0;
}

/* ----- Big-endian read helpers ----- */

static inline uint16_t read16_be(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static inline uint32_t read32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}

static inline void write16_be(uint8_t *p, uint16_t val) {
    p[0] = (uint8_t)(val >> 8);
    p[1] = (uint8_t)val;
}

static inline void write32_be(uint8_t *p, uint32_t val) {
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)val;
}

/* ----- General Bus Access ----- */

uint8_t bus_read8(uint32_t addr) {
    addr &= 0xFFFFFF;  /* 24-bit address bus */
    debug_trace_mem_read(addr, 0, 1);

    if (addr < 0x100000) {
        /* P ROM bank 1 */
        if (s_prom && addr < s_prom_size) return s_prom[addr];
        return 0xFF;
    }
    if (addr < 0x200000) {
        /* Work RAM (mirrored every 64 KB) */
        return s_wram[addr & 0xFFFF];
    }
    if (addr < 0x300000) {
        /* P ROM bank 2 */
        uint32_t offset = 0x100000 + (uint32_t)s_prom_bank * 0x100000 + (addr & 0xFFFFF);
        if (s_prom && offset < s_prom_size) return s_prom[offset];
        return 0xFF;
    }
    if (addr < 0x400000) {
        /* I/O registers */
        switch (addr & 0xFE0001) {
            case 0x300000: return io_read_p1cnt();
            case 0x300001: return io_read_dipsw();
            case 0x320000: return z80_read_reply();
            case 0x320001: return io_read_status_a();
            case 0x340000: return io_read_p2cnt();
            case 0x380000: return io_read_status_b();
            case 0x380001: return 0xFF;  /* REG_POUTPUT */
            default:
                /* Video registers are word-accessed */
                break;
        }
        return 0xFF;
    }
    if (addr < 0x800000) {
        /* Palette RAM (mirrored) */
        return (uint8_t)(palette_read((addr & 0x1FFF) >> 1) >> ((addr & 1) ? 0 : 8));
    }
    if (addr < 0xC00000) {
        /* Memory card — not implemented */
        return 0xFF;
    }
    if (addr < 0xD00000) {
        /* BIOS (mirrored) */
        if (s_bios) return s_bios[addr & 0x1FFFF];
        return 0xFF;
    }
    if (addr < 0xE00000) {
        /* Backup RAM (mirrored) */
        return s_backup_ram[addr & 0xFFFF];
    }

    return 0xFF;
}

uint16_t bus_read16(uint32_t addr) {
    addr &= 0xFFFFFE;  /* Word-aligned */
    debug_trace_mem_read(addr, 0, 2);

    if (addr < 0x100000) {
        if (s_prom && addr + 1 < s_prom_size) return read16_be(s_prom + addr);
        return 0xFFFF;
    }
    if (addr < 0x200000) {
        /*
         * VBlank simulation hook:
         * When the game reads $100424 (frame-ready flag) and it's 0,
         * the game is spin-waiting for VBlank to fire. We yield to
         * the runtime to fire VBlank, render, present, and poll input.
         * This is how we simulate the 68000's interrupt-driven VBlank
         * without actual interrupts.
         */
        /*
         * VBlank simulation hooks:
         * On real hardware, VBlank fires as an interrupt ~60 times/sec.
         * In our recomp, we detect spin-wait patterns and yield to fire
         * VBlank, render, and present.
         *
         * Known spin-wait patterns:
         *  1. $100424: gameplay dispatcher waits for VBlank to set frame-ready flag
         *  2. $101700-$101B14: palette DMA ring buffer — game waits for VBlank
         *     to drain entries before writing new ones
         */
        {
            static bool s_in_yield = false;
            bool should_yield = false;

            if (addr == 0x100424) {
                uint16_t val = read16_be(s_wram + (addr & 0xFFFF));
                if (val == 0) should_yield = true;
                /* After the frame yield sets $100424 = 1 and the dispatcher
                 * reads it, also set $10041A = 1 to simulate demo timer.
                 * This must happen HERE (inside the dispatcher's loop) because
                 * USER clears $10041A at the start of each frame. */
                static int s_bus_frame = 0;
                if (val == 1) s_bus_frame++;
                if (val == 1 && s_bus_frame >= 120) {
                    write16_be(s_wram + 0x041A, 0x0001);
                }
            }

            /* Sprite upload flag: game spins waiting for VBlank to clear it */
            if (addr == 0x102224) {
                uint16_t val = read16_be(s_wram + (addr & 0xFFFF));
                if (val != 0) should_yield = true;
            }

            /* Palette ring buffer: when the game's palette write function
             * ($012036) spins waiting for a free slot, we process the
             * pending entry immediately instead of waiting for VBlank.
             * The entry format is: 4-byte dest pointer + 32 bytes of palette data.
             * We copy the palette data to its destination and clear the slot. */
            if (addr >= 0x101700 && addr < 0x101B20) {
                uint32_t slot_base = addr & ~0x3u;  /* Align to slot start */
                uint16_t hi = read16_be(s_wram + (slot_base & 0xFFFF));
                uint16_t lo = read16_be(s_wram + ((slot_base + 2) & 0xFFFF));
                uint32_t dest_ptr = ((uint32_t)hi << 16) | lo;
                if (dest_ptr != 0 && dest_ptr >= 0x400000 && dest_ptr < 0x402000) {
                    /* Valid palette destination — process the entry inline */
                    uint32_t src = slot_base + 4;
                    for (int i = 0; i < 16; i++) {
                        uint16_t color = read16_be(s_wram + ((src + i * 2) & 0xFFFF));
                        palette_write((dest_ptr - 0x400000) / 2 + i, color);
                    }
                    /* Clear the slot */
                    write16_be(s_wram + (slot_base & 0xFFFF), 0);
                    write16_be(s_wram + ((slot_base + 2) & 0xFFFF), 0);
                    return 0;  /* Slot is now free */
                }
            }

            if (should_yield && !s_in_yield) {
                s_in_yield = true;
                neogeo_frame_yield();
                s_in_yield = false;
                return read16_be(s_wram + (addr & 0xFFFF));
            }
        }
        return read16_be(s_wram + (addr & 0xFFFF));
    }
    if (addr < 0x300000) {
        uint32_t offset = 0x100000 + (uint32_t)s_prom_bank * 0x100000 + (addr & 0xFFFFF);
        if (s_prom && offset + 1 < s_prom_size) return read16_be(s_prom + offset);
        return 0xFFFF;
    }
    if (addr < 0x400000) {
        /* I/O and video registers */
        switch (addr) {
            case 0x300000: return (uint16_t)io_read_p1cnt() << 8 | io_read_dipsw();
            case 0x320000: return (uint16_t)z80_read_reply() << 8 | io_read_status_a();
            case 0x340000: return (uint16_t)io_read_p2cnt() << 8;
            case 0x380000: return (uint16_t)io_read_status_b() << 8;
            case 0x3C0000: return 0; /* VRAMADDR is write-only for practical purposes */
            case 0x3C0002: return video_read_vram();
            case 0x3C0006: return video_get_lspc_mode();
            default: return 0xFFFF;
        }
    }
    if (addr < 0x800000) {
        return palette_read((addr & 0x1FFF) >> 1);
    }
    if (addr < 0xC00000) {
        return 0xFFFF;  /* Memory card */
    }
    if (addr < 0xD00000) {
        if (s_bios) return read16_be(s_bios + (addr & 0x1FFFF));
        return 0xFFFF;
    }
    if (addr < 0xE00000) {
        return read16_be(s_backup_ram + (addr & 0xFFFF));
    }

    return 0xFFFF;
}

/*
 * Generic spin-wait detection: tracks total Work RAM reads without
 * any bus writes happening. When reads exceed a threshold without
 * any writes (indicating a pure polling loop), yield to VBlank.
 */
static int s_reads_without_write = 0;

uint32_t bus_read32(uint32_t addr) {
    /* Count reads in Work RAM range */
    if (addr >= 0x100000 && addr < 0x110000) {
        s_reads_without_write += 2;  /* bus_read32 = 2 word reads */
        if (s_reads_without_write >= 200) {
            static bool s_in32 = false;
            if (!s_in32) {
                s_in32 = true;
                neogeo_frame_yield();
                s_in32 = false;
            }
            s_reads_without_write = 0;
        }
    }

    return ((uint32_t)bus_read16(addr) << 16) | bus_read16(addr + 2);
}

void bus_write8(uint32_t addr, uint8_t val) {
    addr &= 0xFFFFFF;
    debug_trace_mem_write(addr, val, 1);

    if (addr < 0x100000) return;  /* P ROM is read-only */
    if (addr < 0x200000) {
        s_wram[addr & 0xFFFF] = val;
        return;
    }
    if (addr < 0x300000) {
        /* Write to $2xxxxx: P ROM bank select */
        bus_set_prom_bank(val);
        return;
    }
    if (addr < 0x400000) {
        /* I/O registers (byte writes) */
        switch (addr) {
            case 0x300001: io_kick_watchdog(); break;
            case 0x320000: z80_send_command(val); break;
            default:
                if ((addr & 0xFF0000) == 0x3A0000) {
                    io_write_sysctrl(addr);
                }
                break;
        }
        return;
    }
    if (addr < 0x800000) {
        /* Palette RAM — byte writes are unusual but handled */
        uint16_t offset = (uint16_t)((addr & 0x1FFF) >> 1);
        uint16_t cur = palette_read(offset);
        if (addr & 1) {
            palette_write(offset, (cur & 0xFF00) | val);
        } else {
            palette_write(offset, ((uint16_t)val << 8) | (cur & 0xFF));
        }
        return;
    }
    if (addr >= 0xD00000 && addr < 0xE00000) {
        if (!s_sram_locked) {
            s_backup_ram[addr & 0xFFFF] = val;
        }
        return;
    }
}

void bus_write16(uint32_t addr, uint16_t val) {
    addr &= 0xFFFFFE;
    debug_trace_mem_write(addr, val, 2);

    if (addr < 0x100000) return;  /* P ROM is read-only */
    if (addr < 0x200000) {
        s_reads_without_write = 0;
        /* Debug: trace writes to sprite attribute flag ($101B20 + n*$16) */
        if (addr == 0x101B20 && val != 0) {
            fprintf(stderr, "[SPR-FLAG] $%06X = $%04X !!\n", addr, val);
            fflush(stderr);
        }
        write16_be(s_wram + (addr & 0xFFFF), val);
        return;
    }
    if (addr < 0x300000) {
        bus_set_prom_bank((uint8_t)(val & 0xFF));
        return;
    }
    if (addr < 0x400000) {
        /* I/O and video registers */
        switch (addr) {
            case 0x300000: io_kick_watchdog(); break;
            case 0x320000: z80_send_command((uint8_t)(val >> 8)); break;
            case 0x3C0000: video_set_vram_addr(val); break;
            case 0x3C0002: video_write_vram(val); break;
            case 0x3C0004: video_set_vram_mod(val); break;
            case 0x3C0006: video_set_lspc_mode(val); break;
            case 0x3C0008: timer_set_reload((timer_get_counter() & 0x0000FFFF) | ((uint32_t)val << 16)); break;
            case 0x3C000A: timer_set_reload((timer_get_counter() & 0xFFFF0000) | val); break;
            case 0x3C000C: timer_irq_ack((uint8_t)(val & 0x07)); break;
            case 0x3C000E: timer_set_stop_on_border(val & 1); break;
            default:
                if ((addr & 0xFF0000) == 0x3A0000) {
                    io_write_sysctrl(addr);
                }
                break;
        }
        return;
    }
    if (addr < 0x800000) {
        palette_write((uint16_t)((addr & 0x1FFF) >> 1), val);
        return;
    }
    if (addr >= 0xD00000 && addr < 0xE00000) {
        if (!s_sram_locked) {
            write16_be(s_backup_ram + (addr & 0xFFFF), val);
        }
        return;
    }
}

void bus_write32(uint32_t addr, uint32_t val) {
    bus_write16(addr, (uint16_t)(val >> 16));
    bus_write16(addr + 2, (uint16_t)val);
}

/* ----- Fast Work RAM Access ----- */

uint8_t bus_wram_read8(uint32_t offset)   { return s_wram[offset & 0xFFFF]; }
uint16_t bus_wram_read16(uint32_t offset) { return read16_be(s_wram + (offset & 0xFFFF)); }
uint32_t bus_wram_read32(uint32_t offset) { return read32_be(s_wram + (offset & 0xFFFF)); }

void bus_wram_write8(uint32_t offset, uint8_t val)   { s_wram[offset & 0xFFFF] = val; }
void bus_wram_write16(uint32_t offset, uint16_t val) { write16_be(s_wram + (offset & 0xFFFF), val); }
void bus_wram_write32(uint32_t offset, uint32_t val) { write32_be(s_wram + (offset & 0xFFFF), val); }

/* ----- P ROM Banking ----- */

void bus_set_prom_bank(uint8_t bank) {
    s_prom_bank = bank;
}

uint8_t bus_get_prom_bank(void) {
    return s_prom_bank;
}

/* ----- Vector Table Swap ----- */

void bus_set_vector_source(bool use_bios) {
    s_use_bios_vectors = use_bios;
}

bool bus_get_vector_source(void) {
    return s_use_bios_vectors;
}

/* ----- Direct Pointers ----- */

const uint8_t *bus_get_prom_ptr(void)  { return s_prom; }
uint32_t bus_get_prom_size(void)       { return s_prom_size; }
uint8_t *bus_get_wram_ptr(void)        { return s_wram; }
