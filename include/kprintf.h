#ifndef WALTEX_KPRINTF_H
#define WALTEX_KPRINTF_H

/* stdarg.h è un header del compilatore, non della libc: disponibile anche in
   freestanding. */
#include <stdarg.h>

#include "types.h"

/* Il cuore del formatter è separato dalla destinazione: riceve la funzione a
   cui consegnare un carattere alla volta. Questo lo rende compilabile e
   testabile col gcc dell'host, senza VGA né seriale.

   Specificatori richiesti: %d (int32 con segno), %x (uint32 esadecimale
   minuscolo, senza zeri iniziali), %s, %c, %%. Uno specificatore non
   riconosciuto viene emesso letteralmente, preceduto dal suo '%'.

   Casi limite, entrambi coperti dai test:
   - un '%' come ultimo carattere della stringa emette '%' e termina, senza
     leggere oltre il terminatore;
   - %s con puntatore nullo stampa "(null)" invece di dereferenziarlo: il
     chiamante tipico di questo caso è il panic handler, e lì un crash dentro
     il formatter nasconderebbe l'errore vero. */
void kvprintf(void (*putc)(char), const char *fmt, va_list ap);

/* Scrive su VGA e su COM1. */
void kprintf(const char *fmt, ...);

#endif
