# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow SemVer.

## [Unreleased]

## [0.2.0] - 2026-10-20

The runtime is rebuilt as a generic model of the MVS board (#1). The previous runtime (v0.1.0) was shaped around Neo Drift Out with title-specific patches; nothing from it survives except the name.

### Added
- **Gamepads**: up to two through SDL2's GameController API (player 1, player 2, hot-pluggable), D-pad or left stick, A/B/X/Y = A/B/C/D, Start, Back = coin, left shoulder = select; works beside the keyboard. `ng_gamepad_test` (CTest, when SDL2 is found) checks the mapping, both players and unplugging with SDL's virtual joystick. (#6)
- **macOS** (Apple Silicon) in CI beside Ubuntu and Windows, and a release workflow: a `v*` tag attaches a Windows, Linux and macOS bundle of the standalone `m68krecomp` tool and the docs, after the conformance harness passes on each. (#5)
- `LICENSES/` with the verbatim licences of Musashi, z80 and ymfm, and their pinned commits in `NOTICE`. (#7)
- **Sound**: YM2610 synthesis (FM, SSG, ADPCM-A/B from the V ROMs) through [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3, ext/ymfm). Samples are generated up to the Z80's clock before each register write, at the chip's rate (8 MHz / 144); the window plays them through SDL2, `--wav FILE` writes them, `--mute` silences the window. The YM2610 timers and IRQ stay modelled in `audio.c`. — *[@danilopaulinodasilva](https://github.com/danilopaulinodasilva)* (#3)
- **m68krecomp**, the 68000 static recompiler: a full 68000 decoder; recursive-descent discovery from vectors, cartridge header entry points, static call targets, jump tables and code pointers; readable C output with address and disassembly on every line; `--entries` for runtime-found entry points; a `--raw` mode and a standalone tool for flat images.
- **Flat dispatch execution model**: return addresses on the emulated stack, interrupts as real 68000 exceptions, and a [Musashi](https://github.com/kstenerud/Musashi) fallback for addresses without a recompiled entry.
- **`--verify`**: every native block is replayed in Musashi against the same memory and I/O values, and registers, flags and writes are compared.
- **Conformance harness** (`ng_conformance`, CTest): Musashi's 60 68000 test programs, recompiled and run natively, with a reference column and a regression baseline. Currently 59/60, the same as the reference.
- **Board model**: full 68000 memory map with vector swap, P ROM banking and backup RAM lock; LSPC VRAM ports, raster counter, timer IRQ and auto-animation; per-scanline sprite renderer (sticky chains, L0 shrink, horizontal shrink, 96/line) and fix layer; palette banks; controllers, status ports, system latch and uPD4990A calendar; Z80 with banking, NMI/reply latches and YM2610 timers.
- Boot through the real MVS system ROM. There is no BIOS HLE.
- Headless mode, `--record` (ffmpeg), `--screenshot`, scripted `--input`, `--interp`, `--dump-misses`, `NG_PROFILE`, and symbolized crash reports on Windows.
- The SDL2 window is optional; headless-only builds need no dependencies.
- CI on Ubuntu (gcc) and Windows (MSVC).
- Docs: architecture, recompiler, running, conformance, porting a game, hardware notes.

### Changed
- Neo Drift Out is ported to the generic runtime with no toolkit changes. Its old per-game overrides are deleted, and it boots, attracts and races verified.
- Games describe only their ROM layout (`ng_game_t`) and hand the runtime their recompiled table through `ng_main()`.

### Removed
- The Drift Out-specific runtime: hardcoded USER/VBlank entry points, BIOS RAM guards, forced palettes and shrink, and the BIOS stub. The old public headers (`func_table.h`, `m68k.h`, ...) are gone.

### Fixed (found by the new checks)
- `MOVE.L`/`MOVEM.L` to `-(An)` store the low word first.
- `.w` absolute addresses keep their sign extension in `LEA`/`PEA`.
- `NBCD` with a zero result leaves its destination unwritten.

## [0.1.0]
- The initial runtime, brought up on Neo Drift Out.
