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
     il formatter nasconderebbe l'errore vero.

   IL SINK RICEVE UN CONTESTO, da M11d+. È l'aggiunta che rende `vsnprintf`
   corretta sotto prelazione: un sink che scrive in un buffer deve sapere QUALE
   buffer, e senza questo parametro deve trovarselo in una variabile globale.
   Una globale è sicura all'annidamento — basta salvarla e ripristinarla — ma non
   all'INTRECCIO: se un task viene prelazionato a metà di una `snprintf`, il
   successivo gli scrive dentro il buffer.

   Chi non ne ha bisogno passa 0 e lo ignora con `(void)ctx`. Costa un argomento
   sullo stack per carattere e compra il fatto che il formatter non abbia più
   nessuno stato proprio. */
void kvprintf(void (*putc)(void *ctx, char c), void *ctx,
              const char *fmt, va_list ap);

/* Il sink della console: scrive OGNI carattere su COM1 e sulla VGA.

   Serve a `panic`, che ha bisogno di formattare senza passare da `kprintf`
   perché cambia il colore prima e si ferma dopo. Esportarlo evita che panic.c si
   riscriva i propri adattatori — e soprattutto evita che rifaccia il bug che
   aveva: due `kvprintf` con lo STESSO `va_list`, cioè il debito di M1 che
   `kprintf` si è tolto con `va_copy` e che in panic era rimasto.

   Con questo, sia `kprintf` sia `panic` fanno UNA passata sola su UN va_list, e
   il debito di M1 è chiuso per intero invece che a metà.

   ctx è ignorato: la console è una, non c'è niente da distinguere. */
void kputc_console(void *ctx, char c);

/* Scrive su VGA e su COM1, in una passata sola.

   Nota su cosa cambia rispetto alle due passate di prima: l'ordine dei byte sui
   due dispositivi si ALTERNA per carattere invece di essere "tutta la stringa su
   COM1, poi tutta sulla VGA". Nessuna conseguenza sul contenuto — gli stessi
   byte nello stesso ordine su ciascuno dei due — e una piccolissima in
   post-mortem: una tripla fault a metà di una kprintf adesso tronca anche la
   riga sulla seriale, mentre prima la seriale era già completa. Un crash dentro
   il formatter è comunque catastrofico, e vale meno del non avere più uno stato
   globale. */
void kprintf(const char *fmt, ...);

/* Formattano in un buffer invece che su un dispositivo, con la semantica C99:
   ritornano la lunghezza VOLUTA, che può essere >= size se hanno troncato.
   `size` conta il buffer intero, terminatore compreso, e con size > 0 il
   terminatore c'è sempre.

   Da qui esce il testo di /proc/N/status: procfs genera il contenuto invece di
   leggerlo, e queste sono il formattatore che riusa invece di duplicare.

   Rientranti e sicure sotto prelazione, perché lo stato del sink sta sullo stack
   di vsnprintf e ci arriva attraverso il ctx di kvprintf. */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);

#endif
