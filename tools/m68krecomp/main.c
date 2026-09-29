/*
 * main.c - the standalone m68krecomp tool, for flat 68000 images (the
 * conformance corpus). Neo Geo games use ng_recomp_main() from their own
 * generator instead, so the ROM layout comes from the game's ng_game_t.
 */
#include "recomp.h"

int main(int argc, char **argv) { return rc_raw_main(argc, argv); }
