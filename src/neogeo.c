/*
 * neogeo.c — runtime lifecycle (ng_init/ng_step_frame/ng_shutdown) and ng_main():
 * command line, ROM loading, the frame loop, scripted input, screenshots.
 *
 * A frame is 264 lines of 768 68k clocks. The loop runs the CPU to the
 * start of each line, renders visible lines as they are reached, raises
 * VBlank (IRQ1) at line 240 pixel $11F, and services the LSPC timer
 * whenever its deadline falls inside the slice.
 *
 * Input comes from the window, or from an --input script so headless runs
 * are reproducible: `<frame>[-<end>] <button>[+<button>...]`, one per line.
 */
#include "ng_internal.h"
#include <neogeorecomp/bus.h>
#include "romload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

int64_t ng_frame_start;

static uint32_t s_fb[NG_SCREEN_W * NG_SCREEN_H];

/* ---- options ---- */

#define MAX_SHOTS 64
typedef struct {
    ng_config_t cfg;
    struct { long frame; const char *path; } shots[MAX_SHOTS];
    int nshots;
} options_t;

void ng_config_default(ng_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->rom_path = "roms";
    cfg->bios_name = "sp-s2.sp1";
    cfg->scale = 3;
    cfg->frames = -1;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --rom-path DIR        game ROM files (default: roms)\n"
        "  --bios-path DIR       system ROM files (default: the ROM path)\n"
        "  --bios FILE           system ROM (default: sp-s2.sp1, MVS Asia/Europe)\n"
        "  --headless            no window; run as fast as possible\n"
        "  --record FILE.mp4     pipe frames to ffmpeg\n"
        "  --frames N            stop after N frames\n"
        "  --screenshot N:FILE   save frame N as PNG (repeatable)\n"
        "  --input FILE          scripted input (see docs/running.md)\n"
        "  --interp              run everything in the interpreter\n"
        "  --verify              re-run every recompiled block in the interpreter and compare\n"
        "  --dump-misses FILE    write interpreted entry points for the recompiler\n"
        "  --scale N             window scale (default 3)\n"
        "  --wav FILE            write the audio to a WAV file\n"
        "  --mute                no sound from the window\n", prog);
}

static int parse_args(int argc, char **argv, options_t *o) {
    memset(o, 0, sizeof(*o));
    ng_config_default(&o->cfg);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom-path") && next) { o->cfg.rom_path = next; i++; }
        else if (!strcmp(a, "--bios-path") && next) { o->cfg.bios_path = next; i++; }
        else if (!strcmp(a, "--bios") && next) { o->cfg.bios_name = next; i++; }
        else if (!strcmp(a, "--headless")) o->cfg.headless = 1;
        else if (!strcmp(a, "--record") && next) { o->cfg.record_path = next; i++; }
        else if (!strcmp(a, "--frames") && next) { o->cfg.frames = atol(next); i++; }
        else if (!strcmp(a, "--input") && next) { o->cfg.input_path = next; i++; }
        else if (!strcmp(a, "--interp")) o->cfg.interp = 1;
        else if (!strcmp(a, "--verify")) o->cfg.verify = 1;
        else if (!strcmp(a, "--dump-misses") && next) { o->cfg.dump_misses = next; i++; }
        else if (!strcmp(a, "--scale") && next) { o->cfg.scale = atoi(next); i++; }
        else if (!strcmp(a, "--wav") && next) { o->cfg.wav_path = next; i++; }
        else if (!strcmp(a, "--mute")) o->cfg.mute = 1;
        else if (!strcmp(a, "--screenshot") && next) {
            const char *colon = strchr(next, ':');
            if (!colon || o->nshots == MAX_SHOTS) { usage(argv[0]); return -1; }
            o->shots[o->nshots].frame = atol(next);
            o->shots[o->nshots].path = colon + 1;
            o->nshots++; i++;
        }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); exit(0); }
        else { usage(argv[0]); return -1; }
    }
    if (!o->cfg.bios_path) o->cfg.bios_path = o->cfg.rom_path;
    return 0;
}

/* ---- scripted input ---- */

