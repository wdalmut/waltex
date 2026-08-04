/* Test dell'albero di devfs, col gcc dell'host.

   devfs non ha hardware sotto: legge il registry, che e' un array in .bss, e
   riempie inode. Quindi si prova interamente qui — ed e' il quinto modulo del
   progetto provato fuori da QEMU, dopo kprintf, il VFS, minixfs e procfs.

   Le inode_ops si chiamano DIRETTAMENTE, senza passare da vfs_resolve: e' il modo
   di provare l'albero senza dipendere dal risolutore, ed e' lo stesso argomento
   per cui vfs_resolve sta nell'header invece di essere provata attraverso
   vfs_open. Un FAIL qui parla di devfs e di nient'altro.

   I dispositivi finti sono struct chardev STATIC: da M11e il registry ne conserva
   il puntatore, quindi una locale sarebbe il bug che test_dev documenta. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "dev.h"
#include "chardev.h"
#include "blockdev.h"
#include "devio.h"
#include "devfs.h"
#include "vfs.h"

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

static int finta_read(struct chardev *c, void *buf, uint32_t n)
{
    (void)c; (void)buf;
    return (int)n;
}

static int finta_write(struct chardev *c, const void *buf, uint32_t n)
{
    (void)c; (void)buf;
    return (int)n;
}

/* STATIC, e non e' pigrizia: il registry ne conserva l'indirizzo. */
static struct chardev uno = { .read = finta_read };
static struct chardev due = { .write = finta_write };

/* Un disco finto, per il controllo che conta di questa milestone: devfs NON deve
   consegnare un inode per una specie che devio non sa ancora servire. */
static int finto_blk_read(struct blockdev *b, uint32_t lba, void *buf,
                          uint32_t count)
{
    (void)b; (void)lba; (void)buf;
    return (int)count;
}

static struct blockdev disco = {
    .name = "hda", .nsectors = 4, .read = finto_blk_read
};

/* ---- la radice e /dev ---------------------------------------------------- */

static void test_radice_e_devdir(void)
{
    struct inode *root, *devdir, *out;

    dev_init();
    devfs_init();

    root   = devfs_root();
    devdir = devfs_devdir();

    check("devfs_root non e' nullo dopo devfs_init", root != 0);
    check("devfs_devdir non e' nullo", devdir != 0);
    check("sono due inode DISTINTI", root != 0 && devdir != 0 && root != devdir);

    if (root == 0 || devdir == 0)
        return;

    check("la radice e' una directory", root->type == INODE_DIR);
    check("e /dev pure", devdir->type == INODE_DIR);

    /* La radice ha UNA voce e si chiama dev. E' la ragione per cui kmain monta
       devfs_devdir() e NON devfs_root(): innestando la radice si otterrebbe
       /dev/dev/kbd, e in M11a quattro self-check lo presero. */
    check("la radice contiene dev, e punta a devdir",
          root->ops != 0 && root->ops->lookup != 0 &&
          root->ops->lookup(root, "dev", &out) == 0 && out == devdir);

    /* Il controllo NEGATIVO, e vale gli altri messi insieme: una lookup che
       sbaglia il valore di ritorno sull'insuccesso — 1 invece di -1 — fa credere
       a vfs_resolve di aver trovato qualcosa, e da li' cammina su un puntatore
       mai inizializzato. Nessun controllo positivo lo vede. */
    check("la radice non contiene altro",
          root->ops->lookup(root, "pippo", &out) == -1);

    /* Nessun inode vale zero: lo zero significa "nessun inode" — e' il valore con
       cui una voce di directory minix dice "cancellata" — quindi usarlo per un
       file vero mente a chiunque lo controlli. */
    check("radice e devdir hanno ino non nullo",
          root->ino != 0 && devdir->ino != 0);
    check("e sono numeri diversi", root->ino != devdir->ino);
}

/* ---- l'identita' degli inode --------------------------------------------- */

/* LA lezione di M11a: lookup rende un puntatore che deve sopravvivere alla
   chiamata, e due lookup dello stesso file devono rendere lo STESSO puntatore.
   Altrimenti due size possono divergere, e in M16 due refcount. */
static void test_lookup_e_idempotente(void)
{
    struct inode *devdir, *a, *b;

    dev_init();
    check("l'iscrizione del finto riesce",
          chardev_register("uno", 5, 1, &uno) == 0);
    devfs_init();

    devdir = devfs_devdir();

    check("lookup trova il dispositivo",
          devdir->ops->lookup(devdir, "uno", &a) == 0);

    if (devdir->ops->lookup(devdir, "uno", &a) != 0)
        return;

    check("un secondo lookup rende lo STESSO puntatore",
          devdir->ops->lookup(devdir, "uno", &b) == 0 && a == b);

    check("l'inode e' un dispositivo a caratteri", a->type == INODE_CHARDEV);
    check("major e minor arrivano dalla voce del registry",
          a->major == 5 && a->minor == 1);
    check("priv punta all'IMPL e non alla voce", a->priv == (void *)&uno);
    check("ops e' stato riempito", a->ops != 0 && a->ops->read != 0);
    check("ino non e' zero", a->ino != 0);

    /* Un dispositivo non e' una directory: le tre caselle restano nulle, ed e' il
       motivo per cui mkdir /dev/x fallisce da se'. */
    check("un dispositivo non ha lookup ne' readdir ne' create",
          a->ops->lookup == 0 && a->ops->readdir == 0 && a->ops->create == 0);

    check("lookup su un nome assente rende -1",
          devdir->ops->lookup(devdir, "pippo", &b) == -1);
}

