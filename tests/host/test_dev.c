/* Test del registry dei dispositivi, col gcc dell'host.

   Il registry e' logica pura sopra un array statico: nessuna porta I/O, nessun
   interrupt, nessuna allocazione. Quindi si prova interamente qui, in
   millisecondi, e dentro la VM restano solo i dispositivi veri.

   Nessuno stub: dev.c chiama soltanto strcmp, che arriva da memory.c. E non
   include ne' chardev.h ne' blockdev.h — impl e' un void *, e questo test e' il
   posto dove quel fatto si vede, perche' ci mette dentro un int. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "dev.h"

static int failures;

static void check(const char *name, int ok)
{
    printf(ok ? "ok   -- %s\n" : "FAIL -- %s\n", name);
    if (!ok)
        failures++;
}

static int same(const char *a, const char *b)
{
    int i;

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (a[i] != b[i])
            return 0;
    }

    return a[i] == b[i];
}

/* Un impl finto. Al registry interessa solo che il puntatore non sia nullo: non
   sa cosa sia, e non deve saperlo. */
static int finto_impl;

static struct dev_entry fai(const char *nome, enum dev_kind kind,
                            uint16_t major, uint16_t minor)
{
    struct dev_entry e;
    int i;

    for (i = 0; i < DEV_NAME_MAX; i++)
        e.name[i] = '\0';
    for (i = 0; i < DEV_NAME_MAX - 1 && nome[i] != '\0'; i++)
        e.name[i] = nome[i];

    e.kind  = kind;
    e.major = major;
    e.minor = minor;
    e.impl  = &finto_impl;

    return e;
}

/* ---- iscrizione ---------------------------------------------------------- */

static void test_iscrizione_semplice(void)
{
    struct dev_entry e = fai("console", DEV_CHAR, 5, 1);

    dev_init();
    check("il registry parte vuoto", dev_count() == 0);
    check("dev_register accetta la prima voce", dev_register(&e) == 0);
    check("dev_count passa a 1", dev_count() == 1);
}

/* Il NOME si copia, e questo test attraversa M11e invariato nella sostanza:
   e' il test_il_registro_copia di M8 con i nomi nuovi. */
static void test_il_nome_si_copia(void)
{
    struct dev_entry e = fai("uno", DEV_CHAR, 1, 1);
    const struct dev_entry *reg;

    dev_init();
    dev_register(&e);

    e.name[0] = 'X';

    reg = dev_get(0);
    check("il nome nel registry non cambia se la sorgente cambia",
          reg != 0 && same(reg->name, "uno"));
}

/* IL ROVESCIO del precedente, ed e' il contratto NUOVO di M11e: impl e' un
   puntatore, quindi cio' che sta dall'altro capo NON e' copiato.

   Sta qui perche' e' la trappola numero uno della milestone. Fino a M11d il
   registro copiava l'intera struct e i tre driver a caratteri la riempivano sullo
   stack — con un commento in serial_init che spiegava perche' fosse sicuro.
   Adesso quella memoria deve sopravvivere, e questo test e' il posto dove il
   contratto nuovo e' scritto in modo ESEGUIBILE invece che soltanto commentato.

   Il guasto che previene non si vede provando: con una locale il kernel boota, ls
   /dev funziona, e l'assert sull'iscrizione e' verde. Arriva al primo read dopo il
   riuso di quello stack. */
static void test_impl_si_riferisce(void)
{
    int oggetto = 7;
    struct dev_entry e = fai("due", DEV_CHAR, 2, 2);
    const struct dev_entry *reg;

    e.impl = &oggetto;

    dev_init();
    dev_register(&e);

    oggetto = 9;

    reg = dev_get(0);
    check("impl RIFERISCE e non copia: il puntatore e' lo stesso",
          reg != 0 && reg->impl == &oggetto);
    check("e il cambiamento della sorgente si vede",
          reg != 0 && *(int *)reg->impl == 9);
}

/* ---- i sei rifiuti ------------------------------------------------------- */

static void test_registry_pieno(void)
{
    struct dev_entry e;
    int i;

    dev_init();

    for (i = 0; i < DEV_MAX; i++) {
        char nome[4];

        nome[0] = 'a';
        nome[1] = (char)('0' + (i / 10));
        nome[2] = (char)('0' + (i % 10));
        nome[3] = '\0';

        e = fai(nome, DEV_CHAR, (uint16_t)(100 + i), 0);
        if (dev_register(&e) != 0)
            break;
    }

    check("dev_count arriva a DEV_MAX", dev_count() == DEV_MAX);

    e = fai("troppo", DEV_CHAR, 200, 0);
    check("la voce oltre DEV_MAX e' rifiutata", dev_register(&e) == -1);
    check("e non e' entrata", dev_count() == DEV_MAX);
}

