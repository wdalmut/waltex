#ifndef WALTEX_RING_H
#define WALTEX_RING_H

#include "types.h"

/* Potenza di due: avanzare un indice diventa un AND invece di una divisione,
   e questo codice gira dentro un interrupt handler. */
#define RING_SIZE 128
#define RING_MASK (RING_SIZE - 1)

/* Buffer circolare a produttore singolo e consumatore singolo.

   La proprieta' che lo rende sicuro senza alcun lock, e che va rispettata alla
   lettera:

     head e' scritto SOLO dal produttore.
     tail e' scritto SOLO dal consumatore.
     Ognuno dei due legge l'indice dell'altro, e non lo modifica mai.

   Con un solo scrittore per indice non serve nessun cli: su i386 leggere un
   uint32_t allineato e' una singola istruzione, quindi nessuno dei due puo'
   osservare un valore a metà aggiornamento. Il consumatore puo' leggere un
   head un po' vecchio — vedra' il buffer piu' vuoto di quanto sia — e questo
   e' innocuo, perche' al giro successivo lo vedra'.

   Perche' non c'e' un contatore degli elementi: sarebbe incrementato dal
   produttore e decrementato dal consumatore, cioe' scritto da entrambi. Un
   count++ e' leggi-modifica-scrivi, l'interrupt puo' cadere nel mezzo, e i due
   aggiornamenti si perdono a vicenda. Introdurre un contatore distrugge
   esattamente la proprieta' che rende questo buffer corretto.

   Conseguenza: senza contatore, "pieno" e "vuoto" sarebbero entrambi
   head == tail. Si sacrifica uno slot — pieno significa "il prossimo head
   raggiungerebbe tail" — quindi la capacita' utile e' RING_SIZE - 1. */
struct ring {
    volatile uint8_t  buf[RING_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
};

void ring_init(struct ring *r);

/* Accoda un valore. Ritorna 1 se accodato, 0 se il buffer e' pieno.
   A buffer pieno il valore si perde e nulla viene sovrascritto: perdere un
   tasto e' accettabile, corrompere la coda no. Chiamabile solo dal produttore. */
int ring_push(struct ring *r, uint8_t v);

/* Estrae il valore piu' vecchio, o -1 se il buffer e' vuoto.
   Chiamabile solo dal consumatore. */
int ring_pop(struct ring *r);

int ring_empty(const struct ring *r);

#endif
