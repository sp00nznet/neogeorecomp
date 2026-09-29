# Roadmap

## Next

- **Sound.** The Z80 already runs the real driver. What's missing is YM2610 synthesis (FM, SSG, ADPCM-A/B from the V ROMs) and an audio path: SDL output, plus the audio track in `--record`. The plan is to wrap [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3) behind `audio.c`'s current register interface.
- **Port Neo Drift Out to this runtime** and delete its per-game overrides. The old runtime hardcoded Drift Out's USER entry, patched its BIOS state byte, and forced sprite palettes and shrink. All of that should disappear once the real BIOS and board model run it; anything that doesn't should be fixed here, generically.
- **Load ROMs from zip files**, so users can point at `mslug.zip` + `neogeo.zip` instead of unzipping.

## Later

- AES mode and AES system ROMs; the Unibios.
- Memory card.
- Multi-bank P ROMs (games over 2 MB): the dispatch table currently assumes bank 0 at `$200000`.
- Protected and encrypted sets (CMC, PVC, SMA): they need decryption before the recompiler sees the code.
- PAL timing and `TIMERSTOP`.
- Flag liveness in the emitter, to drop dead flag computations (speed; correctness doesn't need it).
- A frontend with settings, controller mapping and save states, in the style of lynxrecomp's.

## Out of scope

- Shipping ROMs, system ROMs, or recompiled output. Users always supply their own dump.
- Title-specific patches in the toolkit. Game repos describe their ROM layout and nothing else.