typedef struct { long start, end; ng_input_t in; } script_ev_t;
static script_ev_t *s_script;
static int s_nscript;

static int button(const char *name, ng_input_t *in) {
    static const struct { const char *n; int kind; uint8_t bit; } map[] = {
        {"up", 1, NG_UP}, {"down", 1, NG_DOWN}, {"left", 1, NG_LEFT}, {"right", 1, NG_RIGHT},
        {"a", 1, NG_A}, {"b", 1, NG_B}, {"c", 1, NG_C}, {"d", 1, NG_D},
        {"p2up", 2, NG_UP}, {"p2down", 2, NG_DOWN}, {"p2left", 2, NG_LEFT}, {"p2right", 2, NG_RIGHT},
        {"p2a", 2, NG_A}, {"p2b", 2, NG_B}, {"p2c", 2, NG_C}, {"p2d", 2, NG_D},
        {"start", 3, 0}, {"start2", 4, 0}, {"select", 5, 0}, {"coin", 6, 0}, {"coin2", 7, 0},
        {"service", 8, 0}, {"test", 9, 0},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].n)) continue;
        switch (map[i].kind) {
        case 1: in->p1 |= map[i].bit; break;
        case 2: in->p2 |= map[i].bit; break;
        case 3: in->start1 = 1; break;
        case 4: in->start2 = 1; break;
        case 5: in->select1 = 1; break;
        case 6: in->coin1 = 1; break;
        case 7: in->coin2 = 1; break;
        case 8: in->service = 1; break;
        case 9: in->test = 1; break;
        }
        return 0;
    }
    return -1;
}

static int load_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return -1; }
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        long start, end;
        char buttons[200];
        int n = sscanf(line, "%ld-%ld %199s", &start, &end, buttons);
        if (n != 3) {
            if (sscanf(line, "%ld %199s", &start, buttons) != 2) continue;
            end = start + 5;   /* long enough for the BIOS to debounce a press */
        }
        script_ev_t ev = {start, end, {0}};
        for (char *tok = strtok(buttons, "+"); tok; tok = strtok(NULL, "+"))
            if (button(tok, &ev.in)) {
                fprintf(stderr, "%s:%d: unknown button '%s'\n", path, lineno, tok);
                fclose(f); return -1;
            }
        s_script = (script_ev_t *)realloc(s_script, (size_t)(s_nscript + 1) * sizeof(*s_script));
        s_script[s_nscript++] = ev;
    }
    fclose(f);
    return 0;
}

static void merge_input(ng_input_t *dst, const ng_input_t *src) {
    dst->p1 |= src->p1; dst->p2 |= src->p2;
    dst->start1 |= src->start1; dst->start2 |= src->start2;
    dst->select1 |= src->select1; dst->select2 |= src->select2;
    dst->coin1 |= src->coin1; dst->coin2 |= src->coin2;
    dst->service |= src->service; dst->test |= src->test;
}

static void script_input(long frame, ng_input_t *in) {
    for (int i = 0; i < s_nscript; i++) {
        const script_ev_t *e = &s_script[i];
        if (frame < e->start || frame > e->end) continue;
        merge_input(in, &e->in);
    }
}

/* ---- frame loop ---- */

static void run_until(int64_t t) {
    while (ng_cycles < t) {
        int64_t target = t, d = lspc_timer_deadline();
        if (d < target) target = d;
        if (target > ng_cycles) exec_run(target);
        if (ng_cycles >= lspc_timer_deadline()) lspc_timer_fire();
    }
}

static void run_frame(void) {
    int64_t fs = ng_frame_start;
    for (int line = 0; line < NG_LINES_PER_FRAME; line++) {
        int64_t ls = fs + (int64_t)line * NG_CLOCKS_PER_LINE;
        run_until(ls);
        if (line >= NG_FIRST_VISIBLE && line < NG_VBLANK_LINE)
            video_render_line(line - NG_FIRST_VISIBLE, s_fb + (line - NG_FIRST_VISIBLE) * NG_SCREEN_W);
        if (line == NG_VBLANK_LINE) {
            run_until(ls + NG_VBLANK_HPOS * 2);
            lspc_vblank();
        }
    }
    ng_frame_start = fs + NG_CLOCKS_PER_FRAME;
    run_until(ng_frame_start);
    audio_sync();
}

