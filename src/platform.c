/*
 * platform.c — host window, keyboard, frame pacing, and headless output.
 *
 * --headless opens no window at all (it works over RDP and in CI) and runs
 * unpaced. --record pipes raw frames to ffmpeg, which must be on PATH.
 *
 * Keys: arrows, Z/X/C/V = A/B/C/D, 1/2 = start P1/P2, 3 = select,
 * 5/6 = coin 1/2, 9 = service, F2 = test switch, Esc = quit.
 */
#include "ng_internal.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

/* On a crash, print a symbolized stack so a report is actionable. */
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, NULL, TRUE);
    fprintf(stderr, "\n*** crash: exception %08lX at %p\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f;
    memset(&f, 0, sizeof(f));
    f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 32; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL) || !f.AddrPC.Offset)
            break;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line = {sizeof(line)};
        DWORD ldisp = 0;
        const char *name = SymFromAddr(proc, f.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
        if (SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &line))
            fprintf(stderr, "  %s  %s:%lu\n", name, line.FileName, line.LineNumber);
        else
            fprintf(stderr, "  %s+0x%llx\n", name, (unsigned long long)disp);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static int s_headless;

void platform_early_init(void) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
#endif
}
static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static FILE *s_rec;
static uint64_t s_next_tick;

int platform_init(const char *title, int scale, int headless, const char *record_path) {
    s_headless = headless;
#ifdef _WIN32
    platform_early_init();
#endif
    if (record_path) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s %dx%d -r 59.185606 -i - "
                 "-vf scale=%d:%d:flags=neighbor -c:v libx264 -pix_fmt yuv420p -crf 18 \"%s\"",
                 NG_SCREEN_W, NG_SCREEN_H, NG_SCREEN_W * 2, NG_SCREEN_H * 2, record_path);
        s_rec = popen(cmd, "wb");
        if (!s_rec) { fprintf(stderr, "error: cannot start ffmpeg for --record\n"); return -1; }
    }
    if (headless) return 0;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "error: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    s_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             NG_SCREEN_W * scale, NG_SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(s_ren, NG_SCREEN_W, NG_SCREEN_H);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                              NG_SCREEN_W, NG_SCREEN_H);
    s_next_tick = SDL_GetPerformanceCounter();
    return 0;
}

static void read_keys(ng_input_t *in) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    if (k[SDL_SCANCODE_UP]) in->p1 |= NG_UP;
    if (k[SDL_SCANCODE_DOWN]) in->p1 |= NG_DOWN;
    if (k[SDL_SCANCODE_LEFT]) in->p1 |= NG_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) in->p1 |= NG_RIGHT;
    if (k[SDL_SCANCODE_Z]) in->p1 |= NG_A;
    if (k[SDL_SCANCODE_X]) in->p1 |= NG_B;
    if (k[SDL_SCANCODE_C]) in->p1 |= NG_C;
    if (k[SDL_SCANCODE_V]) in->p1 |= NG_D;
    in->start1 = k[SDL_SCANCODE_1];
    in->start2 = k[SDL_SCANCODE_2];
    in->select1 = k[SDL_SCANCODE_3];
    in->coin1 = k[SDL_SCANCODE_5];
    in->coin2 = k[SDL_SCANCODE_6];
    in->service = k[SDL_SCANCODE_9];
    in->test = k[SDL_SCANCODE_F2];
}

bool platform_frame(const uint32_t *fb, ng_input_t *in) {
    if (fb && s_rec) fwrite(fb, 4, NG_SCREEN_W * NG_SCREEN_H, s_rec);
    if (s_headless) return true;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return false;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return false;
    }
    read_keys(in);
    if (fb) {
        SDL_UpdateTexture(s_tex, NULL, fb, NG_SCREEN_W * 4);
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);
    }
    /* Pace to the MVS refresh rate, 59.1856 Hz. */
    uint64_t freq = SDL_GetPerformanceFrequency();
    s_next_tick += (uint64_t)((double)freq / 59.185606);
    uint64_t now = SDL_GetPerformanceCounter();
    if (now < s_next_tick) SDL_Delay((Uint32)((s_next_tick - now) * 1000 / freq));
    else if (now - s_next_tick > freq / 10) s_next_tick = now;
    return true;
}

void platform_audio(const int16_t *samples, size_t frames) { (void)samples; (void)frames; }

void platform_shutdown(void) {
    if (s_rec) { pclose(s_rec); s_rec = NULL; }
    if (s_tex) SDL_DestroyTexture(s_tex);
    if (s_ren) SDL_DestroyRenderer(s_ren);
    if (s_win) SDL_DestroyWindow(s_win);
    if (!s_headless) SDL_Quit();
}

/* ---- PNG writer (stored deflate blocks; no zlib dependency) ---- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t hdr[8];
    put32(hdr, (uint32_t)n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc32_update(0, hdr + 4, 4);
    c = crc32_update(c, data, n);
    uint8_t cb[4];
    put32(cb, c);
    fwrite(cb, 1, 4, f);
}

int write_png(const char *path, const uint32_t *argb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)w); put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);

    size_t raw_n = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    uint8_t *p = raw;
    for (int y = 0; y < h; y++) {
        *p++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = argb[y * w + x];
            *p++ = (uint8_t)(c >> 16); *p++ = (uint8_t)(c >> 8); *p++ = (uint8_t)c;
        }
    }
    size_t blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(z_n), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t len = raw_n - off < 65535 ? raw_n - off : 65535;
        *q++ = (off + len == raw_n) ? 1 : 0;
        *q++ = (uint8_t)len; *q++ = (uint8_t)(len >> 8);
        *q++ = (uint8_t)~len; *q++ = (uint8_t)(~len >> 8);
        memcpy(q, raw + off, len);
        for (size_t i = 0; i < len; i++) { s1 = (s1 + raw[off + i]) % 65521; s2 = (s2 + s1) % 65521; }
        q += len;
    }
    put32(q, (s2 << 16) | s1); q += 4;
    chunk(f, "IDAT", z, (size_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    free(raw); free(z);
    fclose(f);
    return 0;
}
