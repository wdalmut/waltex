/* Test del registro dei dispositivi, col gcc dell'host.

   Il registro e' logica pura sopra un array statico: nessuna porta I/O, nessun
   interrupt, nessuna allocazione. Quindi si prova interamente qui, in
   millisecondi, e dentro la VM restano solo i tre dispositivi veri.

   Nessuno stub: device.c chiama soltanto strcmp, che arriva da memory.c. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "device.h"

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

/* Due operazioni finte: al registro interessa solo se il puntatore e' nullo o
   no, non cosa fanno. */
static int finta_read(struct device *d, void *buf, uint32_t n)
{
    (void)d; (void)buf; (void)n;
    return 0;
}

static int finta_write(struct device *d, const void *buf, uint32_t n)
{
    (void)d; (void)buf; (void)n;
    return (int)n;
}

/* Costruisce un descrittore. Il nome si copia a mano perche' non vogliamo
   dipendere da una strncpy che il progetto non ha. */
static struct device fai(const char *nome, uint16_t major, uint16_t minor,
                         int leggibile, int scrivibile)
{
    struct device d;
    int i;

    for (i = 0; i < DEV_NAME_MAX; i++)
        d.name[i] = '\0';
    for (i = 0; i < DEV_NAME_MAX - 1 && nome[i] != '\0'; i++)
        d.name[i] = nome[i];

    d.major = major;
    d.minor = minor;
    d.read  = leggibile  ? finta_read  : 0;
    d.write = scrivibile ? finta_write : 0;
    d.priv  = 0;

    return d;
}

/* ---- iscrizione ---------------------------------------------------------- */

static void test_iscrizione_semplice(void)
{
    struct device d = fai("console", 5, 1, 0, 1);

    device_init();

    check("il registro parte vuoto", device_count() == 0);
    check("la prima iscrizione ritorna 0", device_register(&d) == 0);
    check("device_count passa a 1", device_count() == 1);
}

/* Il controllo che distingue una copia da un alias, e l'unico modo di farlo
   dall'esterno: si iscrive un dispositivo, si modifica la struct SORGENTE, e si
   verifica che il registro non se ne sia accorto.

   Se device_register conservasse il puntatore invece di copiare, il registro
   cambierebbe sotto i piedi — e il guasto si manifesterebbe solo con chi si
   iscrive da una struct locale, cioe' molto lontano da qui. */
static void test_il_registro_copia(void)
{
    struct device d = fai("aaa", 7, 7, 1, 1);
    struct device *reg;

    device_init();
    device_register(&d);

    d.name[0] = 'z';
    d.name[1] = 'z';
    d.name[2] = 'z';
    d.major = 99;

    reg = device_find("aaa");

    check("il nome iscritto sopravvive alla modifica della sorgente",
          reg != 0 && same(reg->name, "aaa"));
    check("i numeri iscritti sopravvivono alla modifica della sorgente",
          reg != 0 && reg->major == 7);
    check("il nome nuovo della sorgente non compare nel registro",
          device_find("zzz") == 0);
}

static void test_registro_pieno(void)
{
    struct device d;
    char nome[4];
    int i, esiti = 0;

    device_init();

    /* MAX_DEVICES iscrizioni con nomi distinti: "d00", "d01", ... */
    for (i = 0; i < MAX_DEVICES; i++) {
        nome[0] = 'd';
        nome[1] = (char)('0' + i / 10);
        nome[2] = (char)('0' + i % 10);
        nome[3] = '\0';

        d = fai(nome, 1, (uint16_t)i, 1, 0);
        if (device_register(&d) == 0)
            esiti++;
    }

    check("ci stanno MAX_DEVICES dispositivi", esiti == MAX_DEVICES);
    check("device_count arriva a MAX_DEVICES", device_count() == MAX_DEVICES);

    d = fai("uno_in_piu", 2, 2, 1, 0);
    check("l'iscrizione oltre la capacita' e' rifiutata",
          device_register(&d) == -1);

    /* Un rifiuto che incrementasse il contatore lascerebbe uno slot vuoto
       raggiungibile con device_at, e chi ci chiamasse d->write salterebbe a
       zero. */
    check("un'iscrizione rifiutata non incrementa il conteggio",
          device_count() == MAX_DEVICES);
}

static void test_nome_duplicato(void)
{
    struct device primo  = fai("dup", 1, 1, 1, 0);
    struct device secondo = fai("dup", 99, 99, 0, 1);
    struct device *reg;

    device_init();
    device_register(&primo);

    check("un nome gia' iscritto e' rifiutato",
          device_register(&secondo) == -1);

    check("il duplicato rifiutato non incrementa il conteggio",
          device_count() == 1);

    /* Se il rifiuto avvenisse DOPO aver scritto lo slot, il primo dispositivo
       sarebbe stato sovrascritto e il rifiuto sarebbe una bugia. */
    reg = device_find("dup");
    check("il duplicato rifiutato non sovrascrive l'esistente",
          reg != 0 && reg->major == 1);
}

