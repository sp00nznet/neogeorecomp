# Contributing

Thanks for looking. Issues and pull requests are welcome, and so are bug reports from running a game you own.

## Before you send a PR

- **Build and run the conformance harness** (`ctest --test-dir build -C Release`). It must not drop below `tests/conformance/BASELINE`. If your change makes more programs pass, raise the baseline in the same PR.
- **If you touched the recompiler or `cpu.h`, run a game with `--verify`** and include the `[verify]` line in the PR description, e.g. `[verify] 14056442 blocks checked, 0 mismatches`.
- **Keep the toolkit generic.** A fix that only makes sense for one title (an address, a patch, a forced value) belongs in that game's repo, or better, as a missing piece of hardware behaviour here. Say which titles the change affects and whether it is on by default.
- **It must build on MSVC and gcc.** CI checks both.

## What must never be in a PR

ROMs, system ROMs, recompiler output, entry-point lists, disassembly listings, or anything else derived from a game or system ROM. This includes snippets in docs, tests, and issue text; use the Musashi test corpus for examples instead.

## Where your code comes from

Contributions must be original or under an MIT-compatible licence. The easy mistake is porting a fix you saw in a GPL project, which relicenses it by accident and is very hard to undo. MAME, FBNeo, Mednafen and Geolith are the projects most likely to be open in another window. Their licences range from GPL to non-commercial, and none of them is MIT as a whole. Use them to understand the hardware, then write the code yourself, and say in the PR what you used as a reference. We will ask where anything unusually polished came from.

AI-assisted contributions are welcome, provided a human understood and verified the change.

## Style

Match the surrounding code. Comments say why, not what. Anything longer than a paragraph goes in `docs/`, linked from the code. A doc a change invalidates is part of that change.
