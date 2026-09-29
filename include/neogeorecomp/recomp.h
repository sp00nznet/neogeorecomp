/*
 * recomp.h — the recompiler's entry point for a game repo's generator.
 *
 * A game builds a small generator executable whose main() calls
 * ng_recomp_main() with the same ng_game_t its player uses. It reads the
 * user's own ROM files and writes C sources into an output directory
 * (docs/recompiler.md). Nothing it writes is ever committed.
 */
#ifndef NEOGEORECOMP_RECOMP_H
#define NEOGEORECOMP_RECOMP_H

#include "neogeo.h"

#ifdef __cplusplus
extern "C" {
#endif

int ng_recomp_main(int argc, char **argv, const ng_game_t *game);

#ifdef __cplusplus
}
#endif
#endif
