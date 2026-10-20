# Contributors

neogeorecomp exists because people who love the Neo Geo keep showing up.
Thank you to everyone who has contributed code, fixes, testing, or a hard-won
debugging insight. This file is the canonical record of who did what — the
CHANGELOG tells the story release-by-release, but credit lives here.

Reporting a bug counts. If you've contributed and aren't listed, or a line here
is wrong, open a PR against this file — we want every name right.

---

## Maintainer

### Ned Heller — [@sp00nznet](https://github.com/sp00nznet)
Project creator and maintainer. The MVS board model, the m68krecomp static
recompiler, the `--verify` block oracle, the conformance harness, and the
Metal Slug and Neo Drift Out ports.

---

## Contributors

### Danilo P. da Silva — [@danilopaulinodasilva](https://github.com/danilopaulinodasilva)
**Sound (#3)** — the first item on the roadmap, and the toolkit's first
outside contribution. Before it, the Z80 ran the real sound driver against
modelled YM2610 timers, and every game was silent.
- **YM2610 synthesis through [ymfm](https://github.com/aaronsgiles/ymfm)**
  (`src/ym2610.cpp`): FM, SSG and ADPCM-A/B, with ADPCM samples read straight
  from the V ROMs. ymfm only makes the sound; `audio.c` keeps owning the
  timers, status and IRQ the drivers sync to, so nothing the 68000 sees
  changed (`--verify` stays at 0 mismatches).
- **Sample-accurate register writes** — samples are generated up to the Z80's
  clock before every YM2610 write, at the chip's own rate (8 MHz / 144, one
  sample per 72 Z80 clocks), so each write lands on the right sample instead
  of being smeared across a frame. Port 2 now returns ymfm's real ADPCM end
  flags instead of 0.
- **Audio out** — SDL2 playback at the chip's rate with a 30–120 ms queue that
  pads on underrun and drops on stalls; `--wav FILE` (works headless) and
  `--mute`.
- Tested it on The King of Fighters '98 in his own
  [kof98recomp](https://github.com/danilopaulinodasilva/kof98recomp), built on
  a fork of this toolkit.