static void power_on(void) {
    ng_cycles = 0;
    ng_frame_start = 0;
    bus_reset();
    lspc_reset();
    pal_select_bank(0);
    video_set_fix_bios(1);
    audio_select_bios_rom(1);
    audio_reset();
    exec_reset();
    ng_irq_pending = 4;        /* IRQ3 is asserted at power-on until the BIOS acks it */
}

/* ---- runtime lifecycle state ---- */

static int s_initialized;
static int s_quit;
static long s_frame;
static long s_max_frames = -1;
static char *s_dump_misses;
static ng_input_t s_ext_in;

static uint8_t *s_prom, *s_bios, *s_sfix, *s_sm1, *s_lo, *s_srom, *s_m1;
static uint8_t *s_vroms[NG_MAX_PARTS];
static int s_nv;

static char *ng_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void free_rom_buffers(void) {
    free(s_prom); s_prom = NULL;
    free(s_bios); s_bios = NULL;
    free(s_sfix); s_sfix = NULL;
    free(s_sm1);  s_sm1 = NULL;
    free(s_lo);   s_lo = NULL;
    free(s_srom); s_srom = NULL;
    free(s_m1);   s_m1 = NULL;
    for (int i = 0; i < s_nv; i++) { free(s_vroms[i]); s_vroms[i] = NULL; }
    s_nv = 0;
}

int ng_init(const ng_config_t *cfg, const ng_game_t *game,
            const ng_func_entry_t *funcs, size_t nfuncs) {
    ng_config_t c;
    if (cfg) c = *cfg;
    else ng_config_default(&c);

    if (!c.rom_path) c.rom_path = "roms";
    if (!c.bios_path) c.bios_path = c.rom_path;
    if (!c.bios_name) c.bios_name = "sp-s2.sp1";
    if (c.scale <= 0) c.scale = 3;

    if (s_initialized) ng_shutdown();
    memset(s_fb, 0, sizeof(s_fb));
    memset(&s_ext_in, 0, sizeof(s_ext_in));

    if (c.input_path && load_script(c.input_path)) {
        free(s_script); s_script = NULL; s_nscript = 0;
        return -1;
    }

    size_t bios_sz, sfix_sz, sm1_sz, lo_sz, srom_sz, m1_sz;
    s_prom = ng_load_prom(c.rom_path, game);
    s_bios = ng_read_file(c.bios_path, c.bios_name, &bios_sz);
    s_sfix = ng_read_file(c.bios_path, "sfix.sfix", &sfix_sz);
    s_sm1  = ng_read_file(c.bios_path, "sm1.sm1", &sm1_sz);
    s_lo   = ng_read_file(c.bios_path, "000-lo.lo", &lo_sz);
    uint8_t *crom = ng_load_crom(c.rom_path, game);
    s_srom = ng_read_file(c.rom_path, game->srom, &srom_sz);
    s_m1   = ng_read_file(c.rom_path, game->m1, &m1_sz);
    if (!s_prom || !s_bios || !s_sfix || !s_sm1 || !s_lo || !crom || !s_srom || !s_m1) {
        fprintf(stderr, "error: missing ROM files; see docs/running.md for the expected set\n");
        free(crom);
        free_rom_buffers();
        free(s_script); s_script = NULL; s_nscript = 0;
        return -1;
    }
    if (bios_sz < 0x20000) {
        fprintf(stderr, "error: %s is not a 128 KB system ROM\n", c.bios_name);
        free(crom);
        free_rom_buffers();
        free(s_script); s_script = NULL; s_nscript = 0;
        return -1;
    }
    ng_swap16(s_bios, 0x20000);

    const uint8_t *vroms[NG_MAX_PARTS];
    size_t vsizes[NG_MAX_PARTS];
    s_nv = 0;
    for (; s_nv < NG_MAX_PARTS && game->vrom[s_nv]; s_nv++) {
        uint8_t *v = ng_read_file(c.rom_path, game->vrom[s_nv], &vsizes[s_nv]);
        if (!v) {
            free(crom);
            free_rom_buffers();
            free(s_script); s_script = NULL; s_nscript = 0;
            return -1;
        }
        s_vroms[s_nv] = v;
        vroms[s_nv] = v;
    }

    ng_board_t board = {s_prom, game->prom_size, s_bios, 1};
    bus_init(&board);
    io_reset(1);
    video_load(crom, game->crom_size, s_srom, srom_sz, s_sfix, s_lo);
    free(crom);
    audio_init(s_m1, m1_sz, s_sm1, vroms, vsizes, s_nv);
    exec_init(funcs, nfuncs, c.interp || nfuncs == 0);
    exec_set_verify(c.verify);
    if (platform_init(game->title, c.scale, c.headless, c.record_path)) {
        video_shutdown();
        audio_shutdown();
        exec_shutdown();
        free_rom_buffers();
        free(s_script); s_script = NULL; s_nscript = 0;
        return -1;
    }
    if (c.mute) platform_mute();
    if (c.wav_path && platform_wav(c.wav_path)) {
        platform_shutdown();
        video_shutdown();
        audio_shutdown();
        exec_shutdown();
        free_rom_buffers();
        free(s_script); s_script = NULL; s_nscript = 0;
        return -1;
    }

    s_max_frames = c.frames;
    s_dump_misses = ng_strdup(c.dump_misses);
    s_frame = 0;
    s_quit = 0;
    s_initialized = 1;

    fprintf(stderr, "[neogeorecomp] %s: %zu recompiled entry points%s\n", game->name, nfuncs,
            (c.interp || nfuncs == 0) ? ", interpreter only" : "");
    power_on();
    return 0;
}

