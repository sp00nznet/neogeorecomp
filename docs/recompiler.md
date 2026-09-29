# m68krecomp

`m68krecomp` turns the 68000 code of a Neo Geo program ROM and system ROM into C. The code is in `tools/m68krecomp/`: `decode.c` decodes instructions, `recomp.c` handles discovery and output, and `emit.c` lowers instructions to C. Games link it as `m68krecomp_core` into a small generator, and the `m68krecomp` tool uses it for flat images.

## Input

The generator loads the program exactly as the runtime does, through `src/romload.c` and the game's `ng_game_t`. It sees three regions:

| Region | Base | Contents |
|---|---|---|
| prom | `$000000` | the fixed 1 MB of the P ROM |
| bank | `$200000` | the first banked MB, if the P ROM is larger than 1 MB |
| bios | `$C00000` | the 128 KB system ROM |

## Discovery

Discovery is recursive descent from seeds. Each seed becomes one **routine**: everything reachable from it without leaving through a call, return or jump. Branches, `Bcc` and `DBcc` stay inside, and `BSR`/`JSR` continue at their return address. The seeds are:

- **Vectors 1-63** of both the system ROM and the cartridge.
- **The cartridge header entry points** the BIOS calls: USER `$122`, PLAYER_START `$128`, DEMO_END `$12E`, COIN_SOUND `$134`.
- **Static targets**: `BSR`, and `JSR`/`JMP` to absolute or PC-relative addresses.
- **Jump tables**, in the three shapes 68000 switch statements take:
  - `movea.l tbl(pc,d0.w),a0 / jmp (a0)`: a table of absolute pointers
  - `move.w tbl(pc,d0.w),d0 / jmp base(pc,d0.w)`: a table of 16-bit offsets
  - `jmp tbl(pc,d0.w)` into a run of `bra` instructions

  Table bases loaded with `lea tbl(pc),An` are tracked too.
- **Code pointers**: targets of PC-relative `lea`/`pea`, 32-bit immediates, and (by default) every aligned 32-bit value in the program. Neo Geo games run objects as coroutines. They store a resume address with `lea (next,pc),a1 / move.l a1,(a6)` and later reach it with `movea.l (a6),a0 / jmp (a0)`, so a lot of code is only ever referenced from data. `--no-pointer-scan` turns the scan off.
- **`--entries FILE`**: addresses a previous run found only at runtime (below).

Every candidate that isn't a certain code address, meaning pointers and table entries, must pass `plausible_code()`. Following fall-through, it has to reach a return or jump within 256 instructions without hitting an illegal opcode, line-A/line-F, or `ori.b #0,d0` (the decode of zeroed data, which no assembler emits). A table scan stops at its first entry that fails.

A wrong guess is safe. The same address always decodes to the same instructions, and the dispatcher only ever enters a routine at an address control really reaches. So a data word mistaken for code costs some output size and nothing else.

Metal Slug numbers: static discovery alone reaches 445,072 instructions across 15,006 routines (duplication included) and 33,423 dispatch entries. Without the jump-table and pointer passes, it found 3,007.

## Runtime-only entry points

Some targets can't be known statically, for example a computed jump into the middle of an unrolled loop. The runtime interprets those with Musashi, and `--dump-misses FILE` records every address where interpretation started or landed after a jump. The file merges across runs, and feeding it back with `--entries` adds those seeds. A game's `profile` target automates this: it runs scripted sessions headless, then the next build regenerates. For Metal Slug, one profile pass (attract mode plus a 5-minute play script) takes both sessions to 0 interpreted instructions.

The entry list is derived from the game, so it lives in the build directory and is never committed.

## Output

Each routine becomes one C function in the generated files (16 of them for Metal Slug, a fixed number so the build system knows the outputs in advance). The function opens with a `switch` over the addresses the dispatch table maps to it: its seed, and its **resume points**. Those are return addresses after calls, loop heads, and the instruction after an SR write. Every instruction gets a label and a comment with its address and disassembly, and becomes one C block:

```c
L_01004C: /* 01004C: dbra    D0, $10048 */
    { NG_INSN(12); if (!CC_F) { uint16_t cnt = (uint16_t)(CPU.d[0] - 1);
      CPU.d[0] = (CPU.d[0] & 0xFFFF0000u) | (uint16_t)(cnt);
      if (cnt != 0xFFFF) { ng_cycles -= 2;
        if (ng_cycles >= ng_next_event) { CPU.pc = 0x010048u; return; }
        goto L_010048; } } ng_cycles += 2; }
```

- `NG_INSN(n)` adds the instruction's base cycles (from Musashi's 68000 table) and counts it for `--verify`. Dynamic costs are added where the 68000 has them: shift counts, MOVEM registers, branches taken or not.
- Operands are fetched in the 68000's order, source EA before destination EA, so `(An)+` and `-(An)` side effects match.
- Arithmetic goes through the helpers in `cpu.h` (`ng_add16`, `ng_subx32`, `ng_asl8`, `ng_abcd`, ...). They reproduce Musashi's results bit for bit, including undocumented flags.
- Control that leaves the routine sets `CPU.pc` and returns: `JSR`/`BSR` push the return address on the emulated stack, `RTS` pops it, and exceptions build a real frame.
- `recomp_table.c` lists every entry address and its routine; the game passes it to `ng_main()`.

## Why control flow is flat

A routine never calls another routine directly. `JSR` pushes the return address and returns to the dispatcher, which looks up the target. This costs one table lookup per call and return, and buys exactness:

- Return addresses are real values on the real stack. Code that pops its return address, pushes a different one, or `RTS`s into a jump table behaves as on hardware. Neo Geo code does all three.
- Interrupts are taken between routines, or at loop back-edges once `ng_next_event` passes, as genuine 68000 exceptions. The handler's `RTE` resumes at an address that is always a dispatch entry.
- The C stack never grows with the 68000's call depth.

The dispatcher's lookup is a direct array index per region, so the cost is small. Metal Slug runs 60 seconds of game time in about 1.9 s headless, and rendering dominates.

## Checking the output

Two independent checks, neither of which involves timing:

- **`--verify`** (runtime flag): every native block runs with the bus recording its data accesses. Then Musashi starts from the same registers and runs the same number of instructions. Instruction fetches come straight from ROM; every data read is served from the recording, so I/O and RAM look exactly as they did. Registers, flags and the sequence of writes must match exactly, and a mismatch prints both access sequences and the instruction trace. On Metal Slug's 5-minute soak it checks 14.06M blocks with 0 mismatches.
- **The conformance harness** (docs/conformance.md): Musashi's 68000 test programs, recompiled and run natively.

Bugs they found while this was built, each now fixed:

| Bug | Found by |
|---|---|
| `MOVE.L`/`MOVEM.L` to `-(An)` stores the low word first on a 68000; the emitter stored the high word first | `--verify` (matters for I/O registers) |
| `lea $FFFF.w,An` must load `$FFFFFFFF`; the emitter masked absolute addresses to 24 bits | `--verify` |
| `NBCD` with a zero result leaves the destination unwritten; the helper stored `$9A` | conformance |

## The standalone tool

For flat images, not Neo Geo sets:

```
m68krecomp --raw FILE --base ADDR --entry ADDR [--entry ADDR]... [--prefix P] [--out DIR] [--files N]
```

`--prefix` is prepended to every emitted symbol and file name, so several images can link into one program; the conformance harness relies on this.
