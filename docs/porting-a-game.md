# Porting a game

Bringing up a new title needs no reverse engineering if the board model is right. A game repo describes its ROM set, builds a generator and a player, and that's all. [metalslug](https://github.com/sp00nznet/metalslug) is the worked example; copy its layout.

## 1. The repo

```
yourgame/
  neogeorecomp/          this toolkit, as a git submodule
  src/game.c, game.h     the ng_game_t (below)
  src/main.c             the player: ng_main(argc, argv, &game, table, count)
  src/recomp_main.c      the generator: ng_recomp_main(argc, argv, &game)
  tests/*.txt            input scripts for headless runs and the profile pass
  CMakeLists.txt         copy metalslug's
```

`.gitignore` must cover `roms/`, `build/`, `generated/` and `entries*.txt`. None of them may be committed (REPO_RULES section 3).

## 2. Describe the ROM set

Take the layout from MAME's `neogeo.cpp` set definition. P ROMs are stored byte-swapped (`ROM_LOAD16_WORD_SWAP`), so set `swap = 1`. P ROMs larger than 1 MB usually load their second half first. Metal Slug's is the example:

```c
const ng_game_t k_mslug = {
    .name = "mslug",
    .title = "Metal Slug - Super Vehicle-001",
    .prom_size = 0x200000,
    .prom = {
        {"201-p1.p1", 0x100000, 0x000000, 0x100000, 1},   /* file offset, dest, length, swap */
        {"201-p1.p1", 0x000000, 0x100000, 0x100000, 1},
    },
    .crom_size = 0x1000000,
    .crom_pairs = {{"201-c1.c1", "201-c2.c2"}, {"201-c3.c3", "201-c4.c4"}},
    .srom = "201-s1.s1",
    .m1 = "201-m1.m1",
    .vrom = {"201-v1.v1", "201-v2.v2"},
};
```

Check the layout by looking for the `NEO-GEO` signature at `$100` of the fixed bank.

## 3. Run it interpreted first

Build without `YOURGAME_ROM_DIR` (or run with `--interp`) and play headless:

```
yourgame --headless --interp --frames 3000 --screenshot 600:a.png --screenshot 2999:b.png
```

This tests the board model on its own. If the BIOS eyecatcher appears but the game doesn't, or graphics are wrong, the gap is in the runtime, not the recompiler. Fix it there, in generic terms (docs/hardware-notes.md lists what has come up so far). Never add title-specific addresses or patches to the toolkit.

## 4. Recompile

Configure with the ROM directory and build. The generator runs automatically:

```
cmake -S . -B build -DYOURGAME_ROM_DIR=C:/path/to/roms
cmake --build build --config Release
```

Then check correctness and coverage:

```
yourgame --headless --verify --frames 3000 --input tests/coin_start.txt
```

Expect `0 mismatches`. If a mismatch appears, the report names the block and shows where the native and interpreted runs diverged; fix the emitter or a `cpu.h` helper, and add the case to the conformance harness if the corpus doesn't cover it.

## 5. Profile

Write input scripts that reach the game's modes: attract, play, continue, game over. Then run the `profile` target and rebuild. The last `[exec]` line should reach `interpreted instructions 0` for your scripts. Anything left over still runs correctly in the interpreter, just not natively.

## 6. Document

Add the game to the toolkit README's compatibility table. Give the game repo a README with real screenshots and a Getting Started that works from a clean folder.