int ng_step_frame(void) {
    if (!s_initialized) return -1;
    if (s_quit || (s_max_frames >= 0 && s_frame >= s_max_frames)) return 0;

    ng_input_t in;
    memset(&in, 0, sizeof(in));
    if (!platform_poll(&in)) {
        s_quit = 1;
        return 0;
    }
    merge_input(&in, &s_ext_in);
    script_input(s_frame, &in);
    io_set_inputs(&in);
    run_frame();

    static int16_t samples[4096 * 2];
    platform_audio(samples, audio_take_samples(samples, 4096));
    platform_present(s_fb);
    s_frame++;
    return 1;
}

void ng_reset(void) {
    if (!s_initialized) return;
    s_frame = 0;
    s_quit = 0;
    memset(s_fb, 0, sizeof(s_fb));
    power_on();
}

void ng_shutdown(void) {
    if (!s_initialized) return;
    exec_report();
    if (s_dump_misses) exec_dump_misses(s_dump_misses);
    platform_shutdown();
    video_shutdown();
    audio_shutdown();
    exec_shutdown();
    free_rom_buffers();
    free(s_script); s_script = NULL; s_nscript = 0;
    free(s_dump_misses); s_dump_misses = NULL;
    s_frame = 0;
    s_max_frames = -1;
    s_quit = 0;
    s_initialized = 0;
}

const uint32_t *ng_framebuffer(void) { return s_fb; }
long ng_frame_count(void) { return s_frame; }
void ng_set_external_input(const ng_input_t *in) {
    if (in) s_ext_in = *in;
    else memset(&s_ext_in, 0, sizeof(s_ext_in));
}

static void save_shots(const options_t *o, long frame) {
    for (int i = 0; i < o->nshots; i++) {
        if (o->shots[i].frame == frame) {
            if (write_png(o->shots[i].path, s_fb, NG_SCREEN_W, NG_SCREEN_H))
                fprintf(stderr, "error: cannot write %s\n", o->shots[i].path);
            else
                fprintf(stderr, "[neogeorecomp] frame %ld -> %s\n", frame, o->shots[i].path);
        }
    }
}

#ifdef __EMSCRIPTEN__
#define NG_FRAME_MS (1000.0 / 59.185606)