/* Rifiuto NUOVO in M11e. Serve perche' devio deve poter interpretare impl, e uno
   slot che non dichiara la specie non si puo' servire: interpretarlo male
   significa leggere un puntatore a funzione dall'offset sbagliato. */
static void test_kind_invalido(void)
{
    struct dev_entry e = fai("boh", DEV_NONE, 9, 9);

    dev_init();
    check("DEV_NONE e' rifiutato", dev_register(&e) == -1);

    e = fai("boh", (enum dev_kind)99, 9, 9);
    check("un kind fuori intervallo e' rifiutato", dev_register(&e) == -1);
    check("nessuno dei due e' entrato", dev_count() == 0);
}

static void test_nome_duplicato(void)
{
    struct dev_entry a = fai("dup", DEV_CHAR,  1, 1);
    struct dev_entry b = fai("dup", DEV_BLOCK, 2, 2);

    dev_init();
    check("la prima con quel nome entra", dev_register(&a) == 0);

    /* Il rifiuto vale anche fra SPECIE DIVERSE: /dev ha un solo namespace, come
       su Unix, dove non possono coesistere un hda a caratteri e un hda a
       blocchi. */
    check("la seconda con lo STESSO nome e' rifiutata, anche di specie diversa",
          dev_register(&b) == -1);
    check("il registry ne contiene una sola", dev_count() == 1);
    check("e la vincente e' la PRIMA, non l'ultima",
          dev_lookup_index("dup") == 0 &&
          dev_get(0) != 0 && dev_get(0)->kind == DEV_CHAR);
}

/* Rifiuto NUOVO in M11e, e ha un secondo effetto: rende dev_by_id ben definita.
   Senza unicita' la ricerca per coppia renderebbe "una qualsiasi". */
static void test_coppia_duplicata(void)
{
    struct dev_entry a = fai("uno", DEV_CHAR, 5, 1);
    struct dev_entry b = fai("due", DEV_CHAR, 5, 1);
    struct dev_entry c = fai("tre", DEV_CHAR, 5, 2);

    dev_init();
    check("la prima con 5:1 entra", dev_register(&a) == 0);
    check("una seconda con 5:1 e' rifiutata anche con un nome diverso",
          dev_register(&b) == -1);
    check("5:2 invece entra: e' il minor a distinguerle",
          dev_register(&c) == 0);
    check("il registry ne contiene due", dev_count() == 2);
}

/* LA CURA DEL BUG di chardev_register, ed e' il controllo piu' importante di
   questo file.

   In M8 c'era:

       int p = strpos(d->name, '\0');
       if (p > DEV_NAME_MAX) { return 1; }
       size_t lname = strlen(d->name);
       if (lname >= DEV_NAME_MAX || lname == 0) { return -1; }

   strpos cercando '\0' ritorna SEMPRE -1, per un ramo esplicito in memory.c:
   la guardia era codice morto, e il suo "return 1" avrebbe violato il contratto
   "0 oppure -1". Cio' che proteggeva davvero era la strlen, cioe' precisamente la
   scansione illimitata contro cui l'header metteva in guardia per venticinque
   righe.

   Serve una scansione LIMITATA a DEV_NAME_MAX byte. */
static void test_nome_al_limite(void)
{
    struct dev_entry e;
    int i;

    /* Il nome riempie l'array senza lasciare posto al terminatore. Una scansione
       limitata se ne accorge; una strlen cammina FUORI dall'array, e cio' che
       trova dipende da come il compilatore ha disposto i campi dopo name.

       In dev_entry dopo name c'e' kind, che e' un int. Con DEV_CHAR — che vale 1 —
       il primo byte dopo l'array non e' zero, quindi una strlen risponde piu' di
       DEV_NAME_MAX e il controllo sbagliato rifiuterebbe comunque, per la ragione
       sbagliata. */
    dev_init();
    e = fai("", DEV_CHAR, 4, 4);
    for (i = 0; i < DEV_NAME_MAX; i++)
        e.name[i] = 'y';

    check("un nome non terminato e' rifiutato, non troncato",
          dev_register(&e) == -1);
    check("il nome non terminato non e' entrato", dev_count() == 0);

    /* Che il rifiuto sia arrivato dal controllo sul NOME e non da qualche altro,
       lo si vede cambiando una cosa alla volta: lo stesso nome accorciato di un
       byte, cioe' terminato, con tutto il resto identico, deve ENTRARE.

       Senza questa meta', il controllo qui sopra passerebbe anche con un
       dev_register che rifiuta tutto. */
    dev_init();
    e = fai("", DEV_CHAR, 4, 4);
    for (i = 0; i < DEV_NAME_MAX - 1; i++)
        e.name[i] = 'y';

    check("lo stesso nome TERMINATO, di 15 caratteri, entra",
          dev_register(&e) == 0);
    check("ed e' leggibile per intero",
          dev_get(0) != 0 && same(dev_get(0)->name, "yyyyyyyyyyyyyyy"));
}

