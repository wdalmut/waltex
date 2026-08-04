/* Test dell'adapter byte<->LBA, col gcc dell'host.

   E' il cuore di M11e: clamping, bounce buffer, letture a cavallo di piu' settori,
   trasferimenti parziali, read-modify-write. Tutto il resto della milestone e'
   mappatura e rename.

   IL DISCO FINTO STA IN RAM E NON SU FILE, a differenza di test_minixfs, e la
   ragione e' che l'aritmetica va provata anche sui FALLIMENTI: iniettare un errore
   a un settore preciso e' cosa che fread non sa fare, e il ramo "errore dopo aver
   gia' copiato qualcosa rende i byte trasferiti" non e' raggiungibile in nessun
   altro modo.

   Il pattern e' lo stesso generatore di tools/mkdisk.sh — byte[i] = (i*7+3) & 0xFF
   — perche' e' deterministico e NON costante: un memset di un valore solo
   passerebbe anche con un adapter che legge sempre lo stesso settore, o che riempie
   mezzo buffer e lascia il resto a caso.

   Le due vtable sono static dentro devio.c, quindi l'unica strada per arrivarci e'
   devio_fill_inode — ed e' un vantaggio, non un ostacolo: il test esercita il
   percorso vero invece di scorciarlo. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "dev.h"
#include "blockdev.h"
#include "devio.h"
#include "vfs.h"

#define NSECT 4
#define DISCO (NSECT * SECTOR_SIZE)

static int failures;

static void check(const char *name, int ok)
{
    printf(ok ? "ok   -- %s\n" : "FAIL -- %s\n", name);
    if (!ok)
        failures++;
}

static uint8_t disco[DISCO];

/* -1 = mai. Altrimenti l'LBA da cui in poi read e write falliscono. */
static int fallisci_da;

/* Quante chiamate ha ricevuto il driver. E' il controllo che dice che l'adapter va
   UN SETTORE PER VOLTA, cioe' che la buffer cache di M12 avra' un solo punto in cui
   infilarsi. Senza questo contatore, una via rapida per le letture allineate
   passerebbe tutti gli altri controlli. */
static int chiamate;

static uint8_t atteso(uint32_t i)
{
    return (uint8_t)((i * 7 + 3) & 0xFF);
}

static void riempi_disco(void)
{
    uint32_t i;

    for (i = 0; i < DISCO; i++)
        disco[i] = atteso(i);

    fallisci_da = -1;
    chiamate    = 0;
}

static int finto_read(struct blockdev *b, uint32_t lba, void *buf, uint32_t count)
{
    uint8_t *d = (uint8_t *)buf;
    uint32_t i;

    (void)b;
    chiamate++;

    if (fallisci_da >= 0 && lba >= (uint32_t)fallisci_da)
        return -1;

    /* Per SOTTRAZIONE e non con lba + count: e' la regola di M10, e vale anche in
       un driver finto — un test che sbaglia il proprio controllo di limite
       nasconde il bug invece di trovarlo. */
    if (lba >= NSECT || count > (uint32_t)NSECT - lba)
        return -1;

    for (i = 0; i < count * SECTOR_SIZE; i++)
        d[i] = disco[lba * SECTOR_SIZE + i];

    return (int)count;
}

static int finto_write(struct blockdev *b, uint32_t lba, const void *buf,
                       uint32_t count)
{
    const uint8_t *s = (const uint8_t *)buf;
    uint32_t i;

    (void)b;
    chiamate++;

    if (fallisci_da >= 0 && lba >= (uint32_t)fallisci_da)
        return -1;

    if (lba >= NSECT || count > (uint32_t)NSECT - lba)
        return -1;

    for (i = 0; i < count * SECTOR_SIZE; i++)
        disco[lba * SECTOR_SIZE + i] = s[i];

    return (int)count;
}

static struct blockdev bd_rw = {
    .name = "hda", .nsectors = NSECT,
    .read = finto_read, .write = finto_write
};

static struct blockdev bd_ro = {
    .name = "hdb", .nsectors = NSECT,
    .read = finto_read
    /* .write resta 0: disco read-only, uno stato LEGITTIMO */
};

