# Neo Geo 68000 Memory Map

Complete reference for the Neo Geo's 16 MB address space as implemented by the `bus.c` module.

## Address Map Overview

```
$000000 ┌──────────────────────────────┐
        │  P ROM Bank 1 (1 MB)         │  Fixed — vectors + main game code
        │  Vector table at $000000     │  (swappable with BIOS vectors via REG_SWPROM)
$0FFFFF └──────────────────────────────┘
$100000 ┌──────────────────────────────┐
        │  Work RAM (64 KB)            │  Zero wait states
        │  $100000-$10F2FF: user       │
        │  $10F300-$10FFFF: system     │  Reserved by BIOS
$10FFFF └──────────────────────────────┘
$110000 ┌──────────────────────────────┐
        │  Work RAM Mirror             │  Repeats every 64 KB
$1FFFFF └──────────────────────────────┘
$200000 ┌──────────────────────────────┐
        │  P ROM Bank 2 (1 MB)         │  Bankswitchable (for ROMs > 2 MB)
        │  Write to select bank:       │  move.w #bank, $2xxxxx
$2FFFFF └──────────────────────────────┘
$300000 ┌──────────────────────────────┐
        │  I/O Registers               │  See detailed register map below
$3FFFFF └──────────────────────────────┘
$400000 ┌──────────────────────────────┐
        │  Palette RAM (8 KB)          │  2 banks x 256 palettes x 16 colors
        │  Mirrored through $7FFFFF    │
$401FFF └──────────────────────────────┘
$800000 ┌──────────────────────────────┐
        │  Memory Card (up to 8 MB)    │  2 wait cycles
        │  Not implemented in runtime  │
$BFFFFF └──────────────────────────────┘
$C00000 ┌──────────────────────────────┐
        │  System ROM / BIOS (128 KB)  │  Mirrored through $CFFFFF
$C1FFFF └──────────────────────────────┘
$D00000 ┌──────────────────────────────┐
        │  Backup RAM (64 KB)          │  MVS only, battery-backed SRAM
        │  Write-protected by default  │  Unlock via REG_SRAMUNLOCK
$D0FFFF └──────────────────────────────┘
```

## I/O Register Map ($300000-$3FFFFF)

### Input Registers (active low)

| Address    | Name          | R/W | Description |
|------------|---------------|-----|-------------|
| `$300000`  | REG_P1CNT     | R   | Player 1 joystick + ABCD buttons |
| `$300001`  | REG_DIPSW     | R/W | Read: DIP switches / Write: kick watchdog |
| `$300081`  | REG_SYSTYPE   | R   | Test button, slot config, system ID |
| `$320000`  | REG_SOUND     | R/W | Read: Z80 reply / Write: send command to Z80 (triggers NMI) |
| `$320001`  | REG_STATUS_A  | R   | Coin inputs 1-4, service button, RTC |
| `$340000`  | REG_P2CNT     | R   | Player 2 joystick + ABCD buttons |
| `$380000`  | REG_STATUS_B  | R   | Start/Select P1+P2, memory card status, AES/MVS flag |

### Controller Bit Layout (REG_P1CNT / REG_P2CNT)

```
Bit 7: D button
Bit 6: C button
Bit 5: B button
Bit 4: A button
Bit 3: Right
Bit 2: Left
Bit 1: Down
Bit 0: Up
```

All bits are active low (0 = pressed, 1 = released).

### System Control Registers ($3A00xx, write-only)

These registers use address bit 4 as the value. The data written is ignored.

| Address    | Name           | Effect |
|------------|----------------|--------|
| `$3A0001`  | REG_NOSHADOW   | Normal video output |
| `$3A0011`  | REG_SHADOW     | Darken entire display |
| `$3A0003`  | REG_SWPBIOS    | Use BIOS vector table at $000000 |
| `$3A0013`  | REG_SWPROM     | Use cartridge vector table at $000000 |
| `$3A000B`  | REG_BRDFIX     | Use BIOS fix tiles (SFIX) |
| `$3A001B`  | REG_CRTFIX     | Use cartridge fix tiles (S ROM) |
| `$3A000D`  | REG_SRAMLOCK   | Write-protect backup RAM |
| `$3A001D`  | REG_SRAMUNLOCK | Unprotect backup RAM |
| `$3A000F`  | REG_PALBANK1   | Select palette bank 1 |
| `$3A001F`  | REG_PALBANK0   | Select palette bank 0 |

### Video Registers (LSPC, $3C0000-$3C000E)

| Address    | Name           | R/W | Description |
|------------|----------------|-----|-------------|
| `$3C0000`  | REG_VRAMADDR   | R/W | Set VRAM address for read/write |
| `$3C0002`  | REG_VRAMRW     | R/W | Read/write VRAM data (auto-increments) |
| `$3C0004`  | REG_VRAMMOD    | R/W | Auto-increment value after each access |
| `$3C0006`  | REG_LSPCMODE   | R/W | Bits 15-8: current raster line; Bits 7-0: AA config |
| `$3C0008`  | REG_TIMERHIGH  | W   | Timer reload value bits 31-16 |
| `$3C000A`  | REG_TIMERLOW   | W   | Timer reload value bits 15-0 |
| `$3C000C`  | REG_IRQACK     | W   | Interrupt acknowledge (bit2=VBL, bit1=timer, bit0=reset) |
| `$3C000E`  | REG_TIMERSTOP  | W   | Bit 0: stop timer during PAL border lines |

## VRAM Layout

VRAM is **not memory-mapped** — accessed via REG_VRAMADDR / REG_VRAMRW / REG_VRAMMOD.

```
$0000 ┌──────────────────────────────┐
      │  SCB1: Sprite Tilemaps       │  381 sprites x up to 64 words each
      │  (tile numbers, palettes,    │  2 words per tile: tile# + attributes
      │   flip bits, auto-anim)      │
$6FFF └──────────────────────────────┘
$7000 ┌──────────────────────────────┐
      │  Fix Layer Tilemap           │  40x32 = 1280 entries
      │  Each: palette(4) + tile(12) │  Drawn top-to-bottom, left-to-right
$74FF └──────────────────────────────┘
$8000 ┌──────────────────────────────┐
      │  SCB2: Sprite Shrink         │  381 words: V shrink (byte) + H shrink (nibble)
$81FF └──────────────────────────────┘
$8200 ┌──────────────────────────────┐
      │  SCB3: Sprite Y + Height     │  381 words: Y pos, sticky bit, tile count
$83FF └──────────────────────────────┘
$8400 ┌──────────────────────────────┐
      │  SCB4: Sprite X              │  381 words: X position
$85FF └──────────────────────────────┘
```
