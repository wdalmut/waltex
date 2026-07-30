#ifndef WALTEX_LINEEDIT_H
#define WALTEX_LINEEDIT_H

#include "types.h"

/* Quanto e' lunga una riga di comando, NUL finale compreso: la capacita' utile
   e' quindi LINE_MAX - 1 caratteri. Statico come tutto il resto. */
#define LINE_MAX 128

/* L'editor di riga: accumula caratteri finche' non arriva un Invio, e nel
   frattempo gestisce il backspace.

   Il sink di eco e' un puntatore a funzione, e non e' un vezzo: e' lo stesso
   espediente per cui kvprintf riceve il suo putc invece di chiamare la VGA. Se
   questo modulo chiamasse kprintf direttamente non si potrebbe compilare
   sull'host, e soprattutto il test non potrebbe verificare COSA e' stato
   echeggiato — solo che il programma non e' morto.

   Con il sink, il test passa una funzione che accoda in un buffer e confronta
   byte per byte, compresa la sequenza di tre caratteri con cui si cancella.

   Conseguenza da rispettare: lineedit.c non include ne' kprintf.h ne' vga.h.
   Se lo fa, il test host non linka piu' e il modulo ha perso la sua ragione. */
struct lineedit {
    char buf[LINE_MAX];
    int  len;
    void (*echo)(char c);       /* 0 = nessun eco, ed e' lecito */
};

/* Collega il sink e azzera la riga. Da chiamare UNA volta, non a ogni
   carattere: azzera len, quindi rifarlo nel ciclo cancellerebbe la riga mentre
   la si sta scrivendo. Per ricominciare dopo un comando c'e' lineedit_reset. */
void lineedit_init(struct lineedit *le, void (*echo)(char c));

/* Consuma un carattere.

   Ritorna 1 se la riga e' completa: allora buf e' terminato da NUL e len e' la
   sua lunghezza. Ritorna 0 altrimenti.

   I casi sono quattro e non ce ne sono altri:
     '\n'                 riga finita, anche se vuota
     '\b'                 cancella, ma solo se c'e' qualcosa da cancellare
     carattere >= 32      accoda, se c'e' posto
     tutto il resto       scartato in silenzio (tab, ESC, altri controlli) */
int lineedit_putc(struct lineedit *le, char c);

/* Azzera la riga conservando il sink. */
void lineedit_reset(struct lineedit *le);

#endif