static struct blockdev bd_muto = {
    .name = "hdc", .nsectors = NSECT
    /* ne' read ne' write: blockdev_register lo rifiuta, ma l'adapter deve
       comunque non saltare se ci arriva da un'altra strada */
};

/* Costruisce l'inode passando da devio_fill_inode, che e' l'unica strada verso la
   vtable static. */
static int prepara(struct inode *in, struct blockdev *b)
{
    struct dev_entry e;
    int i;

    for (i = 0; i < DEV_NAME_MAX; i++)
        e.name[i] = '\0';
    e.name[0] = 'h'; e.name[1] = 'd'; e.name[2] = 'a';

    e.kind  = DEV_BLOCK;
    e.major = 3;
    e.minor = 0;
    e.impl  = b;

    return devio_fill_inode(&e, in);
}

static int contenuto_giusto(const uint8_t *buf, uint32_t off, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (buf[i] != atteso(off + i))
            return 0;
    }

    return 1;
}

/* ---- la vista a file ----------------------------------------------------- */

static void test_fill_inode(void)
{
    struct inode in;

    riempi_disco();

    check("devio_fill_inode riesce su un blockdev", prepara(&in, &bd_rw) == 0);
    check("il tipo e' INODE_BLOCKDEV", in.type == INODE_BLOCKDEV);
    check("size e' nsectors * SECTOR_SIZE", in.size == DISCO);
    check("priv punta al blockdev, non alla voce", in.priv == (void *)&bd_rw);
    check("major e minor arrivano dalla voce", in.major == 3 && in.minor == 0);
    check("read e write ci sono",
          in.ops != 0 && in.ops->read != 0 && in.ops->write != 0);

    /* Un disco non e' una directory: le tre caselle restano nulle, ed e' il motivo
       per cui mkdir /dev/hda fallisce da se'. */
    check("un disco non ha lookup ne' readdir ne' create",
          in.ops->lookup == 0 && in.ops->readdir == 0 && in.ops->create == 0);

    /* Su -1 *in NON viene toccato, che e' la convenzione di lookup e create. */
    {
        struct dev_entry e;
        struct inode intatto;
        int i;

        for (i = 0; i < DEV_NAME_MAX; i++) e.name[i] = '\0';
        e.name[0] = 'x';
        e.kind = DEV_NONE; e.major = 0; e.minor = 0; e.impl = &bd_rw;

        intatto.type = INODE_FILE;
        check("su kind invalido rende -1", devio_fill_inode(&e, &intatto) == -1);
        check("e non ha toccato *in", intatto.type == INODE_FILE);
    }
}

/* ---- lettura ------------------------------------------------------------- */

static void test_lettura_allineata(void)
{
    struct inode in;
    uint8_t buf[64];

    riempi_disco();
    prepara(&in, &bd_rw);

    check("read di 16 byte dall'inizio rende 16",
          in.ops->read(&in, 0, buf, 16) == 16);
    check("e il contenuto e' quello del disco", contenuto_giusto(buf, 0, 16));
    check("ed e' bastato UN settore", chiamate == 1);
}

static void test_lettura_non_allineata(void)
{
    struct inode in;
    uint8_t buf[64];

    riempi_disco();
    prepara(&in, &bd_rw);

    /* skip = 5: il bounce buffer serve proprio a questo. Un adapter che ignorasse
       lo scostamento renderebbe i byte 0..9 invece di 5..14 — e sarebbero dati
       veri, quindi plausibili. */
    check("read di 10 byte dall'offset 5 rende 10",
          in.ops->read(&in, 5, buf, 10) == 10);
    check("e parte dal byte 5, non dallo 0", contenuto_giusto(buf, 5, 10));
    check("un solo settore letto", chiamate == 1);
}

/* IL caso difficile: 500 + 100 attraversa il confine fra il settore 0 e l'1.
   Esercita in un colpo l'offset non allineato, la doppia iterazione, e lo skip che
   torna a ZERO al secondo giro — che e' l'errore piu' facile, riusare lo skip
   iniziale a ogni settore. */
