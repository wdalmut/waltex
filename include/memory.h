#ifndef WALTEX_MEMORY_H_
#define WALTEX_MEMORY_H_

#include "types.h"

/* Semantica standard: n conta BYTE. memcpy non supporta regioni sovrapposte. */
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* Riempimento con un pattern a 16 bit, che memset non puo' esprimere perche'
   replica un singolo byte. count conta ELEMENTI, non byte: e' il punto della
   funzione, ed e' cosi' anche in Linux, che ha memset16/32/64 in
   include/linux/string.h per la stessa ragione. Primo utilizzatore qui: il
   framebuffer VGA, fatto di celle da 16 bit. */
void *memset16(uint16_t *dest, uint16_t value, size_t count);

#endif