typedef struct {
    options_t opts;
    double last_ms;
    double accum_ms;
    int paused;
    int active;
} em_loop_t;

static em_loop_t s_em;

static void em_tick(void *arg) {
    em_loop_t *em = (em_loop_t *)arg;
    if (!s_initialized || em->paused) return;

    double now = emscripten_get_now();
    if (em->last_ms < 0.0) {
        em->last_ms = now;
        em->accum_ms = NG_FRAME_MS;
    } else {
        double dt = now - em->last_ms;
        em->last_ms = now;
        if (dt < 0.0) dt = 0.0;
        if (dt > NG_FRAME_MS * 4.0) dt = NG_FRAME_MS * 4.0;
        em->accum_ms += dt;
    }

    int stepped = 0;
    while (em->accum_ms >= NG_FRAME_MS && stepped < 4) {
        long frame = s_frame;
        int r = ng_step_frame();
        if (r <= 0) {
            em->active = 0;
            emscripten_cancel_main_loop();
            ng_shutdown();
            return;
        }
        save_shots(&em->opts, frame);
        em->accum_ms -= NG_FRAME_MS;
        stepped++;
    }
}

EMSCRIPTEN_KEEPALIVE void ng_wasm_pause(int paused) {
    s_em.paused = paused ? 1 : 0;
    if (!s_em.paused) s_em.last_ms = -1.0;
    platform_pause(s_em.paused);
}

EMSCRIPTEN_KEEPALIVE int ng_wasm_is_paused(void) { return s_em.paused; }
EMSCRIPTEN_KEEPALIVE void ng_wasm_reset(void) {
    ng_reset();
    s_em.last_ms = -1.0;
    s_em.accum_ms = NG_FRAME_MS;
}

EMSCRIPTEN_KEEPALIVE void ng_wasm_stop(void) {
    if (s_em.active) {
        s_em.active = 0;
        emscripten_cancel_main_loop();
    }
    s_em.paused = 0;
    ng_shutdown();
}

EMSCRIPTEN_KEEPALIVE int ng_wasm_is_running(void) { return s_initialized && !s_quit; }
EMSCRIPTEN_KEEPALIVE double ng_wasm_frame_count(void) { return (double)s_frame; }
EMSCRIPTEN_KEEPALIVE double ng_wasm_native_blocks(void) { return (double)exec_native_blocks; }
EMSCRIPTEN_KEEPALIVE double ng_wasm_interp_instrs(void) { return (double)exec_interp_instrs; }
EMSCRIPTEN_KEEPALIVE void ng_wasm_set_input(uint32_t p1, uint32_t start1, uint32_t select1,
                                            uint32_t coin1, uint32_t service, uint32_t test) {
    ng_input_t in;
    memset(&in, 0, sizeof(in));
    in.p1 = (uint8_t)p1;
    in.start1 = (uint8_t)(start1 ? 1 : 0);
    in.select1 = (uint8_t)(select1 ? 1 : 0);
    in.coin1 = (uint8_t)(coin1 ? 1 : 0);
    in.service = (uint8_t)(service ? 1 : 0);
    in.test = (uint8_t)(test ? 1 : 0);
    ng_set_external_input(&in);
}
#endif

int ng_main(int argc, char **argv, const ng_game_t *game,
            const ng_func_entry_t *funcs, size_t nfuncs) {
    options_t o;
    platform_early_init();
    if (parse_args(argc, argv, &o)) return 2;
    if (ng_init(&o.cfg, game, funcs, nfuncs)) return 1;

#ifdef __EMSCRIPTEN__
    if (!o.cfg.headless) {
        memset(&s_em, 0, sizeof(s_em));
        s_em.opts = o;
        s_em.last_ms = -1.0;
        s_em.active = 1;
        emscripten_set_main_loop_arg(em_tick, &s_em, 0, 1);
        return 0;
    }
#endif

    while (1) {
        long frame = s_frame;
        int r = ng_step_frame();
        if (r <= 0) break;
        save_shots(&o, frame);
    }

    ng_shutdown();
    return 0;
}