static void test_lettura_a_cavallo(void)
{
    struct inode in;
    uint8_t buf[128];

    riempi_disco();
    prepara(&in, &bd_rw);

    check("read di 100 byte dall'offset 500 rende 100",
          in.ops->read(&in, 500, buf, 100) == 100);
    check("il contenuto e' CONTINUO attraverso il confine",
          contenuto_giusto(buf, 500, 100));
    check("sono servite DUE letture di settore", chiamate == 2);
}

static void test_lettura_su_quattro_settori(void)
{
    struct inode in;
    static uint8_t buf[2048];

    riempi_disco();
    prepara(&in, &bd_rw);

    /* 100 .. 1599: settori 0, 1, 2 e 3. */
    check("read di 1500 byte dall'offset 100 rende 1500",
          in.ops->read(&in, 100, buf, 1500) == 1500);
    check("il contenuto e' continuo su quattro settori",
          contenuto_giusto(buf, 100, 1500));

    /* IL controllo che protegge la cuciura per la buffer cache: una lettura per
       settore, non una sola grande. Con una via rapida per le letture allineate
       questo numero scenderebbe, e la cache di M12 avrebbe due punti in cui
       infilarsi invece di uno. */
    check("una lettura PER SETTORE, non una sola grande", chiamate == 4);
}

static void test_clamp_e_eof(void)
{
    struct inode in;
    uint8_t buf[128];

    riempi_disco();
    prepara(&in, &bd_rw);

    /* Il clamp: 2040 + 100 sfora, e si riducono a 8. */
    check("read che sfora la fine rende solo i byte che ci sono",
          in.ops->read(&in, DISCO - 8, buf, 100) == 8);
    check("e sono gli ultimi 8 del disco", contenuto_giusto(buf, DISCO - 8, 8));

    /* EOF VERO, e qui lo zero e' onesto: su un chardev significherebbe "adesso
       niente", qui "finito". E' la ragione per cui shell_cat non va toccato — il
       ramo file si ferma sullo zero, e un INODE_BLOCKDEV prende quel ramo. */
    check("read esattamente alla fine rende 0",
          in.ops->read(&in, DISCO, buf, 10) == 0);
    check("read oltre la fine rende 0",
          in.ops->read(&in, DISCO + 5000, buf, 10) == 0);

    /* off + n GIRA se si somma: off vicino a 2^32 piu' n positivo torna piccolo, e
       un adapter che confrontasse la somma crederebbe di essere dentro il disco e
       chiederebbe un LBA assurdo. Si sottrae. */
    check("un offset enorme rende 0 invece di girare",
          in.ops->read(&in, 0xFFFFFFF0u, buf, 64) == 0);

    riempi_disco();
    prepara(&in, &bd_rw);
    check("n == 0 rende 0", in.ops->read(&in, 0, buf, 0) == 0);
    check("e non ha toccato il disco", chiamate == 0);
}

static void test_operazione_assente(void)
{
    struct inode in;
    uint8_t buf[64];

    riempi_disco();
    prepara(&in, &bd_muto);

    /* "Non supportata" rende -1 e NON 0. La convenzione del puntatore nullo
       descrive il DISPOSITIVO; il valore consegnato a chi ha chiesto di leggere
       deve essere -1, perche' uno zero direbbe EOF. E' il return 1 di chardev_read
       che in M9b faceva avanzare l'offset a cat su un buffer che nessuno aveva
       riempito. */
    check("read con b->read nullo rende -1", in.ops->read(&in, 0, buf, 16) == -1);
    check("write con b->write nullo rende -1",
          in.ops->write(&in, 0, buf, 16) == -1);

    /* La capacita' e' una proprieta' del DISPOSITIVO, non della richiesta: -1
       anche a n == 0 e anche oltre la fine. */
    check("read con b->read nullo rende -1 anche a n == 0",
          in.ops->read(&in, 0, buf, 0) == -1);
    check("e anche oltre la fine", in.ops->read(&in, DISCO, buf, 16) == -1);

    /* E un disco read-only e' LEGITTIMO: si legge, non si scrive. */
    riempi_disco();
    prepara(&in, &bd_ro);
    check("un disco read-only si legge", in.ops->read(&in, 0, buf, 16) == 16);
    check("e non si scrive", in.ops->write(&in, 0, buf, 16) == -1);
}

