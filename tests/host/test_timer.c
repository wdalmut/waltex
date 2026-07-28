/* Test del calcolo del divisore del PIT, col gcc dell'host.
   E' aritmetica pura: non serve QEMU, e il ciclo di prova e' di millisecondi.

   Non c'e' nessun test su timer_init: quella parla con le porte I/O e si puo'
   verificare solo dentro la VM, dove lo fanno i self-check. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "timer.h"
#include "idt.h"
#include "pic.h"

/* timer.c chiama questi due nell'inizializzazione, e sull'host non esistono:
   uno vive nell'IDT, l'altro parla a un chip. Stub inerti, come i sink di
   vga_putc e serial_putc in test_kprintf. Sotto test c'e' solo pit_divisor,
   che e' aritmetica pura e non tocca nulla di tutto questo. */
void irq_register(uint8_t irq, void (*handler)(struct regs *))
{
    (void)irq;
    (void)handler;
}

void pic_mask(uint8_t irq, int masked)
{
    (void)irq;
    (void)masked;
}

static int failures;

static void expect(uint32_t hz, uint32_t want)
{
    uint32_t got = pit_divisor(hz);

    if (got == want) {
        printf("ok   -- %u Hz -> divisore %u\n", hz, got);
    } else {
        printf("FAIL -- %u Hz: atteso divisore %u, ottenuto %u\n",
               hz, want, got);
        failures++;
    }
}

int main(void)
{
    /* I casi normali. 1193182/100 fa 11931.82, troncato 11931: la frequenza
       reale e' 100.007 Hz, un errore dello 0.007%. */
    expect(100, 11931);
    expect(1000, 1193);
    expect(50, 23863);
    expect(19, 62799);

    /* La frequenza piu' bassa rappresentabile: divisore 65535. Sotto i 19 Hz
       il divisore non ci starebbe in 16 bit, quindi va limitato. */
    expect(18, PIT_DIVISOR_MAX);
    expect(1, PIT_DIVISOR_MAX);

    /* Zero: la divisione non va fatta, va evitata. Senza guardia questo test
       non stampa "FAIL", fa crashare il processo. */
    expect(0, PIT_DIVISOR_MAX);

    /* All'altro estremo, una frequenza superiore a quella base darebbe
       divisore 0 — che il PIT interpreta come 65536, cioe' il contrario di
       quello che si voleva. Il minimo utile e' 1. */
    expect(PIT_BASE_FREQ, 1);
    expect(PIT_BASE_FREQ * 2, 1);

    if (failures == 0) {
        printf("tutti i test del divisore PIT passano\n");
        return 0;
    }
    printf("%d test falliti\n", failures);
    return 1;
}
