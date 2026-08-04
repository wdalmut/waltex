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

/* Riempie lo slot i se non lo e' gia', e dice se il dispositivo e' SERVIBILE.
   E' l'unico posto che scrive dentro il pool, e ci passano sia lookup sia readdir.

   "ino != 0" e' il marcatore di slot riempito, e non serve un flag a parte: e' il
   ragionamento di struct file senza flag di occupazione e del ring buffer senza
   contatore. Lo zero come marcatore non e' un caso — nessun inode vale zero,
   radice 1, /dev 2, dispositivi da 3 — ed e' la convenzione di procfs e delle voci
   di directory minix, dove lo zero significa "cancellata".

   Uno slot resta a zero quando devio_fill_inode RIFIUTA, cioe' quando la specie
   non ha ancora una vista a byte: i dischi, finche' blk_inode_ops non esiste.
   Quell'inode non si consegna, e la ragione e' precisa: vfs_read fa
   "f->inode->ops->read == 0" senza controllare che ops esista, quindi un inode
   senza vtable non fallisce, fa una tripla fault.

   IL RIEMPIMENTO E' PIGRO, e la ragione non e' la velocita'. Con devfs_init che
   fotografa il registry, un driver iscritto DOPO non compare in /dev e non c'e'
   nessun errore da nessuna parte — lo stesso difetto silenzioso di M11d spostato
   di un livello. Cosi' invece il pool si allinea da se' a ogni domanda.

   E per questo ci passa ANCHE readdir: con il riempimento pigro, un dispositivo
   mai cercato avrebbe lo slot vuoto, quindi un readdir che si fidasse del solo
   marcatore lo salterebbe — e sarebbe di nuovo una lista plausibile e incompleta.
   Facendo passare le due funzioni da qui, l'accordo fra lookup e readdir e' per
   COSTRUZIONE invece che per disciplina: e' la nota di M11a risolta alla radice
   invece che raccomandata.

   "ino" SI SCRIVE PER ULTIMO, e non e' estetica. Due task prelazionati cento volte
   al secondo possono entrare qui sullo stesso slot:

     - con ino per ultimo la corsa e' BENIGNA: scrivono valori identici, e chi
       arriva secondo vede ancora zero, rifa' il lavoro, e ottiene lo stesso
       risultato;
     - con ino PRIMA, il secondo vede ino != 0 e riceve un inode mezzo riempito,
       con ops ancora nullo — cioe' un salto attraverso un puntatore nullo alla
       prima read.

   Nessuna sezione critica serve, ed e' scritto perche' nessuno la aggiunga
   credendo di sistemare qualcosa: e' la disciplina del ring buffer di M5, dove la
   struttura sostituisce il cli. */
static int prepara(int i)
{
    if (ino_devices[i].ino != 0) {
        return 1;
    }

    if (devio_fill_inode(dev_get(i), &ino_devices[i]) < 0) {
        return 0;
    }

    ino_devices[i].ino = 3 + i;

    return 1;
}

static int dev_lookup(struct inode *dir, const char *name, struct inode **out)
{
    int i;

    (void)dir;

    /* Il nome lo risolve il REGISTRY, non un ciclo qui: dev_lookup_index fa il
       confronto esatto una volta sola, e l'indice che rende e' anche lo slot del
       pool. E' il patto 1:1 fra registry e ino_devices[].

       Ed e' anche cio' che rende il lazy init possibile: senza un indice stabile,
       riempire uno slot al primo lookup vorrebbe dire cercargliene uno libero, e
       due lookup dello stesso dispositivo potrebbero finire in due slot diversi —
       cioe' due inode per un file, che e' precisamente il difetto che la cache di
       M11a esiste per evitare. */
    i = dev_lookup_index(name);

    if (i < 0 || !prepara(i)) {
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
        if (!prepara(i)) {
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

/* NON legge il registry, e questa e' la differenza fra M11e/2 e M11e/3.

   Fino al passo 2 qui c'era un ciclo che fotografava il registry, e la fotografia
   aveva un difetto silenzioso: un driver iscritto DOPO non compariva in /dev, e
   non c'era nessun errore da nessuna parte. Adesso il pool lo riempie prepara(),
   a richiesta, quindi non esiste un istante "giusto" in cui guardare.

   Da cui il VINCOLO D'ORDINE DI KMAIN CHE SPARISCE. Fino al passo 2 devfs_init
   doveva stare dopo tutte le *_init() dei driver; adesso puo' stare dove si vuole,
   e resta un vincolo solo — quello opposto, dev_init() prima di tutti, perche'
   sono i driver a iscriversi.

   L'azzeramento resta, e serve: ogni *_init stabilisce uno STATO NOTO invece di
   aggiungere a quello che c'era. Nel kernel si chiama una volta sola, quindi
   ometterlo non romperebbe niente OGGI — che e' esattamente la nota di dev_init in
   dev.h — ma i test host la chiamano sei volte, e uno slot rimasto dal giro
   precedente farebbe consegnare l'inode vecchio col priv del dispositivo di
   prima. */
void devfs_init(void)
{
    memset(ino_devices, 0, sizeof(ino_devices));

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