#ifndef WALTEX_KEYBOARD_H
#define WALTEX_KEYBOARD_H

#include "types.h"

/* Controller PS/2 8042. */
#define KBD_DATA   0x60
#define KBD_STATUS 0x64

/* Bit 0 dello stato: c'e' un byte da leggere sulla porta dati. */
#define KBD_STATUS_OUTPUT_FULL 0x01

/* Scancode set 1. Il break code di un tasto e' il suo make code con il bit 7
   acceso: 0x1E premi 'a', 0x9E lo rilasci. */
#define KBD_BREAK_BIT 0x80

#define KBD_SC_LSHIFT   0x2A
#define KBD_SC_RSHIFT   0x36
#define KBD_SC_ENTER    0x1C
#define KBD_SC_SPACE    0x39
#define KBD_SC_EXTENDED 0xE0    /* prefisso: il byte seguente va scartato */

/* Traduce uno scancode in carattere. Funzione pura: nessuno stato interno,
   nessun accesso all'hardware, quindi testabile sull'host.

   shift != 0 seleziona la tabella con i caratteri maiuscoli e i simboli.
   Ritorna -1 per tutto cio' che non e' un carattere stampabile o '\n': i
   modificatori, i break code, e gli scancode fuori tabella. */
int scancode_to_char(uint8_t scancode, int shift);

/* Registra il gestore dell'IRQ 1 e smaschera la linea. */
void keyboard_init(void);

/* Il carattere piu' vecchio in attesa, o -1 se non c'e' nulla.
   Va chiamata dal codice normale, mai da un interrupt handler: e' il
   consumatore del buffer, e il buffer ne ammette uno solo. */
int keyboard_getchar(void);

#endif
