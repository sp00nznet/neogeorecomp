/*
 * ng_internal.h — interfaces between the runtime's hardware modules.
 * Not installed; game hosts use <neogeorecomp/neogeo.h>.
 *
 * Timing model (docs/architecture.md, "Timing"): one master count,
 * ng_cycles, in 68000 clocks (12 MHz). A frame is 264 lines of 768
 * clocks. Everything else (LSPC timer, Z80, YM2610 timers, the RTC)
 * is derived from it, so a run is fully deterministic for a given input.
 */
#ifndef NG_INTERNAL_H
#define NG_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <neogeorecomp/neogeo.h>

#define NG_CLOCKS_PER_LINE   768
#define NG_LINES_PER_FRAME   264
#define NG_CLOCKS_PER_FRAME  (NG_CLOCKS_PER_LINE * NG_LINES_PER_FRAME)
#define NG_FIRST_VISIBLE     16
#define NG_VBLANK_LINE       240
#define NG_VBLANK_HPOS       0x11F       /* pixel within line 240 where IRQ1 fires */
#define NG_SCREEN_W          320
#define NG_SCREEN_H          224

/* ---- interrupts (level 1 = VBlank, 2 = LSPC timer, 3 = cold reset) ---- */
extern uint8_t ng_irq_pending;           /* bit n-1 = level n pending */
static inline int ng_irq_level(void) {
    return (ng_irq_pending & 4) ? 3 : (ng_irq_pending & 2) ? 2 : (ng_irq_pending & 1) ? 1 : 0;
}

/* ---- scheduler (neogeo.c) ---- */
extern int64_t ng_frame_start;           /* ng_cycles at line 0 of the current frame */
static inline int ng_vpos(void) {
    extern int64_t ng_cycles;
    return (int)((ng_cycles - ng_frame_start) / NG_CLOCKS_PER_LINE);
}

/* ---- LSPC: VRAM, raster counter, timer IRQ, auto-animation (lspc.c) ---- */
void     lspc_reset(void);
uint16_t lspc_read(uint32_t addr);
void     lspc_write(uint32_t addr, uint16_t v);
void     lspc_vblank(void);              /* IRQ1 time: auto-anim + timer autoload */
int64_t  lspc_timer_deadline(void);
void     lspc_timer_fire(void);
extern uint16_t lspc_vram[0x10000];
extern uint8_t  lspc_anim_counter;

/* ---- palette RAM (video.c) ---- */
uint16_t pal_read(uint32_t addr);
void     pal_write(uint32_t addr, uint16_t v);
void     pal_select_bank(int bank);

/* ---- renderer (video.c) ---- */
int  video_load(const uint8_t *crom, size_t crom_size, const uint8_t *srom, size_t srom_size,
                const uint8_t *sfix, const uint8_t *zoom_rom);
void video_set_fix_bios(int bios);       /* 1 = BIOS SFIX, 0 = cart S ROM */
void video_render_line(int line, uint32_t *row);   /* line 0..223 */
void video_shutdown(void);

/* ---- I/O ports, system latch, RTC (io.c) ---- */
void    io_reset(int mvs);
uint8_t io_read8(uint32_t addr);
void    io_write8(uint32_t addr, uint8_t v);
void    io_set_inputs(const ng_input_t *in);

/* ---- audio: Z80 + YM2610 (audio.c) ---- */
int     audio_init(const uint8_t *m1, size_t m1_size, const uint8_t *sm1,
                   const uint8_t *const *vroms, const size_t *vrom_sizes, int nvrom);
void    audio_reset(void);
void    audio_sync(void);                /* run the Z80 up to ng_cycles */
void    audio_command(uint8_t cmd);      /* 68k write to $320000 */
uint8_t audio_reply(void);               /* 68k read of $320000 */
void    audio_select_bios_rom(int bios); /* SM1 vs cart M1 */
size_t  audio_take_samples(int16_t *out, size_t max_frames);  /* stereo */
void    audio_shutdown(void);

/* ---- execution engine (exec.c) ---- */
void exec_init(const ng_func_entry_t *funcs, size_t nfuncs, int interp_only);
void exec_reset(void);
void exec_run(int64_t until);            /* run the 68000 until ng_cycles >= until */
void exec_report(void);
int  exec_dump_misses(const char *path);
void exec_set_verify(int on);           /* check every native block against Musashi */
void exec_irq_changed(void);            /* an IRQ line changed (tell Musashi if it is running) */
void exec_event_changed(int64_t at);    /* an event was scheduled; end the slice early if needed */
extern uint64_t exec_native_blocks, exec_interp_instrs;

/* ---- host platform (platform.c) ---- */
void platform_early_init(void);       /* crash reporting; call first */
int  platform_init(const char *title, int scale, int headless, const char *record_path);
bool platform_frame(const uint32_t *fb, ng_input_t *in);  /* false = quit */
void platform_audio(const int16_t *samples, size_t frames);
void platform_shutdown(void);
int  write_png(const char *path, const uint32_t *argb, int w, int h);

#endif