static void test_nome_vuoto(void)
{
    struct dev_entry e = fai("", DEV_CHAR, 6, 6);

    dev_init();
    check("un nome vuoto e' rifiutato", dev_register(&e) == -1);
    check("e non e' entrato", dev_count() == 0);
}

/* ---- le tre ricerche ----------------------------------------------------- */

static void test_lookup_index(void)
{
    struct dev_entry a = fai("console", DEV_CHAR,  5, 1);
    struct dev_entry b = fai("ttyS0",   DEV_CHAR,  4, 64);
    struct dev_entry c = fai("hda",     DEV_BLOCK, 3, 0);

    dev_init();
    dev_register(&a);
    dev_register(&b);
    dev_register(&c);

    /* L'indice e' quello di ISCRIZIONE, ed e' cio' su cui devfs costruisce il
       patto col proprio pool di inode: slot i del pool per la voce i. */
    check("dev_lookup_index rende l'indice di iscrizione",
          dev_lookup_index("console") == 0 &&
          dev_lookup_index("ttyS0")   == 1 &&
          dev_lookup_index("hda")     == 2);

    check("dev_lookup_index su un nome assente rende -1",
          dev_lookup_index("pippo") == -1);

    /* La corrispondenza e' ESATTA: in /dev sarebbe la differenza fra aprire un
       file e aprirne un altro. */
    check("dev_lookup_index non accetta un prefisso",
          dev_lookup_index("cons") == -1);
    check("ne' un nome piu' lungo che comincia uguale",
          dev_lookup_index("console2") == -1);
    check("ne' la stringa vuota", dev_lookup_index("") == -1);
}

static void test_get(void)
{
    struct dev_entry a = fai("uno", DEV_CHAR,  1, 1);
    struct dev_entry b = fai("due", DEV_BLOCK, 2, 3);

    dev_init();
    dev_register(&a);
    dev_register(&b);

    check("dev_get rende le voci in ordine di iscrizione",
          dev_get(0) != 0 && same(dev_get(0)->name, "uno") &&
          dev_get(1) != 0 && same(dev_get(1)->name, "due"));

    /* La guardia sul nullo NON e' ridondante con il check qui sopra, e la lezione
       vale per tutta la suite: un test deve FALLIRE, non crashare. Con
       dev_register rotto — un solo || al posto di un && basta — qui non e' entrato
       niente, dev_get(1) rende 0, e senza questa guardia il binario muore di
       SIGSEGV invece di stampare quali controlli sono caduti. Cioe' l'output
       sparisce proprio nel momento in cui serve leggerlo. */
    check("dev_get conserva kind, major e minor",
          dev_get(1) != 0 &&
          dev_get(1)->kind  == DEV_BLOCK &&
          dev_get(1)->major == 2 && dev_get(1)->minor == 3);

    /* Sotto zero legge i byte prima dell'array; sopra il conteggio legge uno slot
       mai scritto, e chi ne usasse impl salterebbe in un indirizzo arbitrario. */
    check("dev_get fuori intervallo rende 0",
          dev_get(-1) == 0 && dev_get(dev_count()) == 0 &&
          dev_get(DEV_MAX) == 0);
}

static void test_by_id(void)
{
    struct dev_entry a = fai("uno", DEV_CHAR,  5, 1);
    struct dev_entry b = fai("due", DEV_CHAR,  5, 2);
    struct dev_entry c = fai("hda", DEV_BLOCK, 3, 0);

    dev_init();
    dev_register(&a);
    dev_register(&b);
    dev_register(&c);

    check("dev_by_id trova per coppia",
          dev_by_id(5, 1) != 0 && same(dev_by_id(5, 1)->name, "uno"));
    check("dev_by_id distingue due minor sotto lo stesso major",
          dev_by_id(5, 2) != 0 && same(dev_by_id(5, 2)->name, "due"));
    check("dev_by_id trova anche un dispositivo a blocchi",
          dev_by_id(3, 0) != 0 && same(dev_by_id(3, 0)->name, "hda"));
    check("dev_by_id su una coppia assente rende 0", dev_by_id(5, 3) == 0);

    /* Il major da solo non basta: 3:1 non e' 3:0, e un confronto che guardasse
       solo il major renderebbe hda per qualunque minor. */
    check("dev_by_id confronta ENTRAMBI i numeri", dev_by_id(3, 1) == 0);
}

int main(void)
{
    test_iscrizione_semplice();
    test_il_nome_si_copia();
    test_impl_si_riferisce();
    test_registry_pieno();
    test_kind_invalido();
    test_nome_duplicato();
    test_coppia_duplicata();
    test_nome_al_limite();
    test_nome_vuoto();
    test_lookup_index();
    test_get();
    test_by_id();

    if (failures == 0) {
        printf("tutti i test del registry dei dispositivi passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
