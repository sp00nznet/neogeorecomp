#ifndef NG_ROMLOAD_H
#define NG_ROMLOAD_H
#include <stdint.h>
#include <stddef.h>
#include <neogeorecomp/neogeo.h>

uint8_t *ng_read_file(const char *dir, const char *name, size_t *size);
void     ng_swap16(uint8_t *p, size_t n);
uint8_t *ng_load_prom(const char *dir, const ng_game_t *g);   /* g->prom_size bytes, 68k order */
uint8_t *ng_load_crom(const char *dir, const ng_game_t *g);   /* odd/even pairs interleaved */

#endif
