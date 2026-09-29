# Conformance harness

`ng_conformance` checks the recompiler against a fixed corpus that needs no game data, so it runs in CI on every push.

## The corpus

[Musashi](https://github.com/kstenerud/Musashi) ships 60 self-checking 68000 test programs (`ext/musashi/test/mc68000/*.bin`, MIT). Each one exercises an instruction family (`add`, `abcd`, `movem`, `shifts`, `divs`, ...) across operand sizes, addressing modes and flag cases. It accumulates results and flags into checksums, compares them with values captured from real hardware, and reports by writing to `TEST_PASS_REG` (`$100004`) or `TEST_FAIL_REG` (`$100000`).

## What the harness does

At build time CMake runs `m68krecomp --raw` on every program (`--prefix t_<name>_`, so they link side by side). The harness then, for each program:

1. sets up the memory map of Musashi's own test driver: RAM at `$0` with vectors (SSP `$3F0`, PC `$10000`, the rest `$DEADBEEF`), the program at `$10000`, the test device at `$100000`, and RAM at `$300000`.
2. runs it in **Musashi as a plain 68000**: the reference column.
3. runs it **natively**, through the recompiled routines only. There is no interpreter fallback, so reaching an address the recompiler didn't find is a failure.

A program passes when it reports pass and never fail.

```
$ ng_conformance --corpus ext/musashi/test/mc68000
  move             FAIL  reached code the recompiler did not find (pc $ADBEEF); reference FAIL
conformance: 59/60 68000 test programs pass recompiled (reference interpreter: 59/60)
```

`-v` lists every program, and `--only NAME` runs one.

## Reading the numbers

**59/60, the same as the reference.** `move.bin` fails under Musashi too when Musashi is a 68000. At `$010160` it uses `cmpi` with a PC-relative operand (opcode `$0C3A`), which is 68020+; on a 68000 it takes the illegal-instruction vector, which the test leaves at `$DEADBEEF`. The corpus was written for Musashi's 68040 driver. A failure the reference shares is a corpus expectation; one it doesn't share is a recompiler bug.

## Regressions

`tests/conformance/BASELINE` holds the pass count CI must reach (`ctest` passes `--min-pass`). Dropping below it fails the build, even if most programs still pass:

```
REGRESSION: 58 passing is below the baseline of 59
```

Raise `BASELINE` when a change makes more programs pass. Never lower it to get a change through.

## History

| Date | Recompiled | Reference | Note |
|---|---|---|---|
| 2026-09-29 | 58/60 | 59/60 | first run: `nbcd` wrote `$9A` for a zero result |
| 2026-09-29 | 59/60 | 59/60 | NBCD fixed |

## The other check

Conformance proves instruction semantics on a known corpus. `--verify` (docs/running.md) proves them on a real game, block by block, against the same reference. The two found different bugs (docs/recompiler.md).
