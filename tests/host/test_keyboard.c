/* Test della decodifica degli scancode col gcc dell'host.

   Sotto test c'e' solo scancode_to_char, che e' pura. Il gestore dell'IRQ 1
   parla con una porta e si verifica dentro QEMU, iniettando tasti. */

#define WALTEX_HOSTED 1

#include <stdio.h>
#include <stdlib.h>

#include "types.h"
#include "keyboard.h"
#include "idt.h"
#include "pic.h"
#include "ring.h"
#include "device.h"

/* keyboard.c li chiama nell'inizializzazione e sull'host non esistono. Sotto
   test c'e' solo scancode_to_char, che e' una traduzione pura: keyboard_init non
   viene mai chiamata da qui, ma i suoi simboli devono comunque risolversi al
   link. */
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

/* Da M8 keyboard_init iscrive il dispositivo "kbd" nel registro, e l'assert sul
   ritorno tira dentro panic. Il registro vero e' provato da test_device; qui
   basta che i simboli esistano. */
int device_register(const struct device *d)
{
    (void)d;
    return 0;
}

/* panic e' noreturn: uno stub che ritorna farebbe emettere un warning, e il
   progetto compila senza warning. exit() e' noreturn e chiude il discorso. */
void panic(const char *fmt, ...)
{
    (void)fmt;
    printf("FAIL -- panic chiamata da codice sotto test\n");
    exit(1);
}

static int failures;

static void expect(uint8_t sc, int shift, int want, const char *nota)
{
    int got = scancode_to_char(sc, shift);

    if (got == want) {
        printf("ok   -- 0x%02x shift=%d -> %-4d  %s\n", sc, shift, got, nota);
    } else {
        printf("FAIL -- 0x%02x shift=%d: atteso %d, ottenuto %d  (%s)\n",
               sc, shift, want, got, nota);
        failures++;
    }
}

int main(void)
{
    /* Lettere: la fila di 'asdfghjkl' comincia a 0x1E. */
    expect(0x1E, 0, 'a', "lettera");
    expect(0x1E, 1, 'A', "lettera con shift");
    expect(0x26, 0, 'l', "ultima della fila");
    expect(0x11, 0, 'w', "fila di qwerty");
    expect(0x2C, 0, 'z', "fila di zxcvbnm");

    /* Cifre: 0x02 e' '1', e con shift diventano i simboli della fila. */
    expect(0x02, 0, '1', "cifra");
    expect(0x02, 1, '!', "cifra con shift");
    expect(0x0B, 0, '0', "lo zero sta in fondo, non all'inizio");
    expect(0x0B, 1, ')', "zero con shift");

    /* Punteggiatura. */
    expect(0x34, 0, '.', "punto");
    expect(0x33, 0, ',', "virgola");
    expect(0x35, 1, '?', "slash con shift");

    /* I due caratteri di controllo che vogliamo. */
    expect(KBD_SC_SPACE, 0, ' ',  "spazio");
    expect(KBD_SC_ENTER, 0, '\n', "invio diventa newline");

    /* I modificatori non sono caratteri. */
    expect(KBD_SC_LSHIFT, 0, -1, "shift sinistro non e' un carattere");
    expect(KBD_SC_RSHIFT, 0, -1, "shift destro non e' un carattere");
    expect(0x1D, 0, -1, "ctrl non e' un carattere");

    /* I break code: stesso codice con il bit 7 acceso. Senza questo controllo
       ogni lettera comparirebbe due volte. */
    expect(0x9E, 0, -1, "rilascio di 'a'");
    expect(0x82, 0, -1, "rilascio di '1'");
    expect(0xB9, 0, -1, "rilascio dello spazio");

    /* Fuori tabella. */
    expect(0x00, 0, -1, "scancode zero");
    expect(0x58, 0, -1, "F12, non in tabella");
    expect(0x7F, 0, -1, "make code massimo, non in tabella");

    if (failures == 0) {
        printf("tutti i test della decodifica passano\n");
        return 0;
    }
    printf("%d test falliti\n", failures);
    return 1;
}
