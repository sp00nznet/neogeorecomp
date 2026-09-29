# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow SemVer.

## [Unreleased]

The runtime is rebuilt as a generic model of the MVS board. The previous runtime (v0.1.0) was shaped around Neo Drift Out with title-specific patches; nothing from it survives except the name.

### Added
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
- Games describe only their ROM layout (`ng_game_t`) and hand the runtime their recompiled table through `ng_main()`.

### Removed
- The Drift Out-specific runtime: hardcoded USER/VBlank entry points, BIOS RAM guards, forced palettes and shrink, and the BIOS stub. The old public headers (`func_table.h`, `m68k.h`, ...) are gone.

### Fixed (found by the new checks)
- `MOVE.L`/`MOVEM.L` to `-(An)` store the low word first.
- `.w` absolute addresses keep their sign extension in `LEA`/`PEA`.
- `NBCD` with a zero result leaves its destination unwritten.

## [0.1.0]
- The initial runtime, brought up on Neo Drift Out.