/* Che priv sia l'impl e non la voce non e' un dettaglio di gusto: chr_read casta
   priv a struct chardev *, e con la voce dentro leggerebbe i primi quattro byte
   del NOME come puntatore a funzione. Qui si esercita il percorso vero. */
static void test_read_arriva_al_driver(void)
{
    struct inode *devdir, *a;
    char buf[8];

    dev_init();
    chardev_register("uno", 5, 1, &uno);
    chardev_register("due", 5, 2, &due);
    devfs_init();

    devdir = devfs_devdir();

    if (devdir->ops->lookup(devdir, "uno", &a) != 0) {
        check("lookup di uno riesce", 0);
        return;
    }

    check("read attraversa l'adapter e arriva al driver",
          a->ops->read(a, 0, buf, 5) == 5);

    /* uno ha read e NON write: "non supportata" rende -1 e non 0, perche' uno
       zero direbbe "adesso non c'e' niente" invece di "rifiutato". */
    check("write su un dispositivo di sola lettura rende -1",
          a->ops->write(a, 0, buf, 5) == -1);

    if (devdir->ops->lookup(devdir, "due", &a) != 0) {
        check("lookup di due riesce", 0);
        return;
    }

    check("write arriva al driver", a->ops->write(a, 0, buf, 5) == 5);
    check("read su un dispositivo di sola scrittura rende -1",
          a->ops->read(a, 0, buf, 5) == -1);
}

/* ---- il lazy init -------------------------------------------------------- */

/* IL test che giustifica il Task 3, e che il devfs eager NON passa.

   Con devfs_init che fotografa il registry, un driver iscritto DOPO non compare in
   /dev — e non c'e' nessun errore da nessuna parte. E' lo stesso difetto silenzioso
   di M11d spostato di un livello. */
static void test_iscrizione_dopo_devfs_init(void)
{
    struct inode *devdir, *out;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;

    dev_init();
    chardev_register("uno", 5, 1, &uno);
    devfs_init();

    /* DOPO devfs_init. */
    check("l'iscrizione tardiva riesce",
          chardev_register("due", 5, 2, &due) == 0);

    devdir = devfs_devdir();

    check("il dispositivo iscritto DOPO devfs_init si trova con lookup",
          devdir->ops->lookup(devdir, "due", &out) == 0);

    check("e il suo priv e' quello giusto",
          devdir->ops->lookup(devdir, "due", &out) == 0 &&
          out->priv == (void *)&due);

    check("e compare anche in readdir",
          devdir->ops->readdir(devdir, 1, nome, &ino) == 1 &&
          same(nome, "due"));
}

/* ---- readdir ------------------------------------------------------------- */

static void test_readdir(void)
{
    struct inode *devdir, *out;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;

    dev_init();
    chardev_register("uno", 5, 1, &uno);
    chardev_register("due", 5, 2, &due);
    devfs_init();

    devdir = devfs_devdir();

    check("readdir rende la prima voce",
          devdir->ops->readdir(devdir, 0, nome, &ino) == 1 && same(nome, "uno"));
    check("readdir rende la seconda",
          devdir->ops->readdir(devdir, 1, nome, &ino) == 1 && same(nome, "due"));

    /* Zero e -1 sono distinti apposta: lo zero e' "le voci sono finite", il -1 e'
       "la domanda non aveva senso". Un ciclo che si fermasse su entrambi
       sembrerebbe funzionare fino al giorno in cui readdir fallisce davvero. */
    check("readdir oltre l'ultima rende 0",
          devdir->ops->readdir(devdir, 2, nome, &ino) == 0);
    check("readdir con un indice negativo NON rende 1",
          devdir->ops->readdir(devdir, -1, nome, &ino) <= 0);

    /* I numeri di readdir devono essere quelli degli inode che lookup rende: se
       divergessero, ls mostrerebbe numeri che nessun open ritrova. */
    devdir->ops->readdir(devdir, 0, nome, &ino);
    devdir->ops->lookup(devdir, "uno", &out);
    check("il numero di readdir e' quello dell'inode di lookup", ino == out->ino);
}

/* LA trappola di procfs, portata qui: idx e' una POSIZIONE, non un indice del
   registry.

   Il buco si costruisce a mano iscrivendo un DISCO in mezzo ai due chardev:
   finche' blk_inode_ops non esiste, devio_fill_inode lo rifiuta e devfs lo salta,
   quindi la voce 1 del registry non ha un inode. La posizione 1 di readdir deve
   essere il secondo dispositivo SERVITO, non il secondo del registry.

   Confonderli fa fermare l'elenco al primo salto, e il sintomo e' il peggiore che
   ci sia: una lista PLAUSIBILE e INCOMPLETA. */
