#ifndef WALTEX_IO_H
#define WALTEX_IO_H

#include "types.h"

/* Le porte I/O dell'x86 sono uno spazio di indirizzamento separato dalla
   memoria: si raggiungono solo con le istruzioni in/out. Il vincolo "Nd"
   permette al compilatore di usare la forma immediata per porte < 256. */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Scrivere sulla porta 0x80 (usata solo per il POST code) costa circa un ciclo
   di bus e non ha effetti collaterali: è il modo canonico per dare a un chip
   lento il tempo di reagire fra due out consecutive. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* Trasferimenti a blocchi fra una porta e la memoria, una WORD per volta.
   Servono da M10: la porta dati dell'ATA è a 16 bit e un settore sono 256
   word, quindi count si conta in word e non in byte — è il modo più facile di
   riempire mezzo buffer e lasciare l'altra metà del settore dentro il disco a
   confondere il comando successivo.

   Il cld è obbligatorio e non è pedanteria: rep insw avanza nella direzione del
   flag DF, e niente garantisce come lo si trova. Senza, il buffer si riempie
   all'indietro — leggibile, ordinato, sbagliato.

   "+D" e "+S" perché edi/esi vengono incrementati dall'istruzione, "+c" perché
   ecx viene decrementato: sono ingressi E uscite. La clobber "memory" dice al
   compilatore che il buffer è cambiato senza che lui abbia visto una scrittura,
   e senza di essa può tenere in un registro un valore letto prima. */
static inline void insw(uint16_t port, void *addr, uint32_t count)
{
    __asm__ volatile ("cld; rep insw"
                      : "+D"(addr), "+c"(count)
                      : "d"(port)
                      : "memory");
}

static inline void outsw(uint16_t port, const void *addr, uint32_t count)
{
    __asm__ volatile ("cld; rep outsw"
                      : "+S"(addr), "+c"(count)
                      : "d"(port)
                      : "memory");
}

#endif
