/*
 * io.c — the $300000-$3BFFFF I/O block: controllers, DIP switches, status
 * ports, the system latch, and the MVS calendar chip (uPD4990A).
 *
 *   $300000 P1   $300001 DIPs (r) / watchdog (w)   $300081 test/slot type
 *   $320000 Z80 reply (r) / command (w)            $320001 coins + RTC
 *   $340000 P2   $380000 start/select, card, MVS/AES
 *   $380001-$3800E1 (w, odd) output latches; $380051 = RTC data/clock/strobe
 *   $3A0001-$3A001F (w, odd) system latch: A1-A3 pick the bit, A4 is its value
 *
 * All button bits are active-low on the bus; ng_input_t is active-high.
 */
#include "ng_internal.h"
#include <neogeorecomp/bus.h>
#include <string.h>
#include <time.h>

static ng_input_t s_in;
static int s_mvs;

void io_set_inputs(const ng_input_t *in) { s_in = *in; }

/* ---- uPD4990A calendar ----
 * Serial mode: each rising CLK edge shifts DATA IN into the 4-bit command
 * register and, in shift mode, through the 48-bit time register (LSB out
 * first). A rising STB executes the command. TP is a square wave the BIOS
 * polls; its frequency is set by commands 4-B. */
static struct {
    int      din, clk, stb;
    uint8_t  cmd_shift, mode;
    uint64_t reg;            /* sec, min, hour, day (BCD), wday, month, year (BCD) */
    int64_t  tp_half;        /* half-period of TP in 68k clocks */
} s_rtc;

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static uint64_t rtc_now(void) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    return (uint64_t)bcd(tmv.tm_sec) | ((uint64_t)bcd(tmv.tm_min) << 8) |
           ((uint64_t)bcd(tmv.tm_hour) << 16) | ((uint64_t)bcd(tmv.tm_mday) << 24) |
           ((uint64_t)(tmv.tm_wday & 0xF) << 32) | ((uint64_t)((tmv.tm_mon + 1) & 0xF) << 36) |
           ((uint64_t)bcd(tmv.tm_year % 100) << 40);
}

static void rtc_exec(uint8_t c) {
    static const int tp_hz[4] = {64, 256, 2048, 4096};
    static const int tp_sec[4] = {1, 10, 30, 60};
    s_rtc.mode = c;
    if (c == 3) s_rtc.reg = rtc_now();
    else if (c >= 4 && c <= 7) s_rtc.tp_half = 12000000 / tp_hz[c - 4] / 2;
    else if (c >= 8 && c <= 0xB) s_rtc.tp_half = (int64_t)12000000 * tp_sec[c - 8] / 2;
}

static int rtc_tp(void) { return (int)((ng_cycles / s_rtc.tp_half) & 1); }

static int rtc_data_out(void) {
    if (s_rtc.mode == 1) return (int)(s_rtc.reg & 1);
    return (int)((ng_cycles / 6000000) & 1);   /* 1 Hz in the other modes */
}

static void rtc_write(uint8_t v) {
    int din = v & 1, clk = (v >> 1) & 1, stb = (v >> 2) & 1;
    s_rtc.din = din;
    if (clk && !s_rtc.clk) {
        s_rtc.cmd_shift = (uint8_t)((s_rtc.cmd_shift >> 1) | (din << 3));
        if (s_rtc.mode == 1)
            s_rtc.reg = (s_rtc.reg >> 1) | ((uint64_t)din << 47);
    }
    if (stb && !s_rtc.stb) rtc_exec(s_rtc.cmd_shift & 0xF);
    s_rtc.clk = clk; s_rtc.stb = stb;
}

void io_reset(int mvs) {
    s_mvs = mvs;
    memset(&s_in, 0, sizeof(s_in));
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.tp_half = 12000000 / 64 / 2;
    s_rtc.reg = rtc_now();
}

/* ---- reads ---- */

static uint8_t status_a(void) {
    uint8_t v = 0x3F;
    if (s_in.coin1) v &= (uint8_t)~0x01;
    if (s_in.coin2) v &= (uint8_t)~0x02;
    if (s_in.service) v &= (uint8_t)~0x04;
    if (rtc_tp()) v |= 0x40;
    if (rtc_data_out()) v |= 0x80;
    return v;
}

static uint8_t status_b(void) {
    uint8_t v = 0x7F;                 /* bits 4-6: no memory card */
    if (s_in.start1) v &= (uint8_t)~0x01;
    if (s_in.select1) v &= (uint8_t)~0x02;
    if (s_in.start2) v &= (uint8_t)~0x04;
    if (s_in.select2) v &= (uint8_t)~0x08;
    if (s_mvs) v |= 0x80;
    return v;
}

uint8_t io_read8(uint32_t a) {
    switch (a & 0xFE0000) {
    case 0x300000:
        if (!(a & 1)) return (uint8_t)~s_in.p1;
        if (a & 0x80) return (uint8_t)(0x3F | (s_in.test ? 0 : 0x80));   /* 1-slot board */
        return 0xFF;                                                     /* DIPs all off */
    case 0x320000:
        if (!(a & 1)) return audio_reply();
        return status_a();
    case 0x340000:
        return (a & 1) ? 0xFF : (uint8_t)~s_in.p2;
    case 0x380000:
        return (a & 1) ? 0xFF : status_b();
    default:
        return 0xFF;
    }
}

/* ---- writes ---- */

static void system_latch(uint32_t a) {
    int bit = (a >> 4) & 1;
    switch ((a >> 1) & 7) {
    case 0: break;                                   /* shadow */
    case 1: bus_select_bios_vectors(!bit); break;    /* SWPBIOS / SWPROM */
    case 2: case 3: case 4: break;                   /* memory card control */
    case 5: video_set_fix_bios(!bit); audio_select_bios_rom(!bit); break;  /* BRDFIX / CRTFIX */
    case 6: bus_set_backup_lock(!bit); break;        /* SRAMLOCK / SRAMUNLOCK */
    case 7: pal_select_bank(bit ? 0 : 1); break;     /* PALBANK1 / PALBANK0 */
    }
}

void io_write8(uint32_t a, uint8_t v) {
    switch (a & 0xFE0000) {
    case 0x300000: break;                            /* watchdog kick */
    case 0x320000: if (!(a & 1)) audio_command(v); break;
    case 0x380000:
        if ((a & 0x7F) == 0x51) rtc_write(v);        /* other latches: LEDs, coin counters */
        break;
    case 0x3A0000: if (a & 1) system_latch(a); break;
    default: break;
    }
}