static void test_readdir_con_un_buco(void)
{
    struct inode *devdir;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;

    dev_init();
    chardev_register("uno", 5, 1, &uno);
    check("il disco si iscrive nel registry",
          blockdev_register("hda", 3, 0, &disco) == 0);
    chardev_register("due", 5, 2, &due);
    devfs_init();

    check("il registry ne contiene TRE", dev_count() == 3);

    devdir = devfs_devdir();

    check("la posizione 0 e' il primo servito",
          devdir->ops->readdir(devdir, 0, nome, &ino) == 1 && same(nome, "uno"));

    /* La riga che conta: "due" e' l'indice 2 del registry e la POSIZIONE 1. */
    check("la posizione 1 salta il disco e da' il secondo servito",
          devdir->ops->readdir(devdir, 1, nome, &ino) == 1 && same(nome, "due"));

    check("dopo i due serviti l'elenco e' finito",
          devdir->ops->readdir(devdir, 2, nome, &ino) == 0);
}

/* lookup e readdir devono essere D'ACCORDO sul disco: se comparisse solo nella
   prima, cat /dev/hda funzionerebbe e ls /dev non lo mostrerebbe. E' la nota di
   M11a, e qui e' un controllo invece di una raccomandazione. */
static void test_il_disco_non_e_servito(void)
{
    struct inode *devdir, *out;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;
    int i, trovato = 0;

    dev_init();
    chardev_register("uno", 5, 1, &uno);
    blockdev_register("hda", 3, 0, &disco);
    devfs_init();

    devdir = devfs_devdir();

    /* Finche' devio non sa fabbricare la vista a byte di un disco, /dev non deve
       consegnarne l'inode: un inode con ops nullo non fallisce, fa una tripla
       fault dentro vfs_read, che dereferenzia ops senza controllarlo.

       Questo controllo cambia di senso nel Task 4 — quando l'adapter arriva, hda
       DEVE comparire — ed e' voluto: e' la stessa domanda posta in due momenti, e
       il passaggio dall'una all'altra e' la milestone. */
    check("lookup del disco fallisce, per ora",
          devdir->ops->lookup(devdir, "hda", &out) == -1);

    for (i = 0; devdir->ops->readdir(devdir, i, nome, &ino) == 1; i++) {
        if (same(nome, "hda"))
            trovato = 1;
    }

    check("e nemmeno readdir lo elenca: le due sono D'ACCORDO", !trovato);
}

/* /dev VUOTA e' uno stato legittimo, e nel kernel esiste: i self-check girano dopo
   i driver, ma un boot senza dischi ne ha tre invece di cinque, e un boot senza
   nessun driver ne avrebbe zero. */
static void test_registry_vuoto(void)
{
    struct inode *devdir, *out;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;

    dev_init();
    devfs_init();

    devdir = devfs_devdir();

    check("con il registry vuoto /dev non ha voci",
          devdir->ops->readdir(devdir, 0, nome, &ino) == 0);
    check("e lookup non trova niente",
          devdir->ops->lookup(devdir, "uno", &out) == -1);
    check("ma la radice contiene ancora dev",
          devfs_root()->ops->lookup(devfs_root(), "dev", &out) == 0);
}

/* devfs_init deve stabilire uno STATO NOTO, non aggiungere a quello di prima.
   Nel kernel si chiama una volta sola, quindi ometterlo non romperebbe niente
   OGGI — che e' esattamente la nota di dev_init in dev.h. Qui si chiama sei
   volte, e uno slot con ino != 0 rimasto dal giro precedente farebbe consegnare
   l'inode vecchio, con il priv del dispositivo di prima. */
static void test_devfs_init_azzera(void)
{
    struct inode *devdir, *out;

    dev_init();
    chardev_register("uno", 5, 1, &uno);
    devfs_init();

    /* Secondo giro con un dispositivo DIVERSO nello stesso slot 0. */
    dev_init();
    chardev_register("due", 5, 2, &due);
    devfs_init();

    devdir = devfs_devdir();

    check("dopo un secondo devfs_init il vecchio nome non si trova piu'",
          devdir->ops->lookup(devdir, "uno", &out) == -1);

    check("e lo slot riusato porta il priv NUOVO",
          devdir->ops->lookup(devdir, "due", &out) == 0 &&
          out->priv == (void *)&due);
}

int main(void)
{
    test_radice_e_devdir();
    test_lookup_e_idempotente();
    test_read_arriva_al_driver();
    test_iscrizione_dopo_devfs_init();
    test_readdir();
    test_readdir_con_un_buco();
    test_il_disco_non_e_servito();
    test_registry_vuoto();
    test_devfs_init_azzera();

    if (failures == 0) {
        printf("tutti i test dell'albero di devfs passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