/* I due rami dell'errore, e la distinzione e' la convenzione di read portata
   dentro: chi ha ricevuto 12 byte buoni deve saperlo, e dirgli -1 glieli fa
   buttare. Chi non ne ha ricevuto nessuno non ha nulla da salvare, e uno zero gli
   direbbe EOF — cioe' una bugia.

   Questo e' il gruppo di controlli che il disco finto in RAM esiste per rendere
   possibile: con un file sotto, fread non fallisce a comando. */
static void test_errore_a_meta(void)
{
    struct inode in;
    uint8_t buf[128];

    riempi_disco();
    prepara(&in, &bd_rw);
    fallisci_da = 0;

    check("errore al PRIMO settore rende -1", in.ops->read(&in, 0, buf, 64) == -1);

    riempi_disco();
    prepara(&in, &bd_rw);
    fallisci_da = 1;

    /* 500 + 100: i primi 12 byte vengono dal settore 0, che si legge; il resto dal
       settore 1, che fallisce. */
    check("errore al SECONDO settore rende i byte GIA' COPIATI",
          in.ops->read(&in, 500, buf, 100) == 12);
    check("e quei byte sono giusti", contenuto_giusto(buf, 500, 12));
}

/* ---- scrittura ----------------------------------------------------------- */

/* LA trappola della scrittura: parziale vuole READ-MODIFY-WRITE. Senza la lettura,
   i 502 byte intorno finiscono con quello che c'era nel bounce buffer — e non e'
   spazzatura casuale, sono dati veri di un altro settore, quindi hanno l'aria di
   essere giusti. E' la zona non azzerata di M11b. */
static void test_scrittura_parziale_preserva(void)
{
    struct inode in;
    uint8_t buf[8];
    int i;

    riempi_disco();
    prepara(&in, &bd_rw);

    for (i = 0; i < 5; i++)
        buf[i] = 0xAA;

    check("write di 5 byte all'offset 5 rende 5",
          in.ops->write(&in, 5, buf, 5) == 5);

    check("i 5 byte sono cambiati",
          disco[5] == 0xAA && disco[6] == 0xAA && disco[7] == 0xAA &&
          disco[8] == 0xAA && disco[9] == 0xAA);

    check("il byte PRIMA e' intatto", disco[4] == atteso(4));
    check("il byte DOPO e' intatto",  disco[10] == atteso(10));
    check("la fine del settore e' intatta",
          disco[SECTOR_SIZE - 1] == atteso(SECTOR_SIZE - 1));
    check("il settore successivo e' intatto",
          disco[SECTOR_SIZE] == atteso(SECTOR_SIZE));
}

static void test_scrittura_a_cavallo(void)
{
    struct inode in;
    static uint8_t buf[128];
    int i;

    riempi_disco();
    prepara(&in, &bd_rw);

    for (i = 0; i < 100; i++)
        buf[i] = 0x55;

    check("write di 100 byte dall'offset 500 rende 100",
          in.ops->write(&in, 500, buf, 100) == 100);
    check("gli ultimi byte del settore 0 sono cambiati",
          disco[500] == 0x55 && disco[SECTOR_SIZE - 1] == 0x55);
    check("i primi del settore 1 pure", disco[SECTOR_SIZE] == 0x55);
    check("il byte prima e' intatto", disco[499] == atteso(499));
    check("il byte dopo e' intatto",  disco[600] == atteso(600));
}

/* Una scrittura che copre il settore INTERO non deve leggerlo: sarebbe un giro di
   I/O buttato, e con la buffer cache di M12 sarebbe anche un settore sfrattato per
   niente. Si contano le chiamate. */
static void test_scrittura_di_un_settore_intero(void)
{
    struct inode in;
    static uint8_t buf[SECTOR_SIZE];
    int i;

    riempi_disco();
    prepara(&in, &bd_rw);

    for (i = 0; i < SECTOR_SIZE; i++)
        buf[i] = 0x33;

    check("write di un settore intero e allineato rende SECTOR_SIZE",
          in.ops->write(&in, 0, buf, SECTOR_SIZE) == SECTOR_SIZE);
    check("UNA sola chiamata al driver: nessuna lettura preventiva",
          chiamate == 1);
    check("il settore e' stato riscritto",
          disco[0] == 0x33 && disco[SECTOR_SIZE - 1] == 0x33);
    check("e quello dopo e' intatto",
          disco[SECTOR_SIZE] == atteso(SECTOR_SIZE));
}

