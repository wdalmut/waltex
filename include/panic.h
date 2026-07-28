#ifndef WALTEX_PANIC_H
#define WALTEX_PANIC_H

#include "types.h"
#include "idt.h"

/* Stampa il messaggio e ferma la macchina. Non ritorna mai.

   Vincolo che rende questa funzione diversa da tutte le altre: gira quando
   qualcosa e' gia' rotto. Non deve allocare, non deve ricorrere, e non deve
   dipendere da sottosistemi che potrebbero essere proprio quelli guasti. */
void panic(const char *fmt, ...) __attribute__((noreturn));

/* Come panic, ma stampa anche il dump completo dei registri. La chiama il
   gestore delle eccezioni, che ha lo stato della CPU sotto mano. */
void panic_regs(struct regs *r) __attribute__((noreturn));

/* Sempre attiva, mai compilata via: nel kernel proseguire dopo un'asserzione
   violata significa corrompere memoria arbitraria. Non introdurre NDEBUG. */
#define assert(cond)                                                    \
    do {                                                                \
        if (!(cond))                                                    \
            panic("assert fallita: %s (%s:%d)", #cond, __FILE__, __LINE__); \
    } while (0)

#endif
