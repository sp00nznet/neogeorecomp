# Roadmap

## Next

- **Sound in `--record`.** Sound plays in the window and `--wav` writes it; `--record` still makes silent videos.
- **Load ROMs from zip files**, so users can point at `mslug.zip` + `neogeo.zip` instead of unzipping.

## Later

- AES mode and AES system ROMs; the Unibios.
- Memory card.
- Multi-bank P ROMs (games over 2 MB): the dispatch table currently assumes bank 0 at `$200000`.
- Protected and encrypted sets (CMC, PVC, SMA): they need decryption before the recompiler sees the code.
- PAL timing and `TIMERSTOP`.
- Flag liveness in the emitter, to drop dead flag computations (speed; correctness doesn't need it).
- A frontend with settings, controller remapping and save states, in the style of lynxrecomp's.

## Out of scope

- Shipping ROMs, system ROMs, or recompiled output. Users always supply their own dump.
- Title-specific patches in the toolkit. Game repos describe their ROM layout and nothing else.
