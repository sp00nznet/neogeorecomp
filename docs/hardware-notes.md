# Hardware and toolchain notes

What took longest to find, so the next person doesn't lose the same day. Notes under *Observed* describe a failure actually hit while building this. Notes under *From references* are behaviour implemented from the NeoGeo Dev Wiki and MAME's driver that no title has yet been seen to depend on. They are recorded so nobody "simplifies" them away.

## Toolchain

### MSVC miscompiles Musashi's `m68kmake` at /O2

**Symptom:** access violation inside `m68ki_build_opcode_table` during `m68k_init`, before anything runs.

**Cause:** with MSVC 19.4x at `/O2`, the generator drops bit 14 of every opcode mask it writes to `m68kops.c` (`0xb000` instead of `0xf000`, `0xbfff` instead of `0xffff`). The table then no longer sorts the way `m68ki_build_opcode_table` assumes, and its first loop runs off the end. A Debug build of the same generator is correct.

**Fix:** `CMakeLists.txt` builds `m68kmake` with `/Od`. It runs once per build, so the cost is nothing. If Musashi is ever vendored differently, check the first entries of the generated table: `{m68k_op_1010, 0xf000, 0xa000, ...}`.

### Linux needs `-lm` for Musashi

Musashi's FPU code (compiled in even for a 68000) calls `sin`/`cos`. The fix is `target_link_libraries(musashi PUBLIC m)` on non-MSVC builds.

### Two targets must not share an output name

`m68krecomp` was once both the static library and the tool's `OUTPUT_NAME`. MSBuild then never relinked the tool after the library changed, so the conformance corpus was regenerated with a stale recompiler. That's why the library is `m68krecomp_core`.

## 68000

### Long stores to `-(An)` write the low word first

`MOVE.L Dn,-(An)` and `MOVEM.L regs,-(An)` store the low word at `addr+2` before the high word at `addr`. Plain memory doesn't care, but I/O registers do. Musashi models it, and `--verify` caught the emitter doing it the other way round.

### `.w` absolute addresses are sign-extended to 32 bits

`lea $FFFF.w,A0` loads `$FFFFFFFF`, not `$00FFFFFF`. The bus masks to 24 bits on access, so the emitter must keep all 32 bits of the address value; LEA/PEA expose it. (`--verify`: A0 native `00FFFFFF`, interp `FFFFFFFF`.)

### NBCD with a zero result writes nothing

When `0 - dst - X` is zero, NBCD clears C/X/V and leaves the destination untouched, with no write cycle. The conformance harness's `nbcd` program caught the helper storing `$9A`.

## Neo Geo: observed

### The backup RAM lock changes the map mid-routine

The BIOS unlocks backup RAM, writes a few bytes and locks it again inside one routine. Anything that caches "is this page read-only" across a block is wrong. The first version of `--verify` did, and reported false mismatches until it logged every data read.

## Neo Geo: from references

Implemented from the documentation. Metal Slug boots, renders and plays correctly with all of it, but none of it was found by watching something break.

### The byte-write quirk of the LSPC

The LSPC ignores the 68000's byte strobes. A `move.b` to `$3C0002` writes the byte to both halves of the data bus, so VRAM receives `$xxxx` with the byte in both halves (`v * 0x0101`).

### Switching the Z80 ROM restarts the Z80

When the BIOS hands over from its own sound driver (SM1) to the cartridge's (M1) through the fix-select latch, the Z80 must start the new driver from its reset vector.

### Z80 NMI is edge-triggered

A command raises NMI once, on the rise of (NMI enabled and a command pending), not on every write to the enable port.

### VRAM address increment wraps within its half

After a `VRAMRW` write, the address advances by `VRAMMOD` but keeps bit 15: `addr = (addr & 0x8000) | ((addr + mod) & 0x7FFF)`. The upper half is only 2K words (`$8000-$87FF`).

### The system latch is A1-A3 for the bit, A4 for the value

`$3A0001` + `2n` sets latch bit `n` to 0 and `$3A0011` + `2n` sets it to 1. The value written is ignored. The bits are shadow, vector select (0 = BIOS vectors), memory card ×3, fix/Z80 ROM select (0 = BIOS SFIX and SM1), backup RAM lock (0 = locked), and palette bank (0 = bank 1).

### Z80 bank selects use the upper byte of the port address

`in a,($08)` selects a bank from the value of A, which is on the upper address lines during the I/O cycle. `in r,(c)` uses B instead. The Z80 core passes only the low byte, so `audio.c` recovers the upper byte from the opcode just executed.

### Sprite rendering is per scanline, from the L0 ROM

Vertical shrink is not a scale factor. For each screen line, `000-lo.lo[(shrink << 8) | line]` gives the tile row and line to fetch. A height of 32 tiles draws the upper 16 and the mirrored lower 16, and 33 or more repeats the shrunk sprite down the screen. Horizontal shrink drops fixed pixels per the 16×16 table in `video.c`. Sticky sprites take the previous sprite's Y, height and vertical shrink, and sit at its X plus its width.
