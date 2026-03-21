# Getting Started: Recompiling a Neo Geo Game

This guide walks through the process of statically recompiling a Neo Geo game using the `neogeorecomp` runtime. We'll use the general approach — the same one used for Metal Slug and Neo Drift Out.

## Prerequisites

- A legally obtained ROM dump of the game you want to recompile (you must own the cartridge)
- [Ghidra](https://ghidra-sre.org/) (free) or IDA Pro for disassembly
- A C17 compiler (MSVC 2022, Clang, or GCC)
- CMake 3.20+
- SDL2 development libraries
- Patience — this is a manual process that rewards attention to detail

## Step 1: Identify Your ROM Set

Every Neo Geo game has a standard ROM set:

| Type | Contents | Used By |
|------|----------|---------|
| P ROM | 68000 program code | This is what we recompile |
| S ROM | Fix layer (HUD/text) tiles | Loaded by video subsystem |
| C ROMs | Sprite tiles (always in pairs) | Loaded by video subsystem |
| M ROM | Z80 audio program | Loaded by Z80 subsystem |
| V ROMs | ADPCM audio samples | Loaded by YM2610 subsystem |

The P ROM is the target of recompilation. Everything else is loaded as data by the runtime.

Check your game on [Arcade Italia](https://adb.arcadeitalia.net/) or in the MAME source to identify the exact ROM filenames and sizes.

## Step 2: Disassemble the P ROM

Load the P ROM into Ghidra with these settings:

1. **Processor**: MC68000 (Motorola 68000)
2. **Base address**: 0x000000
3. **Endianness**: Big-endian

### Key Locations to Start

| Address | Contents |
|---------|----------|
| $000000 | Initial Supervisor Stack Pointer |
| $000004 | Initial Program Counter (reset vector — **start here**) |
| $000068 | Level 1 interrupt vector (VBlank handler) |
| $00006C | Level 2 interrupt vector (Timer handler) |

Start analysis from the reset vector ($000004). Ghidra's auto-analysis will find many functions, but you'll need to manually identify some that are reached through indirect calls or jump tables.

### Understanding BIOS Calls

Neo Geo games interact with the BIOS through a set of standardized calls. The BIOS handles the startup sequence, and eventually transfers control to the game by jumping to addresses stored in a header structure. Key BIOS interaction points:

- **$122**: Pointer to game's USER subroutine (called by BIOS each frame)
- **$11A**: Pointer to game's PLAYER_START routine
- **$116**: Pointer to game's DEMO_END routine

The BIOS calls the USER routine on every VBlank. This is typically where the game's main loop lives.

## Step 3: Lift Functions to C

For each function identified in the disassembly, create a C function that reproduces its behavior using our macro layer.

### Example Translation

**Original 68000 assembly:**
```asm
func_001234:
    movem.l d0-d3/a0-a1, -(sp)    ; save registers
    move.w  (a0), d0               ; load value from address in a0
    add.w   #$0010, d0             ; add 16
    cmp.w   #$0100, d0             ; compare with 256
    bge.s   .overflow              ; branch if >= 256
    move.w  d0, (a0)               ; store result
    bra.s   .done
.overflow:
    move.w  #$00FF, (a0)           ; clamp to 255
.done:
    movem.l (sp)+, d0-d3/a0-a1    ; restore registers
    rts
```

**Recompiled C:**
```c
#include <neogeorecomp/neogeorecomp.h>

void func_001234(void) {
    /* Save registers (handled by compiler — we use local copies) */
    uint32_t save_d0 = g_m68k.d[0], save_d1 = g_m68k.d[1];
    uint32_t save_d2 = g_m68k.d[2], save_d3 = g_m68k.d[3];
    uint32_t save_a0 = g_m68k.a[0], save_a1 = g_m68k.a[1];

    /* move.w (a0), d0 */
    M68K_MOVE16(g_m68k.d[0], bus_read16(g_m68k.a[0]));

    /* add.w #$0010, d0 */
    M68K_ADD16(g_m68k.d[0], 0x0010);

    /* cmp.w #$0100, d0 */
    M68K_CMP16(g_m68k.d[0], 0x0100);

    /* bge.s .overflow */
    if (M68K_CC_GE) {
        /* move.w #$00FF, (a0) — clamp */
        bus_write16(g_m68k.a[0], 0x00FF);
    } else {
        /* move.w d0, (a0) — store result */
        bus_write16(g_m68k.a[0], (uint16_t)g_m68k.d[0]);
    }

    /* Restore registers */
    g_m68k.d[0] = save_d0; g_m68k.d[1] = save_d1;
    g_m68k.d[2] = save_d2; g_m68k.d[3] = save_d3;
    g_m68k.a[0] = save_a0; g_m68k.a[1] = save_a1;
}
```

### Key Translation Patterns

| 68k Pattern | C Equivalent |
|-------------|-------------|
| `move.w d0, d1` | `M68K_MOVE16(g_m68k.d[1], g_m68k.d[0])` |
| `add.l #imm, d0` | `M68K_ADD32(g_m68k.d[0], imm)` |
| `jsr label` | `func_table_call(label_addr)` |
| `bsr label` | `func_table_call(label_addr)` |
| `beq target` | `if (M68K_CC_EQ) goto target;` |
| `move.w (a0)+, d0` | `M68K_MOVE16(g_m68k.d[0], bus_read16(g_m68k.a[0])); g_m68k.a[0] += 2;` |
| `move.w d0, -(sp)` | `g_m68k.a[7] -= 2; bus_write16(g_m68k.a[7], g_m68k.d[0]);` |
| `lea label, a0` | `g_m68k.a[0] = label_addr;` |
| Memory read | `bus_read8/16/32(addr)` |
| Memory write | `bus_write8/16/32(addr, val)` |

## Step 4: Register Functions

In your game's `main.c`, register each recompiled function:

```c
func_table_register(0x001234, func_001234);
func_table_register(0x001300, func_001300);
// ... etc
```

## Step 5: Build and Test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DNEOGEORECOMP_DEBUG=ON
cmake --build build
./build/mygame --rom-path /path/to/roms/
```

Enable debug tracing to see which functions are being called and which addresses are missing:

```c
debug_trace_enable(true);
```

The runtime will log warnings for any `func_table_call()` to an unregistered address — these are functions you still need to recompile.

## Step 6: Iterate

Static recompilation is an iterative process:

1. Run the game, note which functions are missing
2. Disassemble and recompile those functions
3. Register them and run again
4. Repeat until the game progresses further

Start with the critical path: reset vector -> BIOS interaction -> main loop -> VBlank handler. Once the game boots and shows something on screen, work outward from there.

## Tips

- **Start with the VBlank handler** — it's called every frame and is the heart of the game's logic
- **Watch for indirect jumps** — jump tables are common in 68k code. You may need to trace them manually
- **Test frequently** — recompile a few functions at a time and test
- **Use Ghidra's decompiler** — it gives you a C-like view that can guide your translation, though the output isn't directly usable
- **Compare with MAME** — run the game in MAME's debugger alongside your recomp to verify behavior
