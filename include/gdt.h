#ifndef WALTEX_GDT_H
#define WALTEX_GDT_H

/* Un selettore di segmento e' indice * 8, piu' due bit di RPL e uno di TI che
   qui valgono zero. Indice 1 = codice, indice 2 = dati. */
#define GDT_SEL_CODE 0x08
#define GDT_SEL_DATA 0x10

#ifndef __ASSEMBLER__

#include "types.h"

/* Costruisce la tabella e la carica. Dopo questa chiamata la CPU non usa piu'
   la GDT del bootloader. */
void gdt_init(void);

/* Definita in gdt.S. Riceve il puntatore alla struttura a 6 byte che lgdt si
   aspetta: 2 byte di limite seguiti da 4 di base. */
void gdt_flush(void *gdtr);

#endif /* __ASSEMBLER__ */
#endif
