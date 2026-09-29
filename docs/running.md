# Running a game

Every game built on neogeorecomp is one executable whose `main()` calls `ng_main()`, so they all take the same options. The examples use Metal Slug's `metalslug`.

## ROM files

The runtime reads unzipped files from `--rom-path` (default `roms`):

- **The game's set**, named as in MAME, e.g. `201-p1.p1`, `201-s1.s1`, `201-c1.c1`..`201-c4.c4`, `201-m1.m1`, `201-v1.v1`, `201-v2.v2`.
- **From the `neogeo` system set** (`--bios-path`, default the ROM path): `sp-s2.sp1` (MVS Asia/Europe v2, selectable with `--bios`), `sfix.sfix`, `sm1.sm1`, and `000-lo.lo` (the sprite shrink table).

A missing file is reported by name:

```
error: cannot open roms/201-p1.p1
error: missing ROM files; see docs/running.md for the expected set
```

## Options

```
  --rom-path DIR        game ROM files (default: roms)
  --bios-path DIR       system ROM files (default: the ROM path)
  --bios FILE           system ROM (default: sp-s2.sp1, MVS Asia/Europe)
  --headless            no window; run as fast as possible
  --record FILE.mp4     pipe frames to ffmpeg
  --frames N            stop after N frames
  --screenshot N:FILE   save frame N as PNG (repeatable)
  --input FILE          scripted input (see docs/running.md)
  --interp              run everything in the interpreter
  --verify              re-run every recompiled block in the interpreter and compare
  --dump-misses FILE    write interpreted entry points for the recompiler
  --scale N             window scale (default 3)
```

Frame numbers count 59.19 Hz video frames from power-on. The MVS BIOS eyecatcher finishes at about frame 600, and Metal Slug's title appears at about frame 1200.

## Windowed play

Keys: arrows; **Z X C V** = A B C D; **5** = coin; **1** = start (P1); **2** = start (P2); **3** = select; **9** = service; **F2** = test switch; **Esc** = quit. There is no sound yet.

On an MVS you insert a coin before Start does anything.

## Headless runs

`--headless` opens no window and runs unpaced. It works over Remote Desktop and in CI, and it is how everything in these docs was checked. Useful combinations:

```
# a video of the first 32 seconds, coin and start included
metalslug --headless --frames 1900 --input tests/coin_start.txt --record run.mp4

# stills
metalslug --headless --frames 1801 --screenshot 600:bios.png --screenshot 1800:game.png
```

`--record` needs `ffmpeg` on `PATH`. It writes H.264 at 640×448 (2× nearest-neighbour).

## Input scripts

One event per line: `<frame>[-<end>] <button>[+<button>...]`. A single frame holds for 6 frames, which is long enough for the BIOS to register a coin. `#` starts a comment.

```
# tests/coin_start.txt
900 coin
1000 start
1300-1700 right
1400 a
1550 b
```

Buttons: `up down left right a b c d` for player 1; `p2up p2down p2left p2right p2a p2b p2c p2d`; `start start2 select coin coin2 service test`.

With the same script, a run is deterministic, down to identical frames on Windows and Linux.

## Checking recompiled code

`--verify` re-runs every recompiled block in Musashi against the same memory and I/O values and compares the results (docs/recompiler.md). It is slower, and it is the first thing to try when something looks wrong:

```
metalslug --headless --verify --frames 1801 --input tests/coin_start.txt
[neogeorecomp] mslug: 33613 recompiled entry points
[verify] 961369 blocks checked, 0 mismatches
[exec] native blocks 961369, interpreted instructions 0, 0 uncovered entry points
```

A mismatch prints the block, both bus-access sequences side by side, the differing registers, and the instruction trace.

`--interp` runs the whole machine in Musashi. Comparing a screenshot from `--interp` with the same frame from the recompiled build is a quick end-to-end check; for Metal Slug they are byte-identical.

## Coverage

The last `[exec]` line reports how much ran natively: blocks entered natively, instructions interpreted, and addresses where interpretation began. Set `NG_PROFILE=1` to also list the entry points that spent the most instructions in the interpreter:

```
[profile]     179607 interpreted from $05BE66  move.l  D7, (A4)
```

`--dump-misses FILE` merges those addresses into `FILE`, and the recompiler takes them with `--entries`. Games wrap this in a `profile` target (docs/porting-a-game.md).

## Crash reports

On Windows an unhandled crash prints a symbolized stack (build `RelWithDebInfo` for file and line numbers):

```
*** crash: exception C0000005 at 00007FF6BC898A1E
  note_miss  src/exec.c:159
  interpret  src/exec.c:175
  ...
```