static void test_scrittura_clamp_e_eof(void)
{
    struct inode in;
    uint8_t buf[64];
    int i;

    riempi_disco();
    prepara(&in, &bd_rw);

    for (i = 0; i < 64; i++)
        buf[i] = 0x11;

    /* Un disco NON cresce: la sua dimensione e' quella del supporto, a differenza
       di un file regolare su minix, dove scrivere oltre la fine lo allunga. */
    check("write che sfora rende solo i byte che ci stanno",
          in.ops->write(&in, DISCO - 8, buf, 64) == 8);
    check("e gli 8 byte in fondo sono cambiati", disco[DISCO - 1] == 0x11);
    check("write esattamente alla fine rende 0",
          in.ops->write(&in, DISCO, buf, 8) == 0);
    check("write oltre la fine rende 0",
          in.ops->write(&in, DISCO + 5000, buf, 8) == 0);
}

/* ---- caps ---------------------------------------------------------------- */

static void test_caps(void)
{
    struct dev_entry e;
    int i;

    for (i = 0; i < DEV_NAME_MAX; i++)
        e.name[i] = '\0';
    e.name[0] = 'h';

    e.kind = DEV_BLOCK; e.major = 3; e.minor = 0;

    e.impl = &bd_rw;
    check("un disco rw ha entrambe le capacita'",
          devio_caps(&e) == (DEVIO_CAN_READ | DEVIO_CAN_WRITE));

    /* Il ramo che in M11e/2 leggeva un puntatore non inizializzato: la maschera di
       un disco read-only deve avere SOLO la lettura. */
    e.impl = &bd_ro;
    check("un disco read-only ha solo la lettura",
          devio_caps(&e) == DEVIO_CAN_READ);

    e.impl = &bd_muto;
    check("un disco senza operazioni ha maschera 0", devio_caps(&e) == 0);

    e.kind = DEV_NONE; e.impl = &bd_rw;
    check("una voce di specie invalida rende 0", devio_caps(&e) == 0);

    check("una voce nulla rende 0", devio_caps(0) == 0);
}

/* ---- la registrazione asimmetrica ---------------------------------------- */

/* L'asimmetria fra i due wrapper, che fino a qui NON era coperta da nessun test:
   e' scritta in devio.h e implementata, ma niente la esercitava.

   Un disco da cui non si legge e' un errore del driver; uno su cui non si scrive e'
   un read-only legittimo. Un controllo condiviso in dev_register non potrebbe
   esprimerlo, perche' distingue solo "almeno uno dei due". */
static void test_registrazione_asimmetrica(void)
{
    dev_init();
    check("un disco rw si iscrive",
          blockdev_register("hda", 3, 0, &bd_rw) == 0);
    check("un disco READ-ONLY si iscrive: e' legittimo",
          blockdev_register("hdb", 3, 64, &bd_ro) == 0);
    check("un disco senza read e' RIFIUTATO",
          blockdev_register("hdc", 3, 128, &bd_muto) == -1);
    check("e non e' entrato", dev_count() == 2);

    check("blockdev_register rifiuta un impl nullo",
          blockdev_register("hdd", 3, 192, 0) == -1);
    check("e un nome nullo", blockdev_register(0, 3, 192, &bd_rw) == -1);
}

int main(void)
{
    test_fill_inode();
    test_lettura_allineata();
    test_lettura_non_allineata();
    test_lettura_a_cavallo();
    test_lettura_su_quattro_settori();
    test_clamp_e_eof();
    test_operazione_assente();
    test_errore_a_meta();
    test_scrittura_parziale_preserva();
    test_scrittura_a_cavallo();
    test_scrittura_di_un_settore_intero();
    test_scrittura_clamp_e_eof();
    test_caps();
    test_registrazione_asimmetrica();

    if (failures == 0) {
        printf("tutti i test dell'adapter byte<->LBA passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
