# Architecture

neogeorecomp is two things that share one view of the program:

1. **m68krecomp**, a build-time tool that turns a game's 68000 code into C (docs/recompiler.md).
2. **the runtime**, a library that models the MVS/AES board the recompiled C runs against.

A game repo contributes a ROM layout (`ng_game_t`) and nothing else. It has no addresses, patches or overrides: every title-specific fact the old runtime hardcoded (the USER entry point, the BIOS state byte, "force palette 66") is something the real system ROM now works out by itself. If a game misbehaves, the fix belongs in the board model.

## The parts

| File | Owns |
|---|---|
| `src/neogeo.c` | `ng_main()`: command line, ROM loading, power-on, the frame loop, scripted input, screenshots |
| `src/exec.c` | the dispatcher: interrupts, native-or-interpreted choice, Musashi hand-over, `--verify`, `--dump-misses` |
| `src/cpu.c`, `include/neogeorecomp/cpu.h` | the register file, SR writes, exception entry, and the flag-correct ALU helpers generated code calls |
| `src/bus.c`, `include/neogeorecomp/bus.h` | the 68000 memory map: page table for plain memory, slow path for devices, the `--verify` access log |
| `src/lspc.c` | the LSPC's CPU side: VRAM ports, raster counter, auto-animation, timer IRQ, IRQ acknowledge |
| `src/video.c` | palette RAM and the per-scanline renderer (sprites, fix layer) |
| `src/io.c` | controllers, status ports, DIPs, the system latch, the uPD4990A calendar |
| `src/audio.c` | the Z80 (memory, banking, ports, NMI), the 68k-Z80 latches, YM2610 timers |
| `src/platform.c`, `src/window_sdl.c` | headless output, `--record`, PNG writer, crash reports; the optional SDL2 window |
| `src/romload.c` | reading a set's files into regions; shared with the recompiler so both see the same program |
| `tools/m68krecomp/` | decoder, discovery, C emitter |
| `ext/musashi`, `ext/z80` | third-party cores (MIT), as submodules |

## Memory map

```
$000000-$0FFFFF  P ROM fixed bank; $000-$07F shows BIOS or cartridge vectors (system latch)
$100000-$1FFFFF  64 KB work RAM, mirrored
$200000-$2FFFFF  P ROM bank; writing $2FFFF0-$2FFFFF selects it
$300000-$3BFFFF  I/O: P1/DIPs $300000, sound latch + coins $320000, P2 $340000,
                 start/select $380000, output latches $380001+, system latch $3A0001-$3A001F
$3C0000-$3DFFFF  LSPC
$400000-$7FFFFF  palette RAM, 8 KB mirrored, two banks
$800000-$BFFFFF  memory card (absent)
$C00000-$CFFFFF  128 KB system ROM, mirrored
$D00000-$DFFFFF  64 KB backup RAM (MVS), writable only while unlocked
```

`bus.h` resolves plain memory through a 256-entry page table of 64 KB pages. ROM, work RAM and backup RAM are read inline by generated code. Pages with side effects are `NULL` and go to `bus.c`'s slow path. The vector swap, P ROM banking and the backup RAM lock are page-table edits, so they cost nothing per access.

## Timing

There is one clock, `ng_cycles`, counted in 68000 cycles at 12 MHz. Everything else derives from it, so a run is deterministic for a given input script. The same inputs produce byte-identical frames on Windows and Linux.

- A frame is 264 lines × 768 cycles (59.19 Hz). Lines 16-239 are visible.
- The frame loop runs the CPU to the start of each line and renders visible lines as it reaches them, so mid-frame changes from the timer IRQ land on the right lines.
- VBlank (IRQ1) is raised at line 240, pixel `$11F`, together with the auto-animation step and the timer's "reload at VBlank" option.
- The LSPC timer (IRQ2) counts pixel clocks (2 CPU cycles each) and fires `reload+1` pixels after loading. Its deadline caps the CPU slice, so it fires at the right cycle, not the next line.
- The Z80 runs lazily at 12 MHz / 3. It is caught up to the 68000 whenever the 68000 touches the sound latches, and at the end of each frame.

Recompiled code keeps `ng_cycles` current (`NG_INSN(n)` at each instruction) and checks `ng_next_event` on every loop back-edge. When an event is due it returns to the dispatcher, which handles it and resumes at the loop head. Straight-line code always ends in a call, return or jump, which also reach the dispatcher. So no CPU slice can overrun its event by more than one basic block.

## One frame

```
run_frame()
  for line 0..263:
    run_until(line start)            -> exec_run(): dispatch native routines / Musashi
      timer deadline inside? fire it (IRQ2), continue
    visible line? video_render_line()
    line 240: run to pixel $11F, lspc_vblank(): IRQ1, auto-anim, timer autoload
  audio_sync()                       Z80 catches up to the end of the frame
platform_frame()                     window present + input, or ffmpeg pipe, or nothing
```

`exec_run()` loops: take the highest pending interrupt the mask allows (a real 68000 exception frame on the emulated stack), then look up `CPU.pc`. If there is a recompiled entry, call it. It returns when control leaves the routine. Otherwise Musashi steps until it reaches an address that has one. The dispatcher is the only place control transfers between routines (docs/recompiler.md explains why).

## Boot

Power-on selects the BIOS vectors, asserts IRQ3 and starts the 68000 at the system ROM's reset vector. From there the real MVS BIOS does everything: it tests RAM, talks to the Z80, shows the eyecatcher, handles coins and credits, calls the cartridge's USER entry every frame, and calls PLAYER_START when Start is pressed. The runtime implements hardware. The BIOS's own logic is recompiled along with the game's, since `m68krecomp` covers the system ROM as a third region.

## What the runtime does not do yet

Sound synthesis (the YM2610 only models timers and status), memory cards, AES system ROMs, PAL timing, and games with protection or encrypted ROMs. ROADMAP.md tracks them.
