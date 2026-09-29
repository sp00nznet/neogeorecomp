/*
 * neogeo.c — ng_main(): command line, ROM loading, and the frame loop.
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

int64_t ng_frame_start;

static uint32_t s_fb[NG_SCREEN_W * NG_SCREEN_H];

/* ---- options ---- */

#define MAX_SHOTS 64
typedef struct {
    const char *rom_path, *bios_path, *record, *input, *dump_misses, *bios_name;
    int headless, interp, verify, scale;
    long frames;
    struct { long frame; const char *path; } shots[MAX_SHOTS];
    int nshots;
} options_t;

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
        "  --scale N             window scale (default 3)\n", prog);
}

static int parse_args(int argc, char **argv, options_t *o) {
    memset(o, 0, sizeof(*o));
    o->rom_path = "roms"; o->scale = 3; o->frames = -1; o->bios_name = "sp-s2.sp1";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom-path") && next) { o->rom_path = next; i++; }
        else if (!strcmp(a, "--bios-path") && next) { o->bios_path = next; i++; }
        else if (!strcmp(a, "--bios") && next) { o->bios_name = next; i++; }
        else if (!strcmp(a, "--headless")) o->headless = 1;
        else if (!strcmp(a, "--record") && next) { o->record = next; i++; }
        else if (!strcmp(a, "--frames") && next) { o->frames = atol(next); i++; }
        else if (!strcmp(a, "--input") && next) { o->input = next; i++; }
        else if (!strcmp(a, "--interp")) o->interp = 1;
        else if (!strcmp(a, "--verify")) o->verify = 1;
        else if (!strcmp(a, "--dump-misses") && next) { o->dump_misses = next; i++; }
        else if (!strcmp(a, "--scale") && next) { o->scale = atoi(next); i++; }
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
    if (!o->bios_path) o->bios_path = o->rom_path;
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

static void script_input(long frame, ng_input_t *in) {
    for (int i = 0; i < s_nscript; i++) {
        const script_ev_t *e = &s_script[i];
        if (frame < e->start || frame > e->end) continue;
        in->p1 |= e->in.p1; in->p2 |= e->in.p2;
        in->start1 |= e->in.start1; in->start2 |= e->in.start2; in->select1 |= e->in.select1;
        in->coin1 |= e->in.coin1; in->coin2 |= e->in.coin2;
        in->service |= e->in.service; in->test |= e->in.test;
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

int ng_main(int argc, char **argv, const ng_game_t *game,
            const ng_func_entry_t *funcs, size_t nfuncs) {
    options_t o;
    platform_early_init();
    if (parse_args(argc, argv, &o)) return 2;
    if (o.input && load_script(o.input)) return 1;

    size_t sz, bios_sz, sfix_sz, sm1_sz, lo_sz, srom_sz, m1_sz;
    uint8_t *prom = ng_load_prom(o.rom_path, game);
    uint8_t *bios = ng_read_file(o.bios_path, o.bios_name, &bios_sz);
    uint8_t *sfix = ng_read_file(o.bios_path, "sfix.sfix", &sfix_sz);
    uint8_t *sm1 = ng_read_file(o.bios_path, "sm1.sm1", &sm1_sz);
    uint8_t *lo = ng_read_file(o.bios_path, "000-lo.lo", &lo_sz);
    uint8_t *crom = ng_load_crom(o.rom_path, game);
    uint8_t *srom = ng_read_file(o.rom_path, game->srom, &srom_sz);
    uint8_t *m1 = ng_read_file(o.rom_path, game->m1, &m1_sz);
    if (!prom || !bios || !sfix || !sm1 || !lo || !crom || !srom || !m1) {
        fprintf(stderr, "error: missing ROM files; see docs/running.md for the expected set\n");
        return 1;
    }
    if (bios_sz < 0x20000) { fprintf(stderr, "error: %s is not a 128 KB system ROM\n", o.bios_name); return 1; }
    ng_swap16(bios, 0x20000);
    (void)sz;

    const uint8_t *vroms[NG_MAX_PARTS];
    size_t vsizes[NG_MAX_PARTS];
    int nv = 0;
    for (; nv < NG_MAX_PARTS && game->vrom[nv]; nv++) {
        uint8_t *v = ng_read_file(o.rom_path, game->vrom[nv], &vsizes[nv]);
        if (!v) return 1;
        vroms[nv] = v;
    }

    ng_board_t board = {prom, game->prom_size, bios, 1};
    bus_init(&board);
    io_reset(1);
    video_load(crom, game->crom_size, srom, srom_sz, sfix, lo);
    free(crom);
    audio_init(m1, m1_sz, sm1, vroms, vsizes, nv);
    exec_init(funcs, nfuncs, o.interp || nfuncs == 0);
    exec_set_verify(o.verify);
    if (platform_init(game->title, o.scale, o.headless, o.record)) return 1;

    fprintf(stderr, "[neogeorecomp] %s: %zu recompiled entry points%s\n", game->name, nfuncs,
            (o.interp || nfuncs == 0) ? ", interpreter only" : "");
    power_on();

    static int16_t samples[2048 * 2];
    for (long frame = 0; o.frames < 0 || frame < o.frames; frame++) {
        ng_input_t in;
        memset(&in, 0, sizeof(in));
        if (!platform_frame(frame == 0 ? NULL : s_fb, &in)) break;
        script_input(frame, &in);
        io_set_inputs(&in);
        run_frame();
        platform_audio(samples, audio_take_samples(samples, 800));
        for (int i = 0; i < o.nshots; i++)
            if (o.shots[i].frame == frame) {
                if (write_png(o.shots[i].path, s_fb, NG_SCREEN_W, NG_SCREEN_H))
                    fprintf(stderr, "error: cannot write %s\n", o.shots[i].path);
                else
                    fprintf(stderr, "[neogeorecomp] frame %ld -> %s\n", frame, o.shots[i].path);
            }
    }
    platform_frame(s_fb, &(ng_input_t){0});

    exec_report();
    if (o.dump_misses) exec_dump_misses(o.dump_misses);
    platform_shutdown();
    video_shutdown();
    audio_shutdown();
    return 0;
}
