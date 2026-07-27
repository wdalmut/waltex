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

#endif
