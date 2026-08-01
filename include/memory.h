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

/* Confronto fra stringhe con la semantica standard: 0 se uguali, negativo o
   positivo secondo il primo byte che differisce.

   Sta qui e non in shell.c perche' il primo utilizzatore e' la ricerca nella
   tabella dei comandi di M7, ma il secondo e' il VFS di M9, che confronta i
   nomi dei file: la casa naturale e' questa. */
int strcmp(const char *a, const char *b);

size_t strlen(const char *a);

/* Posizione della prima occorrenza di c in s, oppure -1 se non c'e'.

   Non e' una funzione standard: l'equivalente C e' strchr, che restituisce un
   puntatore oppure NULL. Un indice e' una scelta di questo progetto, e -1 come
   "non trovato" e' l'unico valore che non si puo' confondere con una posizione
   valida — restituire la lunghezza lo renderebbe indistinguibile dall'aver
   trovato il carattere in ultima posizione.

   Il terminatore NON e' cercabile: strpos(s, '\0') ritorna sempre -1. Qui
   divergiamo da strchr, che invece lo trova. La funzione serve a cercare
   caratteri visibili — lo spazio che separa due parole, il prefisso di un
   numero — e per quell'uso "la stringa finisce dove finisce" e' la definizione
   utile. E' scritto qui perche' sia una scelta e non una sorpresa. */
int strpos(const char *s, char c);

char tolower(char argument);

/* Il primo carattere c in s, come PUNTATORE, oppure 0 se non c'e'.

   Questa e' la funzione standard, e la firma e' quella standard: `int c` e non
   `char c`, e ritorno `char *`. Non e' pedanteria — <string.h> e' fra gli header
   piu' inclusi che esistano, e una dichiarazione incompatibile con quella di
   glibc e' un errore di compilazione secco, non un warning.

   ATTENZIONE alla differenza con strpos, che e' deliberata e non una svista:

     strchr(s, '\0')   TROVA il terminatore e ne restituisce l'indirizzo
     strpos(s, '\0')   ritorna sempre -1

   strchr ha un nome standard, quindi deve onorare il contratto standard, e
   quello include il terminatore nella ricerca. strpos e' nostra, serve a cercare
   caratteri visibili, e lo dichiara nel proprio commento.

   Il primo utilizzatore e' vfs_resolve in M9a, che con questa trova la fine di
   un componente del path. */
char *strchr(const char *s, int c);

#endif