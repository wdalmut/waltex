/* Test di kernel/memory.c col gcc dell'host, senza QEMU.
   WALTEX_HOSTED (definito nel Makefile) fa arrivare i tipi da stdint.h.

   Nessun <string.h>: il progetto ne ha uno proprio che, raggiunto via -I,
   oscura quello di glibc. I confronti ce li scriviamo.

   Non c'e' nessun test sulle regioni sovrapposte: per memcpy sono
   comportamento indefinito, e un test che le esercitasse certificherebbe
   qualcosa che il contratto non promette. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "memory.h"

static int failures;

static void check(const char *name, int ok)
{
    printf("%s -- %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

/* Arena con guardie: 8 byte di 0xAA prima e dopo l'area utile, per accorgersi
   di una scrittura fuori dai limiti invece di sperare che si noti.
   Union per garantire l'allineamento a 16 bit richiesto da memset16. */
#define GUARD 8
#define AREA  64

static union {
    uint16_t w[(GUARD + AREA + GUARD) / 2];
    uint8_t  b[GUARD + AREA + GUARD];
} arena;

static uint8_t *area(void)
{
    return arena.b + GUARD;
}

static void arena_reset(void)
{
    size_t i;
    for (i = 0; i < sizeof(arena.b); i++)
        arena.b[i] = 0xAA;
}

static int guards_intact(void)
{
    size_t i;
    for (i = 0; i < GUARD; i++) {
        if (arena.b[i] != 0xAA)
            return 0;
        if (arena.b[GUARD + AREA + i] != 0xAA)
            return 0;
    }
    return 1;
}

/* Quanti byte dell'area utile, a partire da 'from', sono ancora intonsi. */
static int untouched_from(size_t from)
{
    size_t i;
    for (i = from; i < AREA; i++)
        if (area()[i] != 0xAA)
            return 0;
    return 1;
}

static void test_memcpy(void)
{
    uint8_t src[16];
    size_t i;
    int ok;

    for (i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)(0x10 + i);

    /* Copia ogni byte, non solo il primo: e' il bug che si ottiene avanzando
       il puntatore sbagliato dentro il ciclo. */
    arena_reset();
    memcpy(area(), src, sizeof(src));
    ok = 1;
    for (i = 0; i < sizeof(src); i++)
        if (area()[i] != src[i])
            ok = 0;
    check("memcpy copia tutti i byte, in ordine", ok);

    check("memcpy non scrive oltre n", untouched_from(sizeof(src)));
    check("memcpy non tocca le guardie", guards_intact());

    /* n = 0 non deve scrivere nulla. */
    arena_reset();
    memcpy(area(), src, 0);
    check("memcpy con n=0 non scrive nulla", untouched_from(0));

    /* Il valore di ritorno e' la destinazione. */
    arena_reset();
    check("memcpy restituisce dest", memcpy(area(), src, 4) == area());

    /* Lunghezza dispari e destinazione disallineata: e' una funzione che
       ragiona in byte, non deve avere preferenze. */
    arena_reset();
    memcpy(area() + 1, src, 7);
    ok = 1;
    for (i = 0; i < 7; i++)
        if (area()[1 + i] != src[i])
            ok = 0;
    check("memcpy funziona disallineato e con lunghezza dispari", ok);
    check("memcpy disallineato non sfora", area()[0] == 0xAA && untouched_from(8));
}

static void test_memset(void)
{
    size_t i;
    int ok;

    arena_reset();
    memset(area(), 0x5C, 10);
    ok = 1;
    for (i = 0; i < 10; i++)
        if (area()[i] != 0x5C)
            ok = 0;
    check("memset riempie n byte con il valore", ok);

    check("memset non scrive oltre n", untouched_from(10));
    check("memset non tocca le guardie", guards_intact());

    /* Il parametro e' un int ma lo standard lo converte a unsigned char:
       conta solo il byte basso. */
    arena_reset();
    memset(area(), 0x1234, 4);
    ok = 1;
    for (i = 0; i < 4; i++)
        if (area()[i] != 0x34)
            ok = 0;
    check("memset tronca il valore al byte basso", ok);

    arena_reset();
    memset(area(), 0x5C, 0);
    check("memset con n=0 non scrive nulla", untouched_from(0));

    arena_reset();
    check("memset restituisce dest", memset(area(), 0, 4) == area());
}

static void test_memset16(void)
{
    uint16_t *cells;
    size_t i;
    int ok;

    /* La cella VGA: due byte diversi nello stesso elemento. E' il caso che
       memset non puo' esprimere, ed e' la ragione per cui memset16 esiste. */
    arena_reset();
    cells = (uint16_t *)area();
    memset16(cells, 0x0720, 6);
    ok = 1;
    for (i = 0; i < 6; i++)
        if (cells[i] != 0x0720)
            ok = 0;
    check("memset16 replica un pattern a 16 bit", ok);

    /* count conta ELEMENTI: 6 elementi sono 12 byte, il tredicesimo e' intonso.
       Se qualcuno lo interpretasse come byte, si fermerebbe a meta'. */
    check("memset16 conta elementi, non byte", untouched_from(12));
    check("memset16 non tocca le guardie", guards_intact());

    arena_reset();
    cells = (uint16_t *)area();
    memset16(cells, 0xBEEF, 0);
    check("memset16 con count=0 non scrive nulla", untouched_from(0));

    arena_reset();
    cells = (uint16_t *)area();
    check("memset16 restituisce dest", memset16(cells, 0, 2) == (void *)cells);
}

int main(void)
{
    test_memcpy();
    test_memset();
    test_memset16();

    if (failures == 0) {
        printf("tutti i test di memory.c passano\n");
        return 0;
    }
    printf("%d test falliti\n", failures);
    return 1;
}
