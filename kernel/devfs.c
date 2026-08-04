#include "devfs.h"
#include "vfs.h"
#include "dev.h"
#include "devio.h"
#include "memory.h"

static struct inode ino_root;                 /* "/"        ino 1 */
static struct inode ino_dev;                  /* "/dev"     ino 2 */
static struct inode ino_devices[DEV_MAX]; /* uno per dispositivo, ino 3+ */
static int ready = 0;                            /* devfs_init e' stata chiamata? */

static int root_lookup(struct inode *dir, const char *name, struct inode **out);
static int root_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out);
static int dev_lookup(struct inode *dir, const char *name, struct inode **out);
static int dev_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out);

/* Inizializzatori DESIGNATI, e non e' stile: da M11b inode_ops ha cinque campi
   e devfs ne usa due o tre. Con la forma posizionale il compilatore segnala
   "missing initializer for field 'create'" a ogni build — un avviso permanente
   e giusto, cioe' un avviso che si smette di leggere. Cosi' invece i campi
   assenti restano a zero DICHIARANDOLO, che e' la convenzione di M8:
   puntatore nullo uguale operazione non supportata. */
static const struct inode_ops ops_root = {
    .lookup = root_lookup, .readdir = root_readdir
};

static const struct inode_ops ops_dev = {
    .lookup = dev_lookup, .readdir = dev_readdir
};

static int root_lookup(struct inode *dir, const char *name, struct inode **out)
{
    (void)dir;

    if (strcmp(name, "dev") == 0) {
        *out = &ino_dev;
        return 0;
    }

    return -1;
}

static int root_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out)
{
    (void)dir;

    int r = 0;
    if (idx == 0) {
        memcpy(name, "dev", 4);
        *ino_out = ino_dev.ino;

        r = 1;
    } 

    return r;
}

/* "ino != 0" e' il marcatore di SLOT RIEMPITO, e le due funzioni sotto lo
   guardano entrambe.

   Uno slot resta a zero quando devio_fill_inode ha rifiutato, cioe' quando la
   specie del dispositivo non ha ancora una vista a byte — i dischi, finche'
   blk_inode_ops non esiste. Quell'inode NON si consegna, e la ragione e' precisa:
   vfs_read fa "f->inode->ops->read == 0" senza controllare che ops esista, quindi
   un inode senza vtable non fallisce, fa una tripla fault.

   Lo zero come marcatore non e' un caso: nessun inode vale zero — radice 1, /dev
   2, dispositivi da 3 — ed e' la convenzione di procfs e delle voci di directory
   minix, dove lo zero significa "cancellata". */
static int servito(int i)
{
    return ino_devices[i].ino != 0;
}

static int dev_lookup(struct inode *dir, const char *name, struct inode **out)
{
    int i;

    (void)dir;

    /* Il nome lo risolve il REGISTRY, non un ciclo qui: dev_lookup_index fa il
       confronto esatto una volta sola, e l'indice che rende e' anche lo slot del
       pool. E' il patto 1:1 fra registry e ino_devices[]. */
    i = dev_lookup_index(name);

    if (i < 0 || !servito(i)) {
        return -1;
    }

    *out = &ino_devices[i];

    return 0;
}

static int dev_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out)
{
    int i, pos = 0;

    (void)dir;

    /* Un indice negativo e' una domanda senza senso, non "elenco finito": sono i
       due valori che vfs_readdir distingue apposta. */
    if (idx < 0) {
        return -1;
    }

    /* idx e' una POSIZIONE, non un indice del registry, e i due divergono appena
       uno slot non e' servito: con hda al posto 3 del registry e saltato, le
       posizioni 0,1,2 restano i tre dispositivi a caratteri.

       Confonderli fa fermare l'elenco al primo salto, e il sintomo e' il peggiore
       che ci sia — una lista PLAUSIBILE e INCOMPLETA. E' la stessa trappola di
       readdir in procfs, dove idx era una posizione e non un indice di task.

       Oggi non si vedrebbe, perche' i tre chardev si iscrivono prima dei due
       dischi e le posizioni coincidono con gli indici. Si vedrebbe il giorno che
       un driver a caratteri si iscrive dopo ata_init. */
    for (i = 0; i < dev_count(); i++) {
        if (!servito(i)) {
            continue;
        }

        if (pos == idx) {
            const struct dev_entry *e = dev_get(i);

            /* VFS_NAME_MAX e' 14 e DEV_NAME_MAX e' 16: un nome di dispositivo piu'
               lungo di 14 caratteri non entra in una voce di directory e qui viene
               troncato. Nessuno dei cinque ci arriva — il piu' lungo e' "console",
               7 — e il buffer del chiamante vuole VFS_NAME_MAX + 1 byte, che e' il
               contratto di vfs_readdir. */
            memcpy(name, e->name, VFS_NAME_MAX);
            name[VFS_NAME_MAX] = '\0';

            *ino_out = ino_devices[i].ino;

            return 1;
        }

        pos++;
    }

    return 0;
}

void devfs_init(void)
{
    int i;

    /* Ogni *_init stabilisce uno STATO NOTO, e non e' cerimonia: i test host
       chiamano devfs_init piu' volte, e uno slot con ino != 0 rimasto dal giro
       precedente farebbe consegnare l'inode vecchio, con il priv del dispositivo
       di prima. Nel kernel si chiama una volta sola, quindi ometterlo non
       romperebbe niente OGGI — che e' esattamente la nota di dev_init in dev.h, e
       la ragione per cui questa riga c'e'. */
    memset(ino_devices, 0, sizeof(ino_devices));

    for (i = 0; i < dev_count(); i++) {
        const struct dev_entry *e = dev_get(i);

        /* Il salto lo decide DEVIO col valore di ritorno, non devfs guardando
           kind — e la differenza e' il confine della milestone. Guardando kind,
           devfs dovrebbe sapere quali specie esistono, cioe' il switch tornerebbe
           a vivere in due posti. Cosi' invece devfs chiede "sai servirmi questo?"
           e non gli importa perche' no: quando arriva blk_inode_ops, questo file
           non cambia di una riga. */
        if (devio_fill_inode(e, &ino_devices[i]) < 0) {
            continue;
        }

        /* ino per ULTIMO, e fuori da devio_fill_inode di proposito: e' il
           marcatore di "slot riempito", quindi scriverlo prima del resto vorrebbe
           dire dichiarare pronto un inode che non lo e' ancora — e con la
           prelazione un altro task potrebbe raccoglierlo con ops nullo. */
        ino_devices[i].ino = 3 + i;
    }

    ino_root.ino = 1;
    ino_root.type = INODE_DIR;
    ino_root.ops = &ops_root;
    ino_root.size = 0;

    ino_dev.ino = 2;
    ino_dev.type = INODE_DIR;
    ino_dev.ops = &ops_dev;
    ino_dev.size = 0;

    ready = 1;
}

struct inode *devfs_root(void)
{
    if (!ready) {
        return 0;
    }

    return &ino_root;
}

/* La directory /dev, non la radice.

   Da M11a la radice viene da minix e devfs diventa un innesto: cio' che si
   innesta sotto il nome "dev" e' QUESTA directory. Innestando devfs_root() si
   otterrebbe /dev/dev/kbd, perche' la radice di devfs ha una sola voce e si
   chiama "dev". */
struct inode *devfs_devdir(void)
{
    if (!ready) {
        return 0;
    }

    return &ino_dev;
}