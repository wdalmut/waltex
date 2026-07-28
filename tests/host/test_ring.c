/* Test del buffer circolare col gcc dell'host.

   Il buffer si pilota direttamente, quindi si verifica tutto qui: dentro QEMU
   servirebbe iniettare tasti e leggere una seriale, cioe' un test lento e
   indiretto per una logica che e' pura. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "ring.h"

static int failures;

static void check(const char *name, int ok)
{
    printf("%s -- %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

static void test_ordine(void)
{
    struct ring r;
    int i, ok = 1;

    ring_init(&r);
    check("appena inizializzato e' vuoto", ring_empty(&r));
    check("pop su vuoto restituisce -1", ring_pop(&r) == -1);

    for (i = 0; i < 5; i++)
        if (!ring_push(&r, (uint8_t)('a' + i)))
            ok = 0;
    check("cinque push su buffer vuoto riescono", ok);
    check("dopo un push non e' piu' vuoto", !ring_empty(&r));

    ok = 1;
    for (i = 0; i < 5; i++)
        if (ring_pop(&r) != 'a' + i)
            ok = 0;
    check("i valori escono nell'ordine in cui sono entrati", ok);
    check("svuotato, torna vuoto", ring_empty(&r));
}

static void test_capacita(void)
{
    struct ring r;
    uint32_t i;
    int ok = 1;

    ring_init(&r);

    /* Uno slot e' sacrificato per distinguere pieno da vuoto senza un
       contatore condiviso: la capacita' utile e' RING_SIZE - 1. */
    for (i = 0; i < RING_SIZE - 1; i++)
        if (!ring_push(&r, (uint8_t)i))
            ok = 0;
    check("ci stanno RING_SIZE - 1 elementi", ok);

    check("l'elemento successivo viene rifiutato",
          ring_push(&r, 0xFF) == 0);

    /* Il rifiuto non deve aver corrotto niente: il primo elemento e' ancora
       quello. */
    check("un push rifiutato non sovrascrive la coda",
          ring_pop(&r) == 0);
}

static void test_wraparound(void)
{
    struct ring r;
    uint32_t i;
    int ok = 1;

    ring_init(&r);

    /* Tre giri completi: gli indici devono superare RING_SIZE e ripartire
       senza che si noti. Con un & al posto del modulo, un errore qui si
       manifesterebbe solo dopo il primo giro. */
    for (i = 0; i < RING_SIZE * 3; i++) {
        if (!ring_push(&r, (uint8_t)(i & 0xFF)))
            ok = 0;
        if (ring_pop(&r) != (int)(i & 0xFF))
            ok = 0;
    }
    check("push e pop alternati per tre giri completi", ok);
    check("dopo tre giri e' vuoto", ring_empty(&r));

    /* E riempiendolo a cavallo del punto di ricongiungimento. */
    ring_init(&r);
    for (i = 0; i < RING_SIZE - 10; i++)
        ring_push(&r, 0);
    for (i = 0; i < RING_SIZE - 10; i++)
        ring_pop(&r);
    /* ora head e tail sono a RING_SIZE-10: i prossimi push scavalcano */
    ok = 1;
    for (i = 0; i < 20; i++)
        if (!ring_push(&r, (uint8_t)(100 + i)))
            ok = 0;
    for (i = 0; i < 20; i++)
        if (ring_pop(&r) != (int)(100 + i))
            ok = 0;
    check("riempimento a cavallo del ricongiungimento", ok);
}

int main(void)
{
    test_ordine();
    test_capacita();
    test_wraparound();

    if (failures == 0) {
        printf("tutti i test del ring buffer passano\n");
        return 0;
    }
    printf("%d test falliti\n", failures);
    return 1;
}