static void test_nome_al_limite(void)
{
    struct device d;
    struct device *reg;
    int i;

    device_init();

    /* DEV_NAME_MAX - 1 caratteri piu' il NUL: entra esatto. */
    d = fai("", 3, 3, 1, 0);
    for (i = 0; i < DEV_NAME_MAX - 1; i++)
        d.name[i] = 'x';
    d.name[DEV_NAME_MAX - 1] = '\0';

    check("un nome di DEV_NAME_MAX - 1 caratteri e' accettato",
          device_register(&d) == 0);

    reg = device_at(0);
    check("il nome al limite e' terminato da NUL",
          reg != 0 && reg->name[DEV_NAME_MAX - 1] == '\0');

    /* Tutti i DEV_NAME_MAX byte pieni: nessun NUL da nessuna parte.
       E' il caso REALE di "nome troppo lungo", perche' name e' un array da
       DEV_NAME_MAX e un nome piu' lungo non ci starebbe nemmeno da passare.
       Va rilevato scandendo AL MASSIMO DEV_NAME_MAX byte: una strlen normale
       qui cammina fuori dall'array.

       Qui major vale 4, quindi il byte subito dopo l'array non e' zero e una
       strlen non limitata risponde MOLTO piu' di DEV_NAME_MAX. */
    device_init();
    d = fai("", 4, 4, 1, 0);
    for (i = 0; i < DEV_NAME_MAX; i++)
        d.name[i] = 'y';

    check("un nome non terminato e' rifiutato, non troncato",
          device_register(&d) == -1);
    check("il nome non terminato non e' entrato nel registro",
          device_count() == 0);

    /* Lo stesso caso, ma con major = 0 — e questo e' quello che conta.

       name sta all'offset 0 della struct e major all'offset DEV_NAME_MAX,
       quindi con major a zero il byte immediatamente dopo l'array VALE ZERO:
       una strlen non limitata risponde esattamente DEV_NAME_MAX.

       Il nome resta invalido — il NUL non e' dentro l'array — ma un controllo
       scritto "rifiuta se lunghezza > DEV_NAME_MAX" lo accetta, e copia nel
       registro un nome senza terminatore. Da quel momento device_find legge
       oltre a ogni confronto.

       E' il controllo che distingue una scansione limitata da una strlen con il
       limite sbagliato: il caso qui sopra passa anche con la strlen, questo
       no. */
    device_init();
    d = fai("", 0, 0, 1, 0);
    for (i = 0; i < DEV_NAME_MAX; i++)
        d.name[i] = 'y';

    check("un nome di DEV_NAME_MAX byte esatti e' rifiutato",
          device_register(&d) == -1);
    check("il nome di DEV_NAME_MAX byte non e' entrato nel registro",
          device_count() == 0);
}

static void test_nome_vuoto(void)
{
    struct device d = fai("", 6, 6, 1, 1);

    device_init();

    check("un nome vuoto e' rifiutato", device_register(&d) == -1);
}

static void test_senza_operazioni(void)
{
    struct device d = fai("muto", 8, 8, 0, 0);

    device_init();

    /* Un dispositivo che non sa ne' leggere ne' scrivere non e' utilizzabile:
       iscriverlo nasconderebbe un driver che ha dimenticato di riempire la
       struct, e il guasto verrebbe fuori in M9 come un file di /dev che non
       risponde. */
    check("un dispositivo senza read ne' write e' rifiutato",
          device_register(&d) == -1);
}

/* ---- ricerca ------------------------------------------------------------- */

static void popola_tre(void)
{
    struct device a = fai("console", 5, 1, 0, 1);
    struct device b = fai("ttyS0", 4, 64, 0, 1);
    struct device c = fai("kbd", 13, 0, 1, 0);

    device_init();
    device_register(&a);
    device_register(&b);
    device_register(&c);
}

static void test_find(void)
{
    struct device *d;

    popola_tre();

    d = device_find("console");
    check("device_find trova console", d != 0 && d->major == 5);

    d = device_find("ttyS0");
    check("device_find trova ttyS0", d != 0 && d->minor == 64);

    d = device_find("kbd");
    check("device_find trova kbd", d != 0 && d->major == 13);

    check("device_find su un nome assente ritorna 0",
          device_find("pippo") == 0);

    /* La corrispondenza e' esatta. Un confronto sui primi N caratteri, o una
       strpos usata come "contiene", troverebbero console. */
    check("device_find non accetta un prefisso",
          device_find("cons") == 0);
}

static void test_by_id(void)
{
    struct device a = fai("uno", 5, 1, 1, 0);
    struct device b = fai("due", 5, 2, 1, 0);   /* stesso major! */
    struct device *d;

    device_init();
    device_register(&a);
    device_register(&b);

    d = device_by_id(5, 1);
    check("device_by_id trova per coppia", d != 0 && same(d->name, "uno"));

    /* Il controllo che prende un confronto sul solo major: due dispositivi che
       differiscono per il minor devono restare distinti. */
    d = device_by_id(5, 2);
    check("device_by_id distingue due minor sotto lo stesso major",
          d != 0 && same(d->name, "due"));

    check("device_by_id su una coppia assente ritorna 0",
          device_by_id(5, 3) == 0);
}

/* ---- enumerazione -------------------------------------------------------- */

static void test_at(void)
{
    struct device *a, *b, *c;

    popola_tre();

    a = device_at(0);
    b = device_at(1);
    c = device_at(2);

    check("device_at restituisce i tre in ordine di iscrizione",
          a != 0 && same(a->name, "console") &&
          b != 0 && same(b->name, "ttyS0") &&
          c != 0 && same(c->name, "kbd"));

    /* Due estremi: sotto zero legge i byte prima dell'array, sopra il conteggio
       legge uno slot mai scritto — e chi ci chiamasse d->write salterebbe a un
       puntatore nullo. */
    check("device_at fuori intervallo ritorna 0",
          device_at(-1) == 0 && device_at(device_count()) == 0 &&
          device_at(MAX_DEVICES) == 0);
}

int main(void)
{
    test_iscrizione_semplice();
    test_il_registro_copia();
    test_registro_pieno();
    test_nome_duplicato();
    test_nome_al_limite();
    test_nome_vuoto();
    test_senza_operazioni();
    test_find();
    test_by_id();
    test_at();

    if (failures == 0) {
        printf("tutti i test del registro dei dispositivi passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
