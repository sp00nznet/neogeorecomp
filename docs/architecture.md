# neogeorecomp Architecture

Deep dive into how the Neo Geo static recompilation runtime is structured and how the components interact.

## Design Philosophy

1. **Accuracy over speed** — Get it right first, optimize later. The 68000 instruction macros faithfully reproduce every flag update.
2. **Separation of concerns** — CPU logic, bus routing, and hardware emulation are cleanly separated. Swap out the video backend without touching the CPU layer.
3. **Reusable 68k core** — The `m68k.h` macros work for any 68000-based platform. Only the bus and hardware layers are Neo Geo-specific.
4. **Progressive recompilation** — Games can be partially recompiled. Missing functions are logged, not crashed on.

## Component Dependency Graph

```
                     ┌─────────────┐
                     │   main.c    │  Game-specific entry point
                     │  (per game) │  Loads ROMs, registers functions
                     └──────┬──────┘
                            │
                     ┌──────▼──────┐
                     │ neogeorecomp│  Orchestrator
                     │    .c/.h    │  Init, frame loop, lifecycle
                     └──────┬──────┘
                            │
              ┌─────────────┼─────────────┐
              │             │             │
       ┌──────▼──────┐ ┌───▼────┐ ┌──────▼──────┐
       │   m68k.h    │ │ bus.c  │ │ func_table.c│
       │ CPU context │ │ Memory │ │  Dispatch   │
       │ + macros    │ │ router │ │   table     │
       └─────────────┘ └───┬────┘ └─────────────┘
                           │
           ┌───────────────┼───────────────┐
           │               │               │
    ┌──────▼──────┐ ┌──────▼──────┐ ┌──────▼──────┐
    │   video.c   │ │  palette.c  │ │    io.c     │
    │ LSPC/sprites│ │ Dual-bank   │ │ Input/DIP   │
    │ Fix layer   │ │ color conv  │ │ Sys control │
    └─────────────┘ └─────────────┘ └─────────────┘
           │
    ┌──────▼──────┐ ┌─────────────┐ ┌─────────────┐
    │  platform.c │ │   z80.c     │ │  ym2610.c   │
    │ SDL2 window │ │ Audio CPU   │ │ Sound chip  │
    │ Input/Audio │ │ Interpreter │ │ FM/ADPCM    │
    └─────────────┘ └──────┬──────┘ └─────────────┘
                           │
                    ┌──────▼──────┐
                    │  timer.c    │
                    │ VBlank/IRQ  │
                    │ Watchdog    │
                    └─────────────┘
```

## Data Flow: One Frame

```
1. neogeo_begin_frame()
   ├── io_update()                    Poll controller state
   └── timer_kick_watchdog()          Auto-kick during development

2. Recompiled game code executes
   ├── Reads/writes via bus_read/write()
   │   ├── Work RAM ($100000)         Direct memory access
   │   ├── I/O regs ($300000)         Dispatched to io.c
   │   ├── Video regs ($3C0000)       Dispatched to video.c (VRAM access)
   │   ├── Palette ($400000)          Dispatched to palette.c
   │   └── Sound ($320000)            Dispatched to z80.c (command latch)
   ├── Function calls via func_table_call()
   └── Condition tests via M68K_CC_* macros

3. neogeo_trigger_vblank()
   ├── video_render_frame()           Read VRAM, decode sprites, composite
   │   ├── Process SCB1-4             381 sprites from VRAM
   │   ├── Decode C ROM tiles         4bpp -> ARGB via palette lookup
   │   ├── Render fix layer           40x32 tiles from S ROM
   │   └── Apply shadow if enabled
   ├── z80_execute(67614)             Run Z80 for one frame
   │   └── Programs YM2610 registers
   └── ym2610_generate()              Produce audio samples
       └── platform_audio_queue()     Send to SDL2 audio device

4. neogeo_end_frame()
   ├── platform_present()             Blit framebuffer to screen
   ├── timer_watchdog_tick()           Advance watchdog counter
   └── platform_frame_sync()          Wait for ~59.19 Hz
```

## The Bus: Central Nervous System

Every memory access from recompiled code goes through `bus.c`. The bus is the central router that makes the rest of the system work. When recompiled code calls `bus_write16(0x3C0002, tile_data)`, the bus recognizes this as a VRAM write and forwards it to `video_write_vram()`.

This design means:
- Recompiled code doesn't need to know about hardware details
- Hardware modules are isolated from each other
- Adding new hardware features (memory card, BIOS calls) just means adding cases to the bus decoder

## The Function Table: Control Flow Bridge

The function table bridges the original program's control flow with the recompiled native code. Every 68k subroutine becomes a C function registered at its original address:

```c
// Original: jsr $001234
// Recompiled:
func_table_call(0x001234);  // looks up and calls func_001234()
```

This is simpler than trying to translate the entire program as one monolithic function. Each 68k function becomes a clean C function boundary.

## Why Not Just Emulate?

Emulation interprets every instruction at runtime. Static recompilation translates instructions ahead of time, producing native code that runs at full CPU speed. The overhead is in the hardware abstraction (bus routing, video rendering, audio) — but the game logic itself runs at native speed.

For the Neo Geo, this means the 68000 game code (which runs at 12 MHz on real hardware) executes in microseconds on a modern x86-64 CPU. The bottleneck becomes the video rendering, not the game logic — and that can be GPU-accelerated.
