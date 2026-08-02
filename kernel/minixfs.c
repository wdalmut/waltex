#include "minixfs.h"
#include "types.h"
#include "blockdev.h"
#include "memory.h"

/* minix v1, sola lettura. Il formato e' quello VERIFICATO su un'immagine di
   mkfs.minix con od, non ricordato: i numeri nei commenti vengono da
   tests/data/minix.img.

   Questo file e' l'unico punto del kernel in cui il formato di minix tocca
   qualcosa. Da inode_carica in su nessuno sa piu' che esistano le zone — ed e'
   il motivo per cui lo stesso cat di M9b legge da qui senza una modifica. */

#define MINIX_MAGIC        0x137F   /* v1, nomi da 14. La 0x138F si RIFIUTA */
#define MINIX_ROOT_INO     1        /* la radice, per convenzione */

#define BLOCK_SIZE         1024
#define SETTORI_PER_BLOCCO (BLOCK_SIZE / SECTOR_SIZE)   /* 2 */

#define INODE_SIZE         32
#define INODE_PER_BLOCCO   (BLOCK_SIZE / INODE_SIZE)    /* 32, non 16 */

#define ZONE_DIRETTE       7
#define ZONA_INDIRETTA     7        /* l'indice in i_zone[], non una zona */
#define ZONA_DOPPIA        8
#define PTR_PER_BLOCCO     (BLOCK_SIZE / 2)             /* 512 uint16 */

#define DIRENT_SIZE        16
#define MINIX_NAME_MAX     14

/* ---- il formato su disco ---------------------------------------------------

   Tre struct che descrivono byte, non oggetti. packed su tutte e tre: senza, il
   compilatore infila padding e ogni campo dopo il primo e' spostato. La verifica
   e' che sizeof(struct minix_inode) faccia 32, non 36. */

struct minix_superblock {
    uint16_t s_ninodes;         /*  96 sull'immagine di prova */
    uint16_t s_nzones;          /* 256 */
    uint16_t s_imap_blocks;     /*   1 */
    uint16_t s_zmap_blocks;     /*   1 */
    uint16_t s_firstdatazone;   /*   7 */
    uint16_t s_log_zone_size;   /*   0 = una zona e' un blocco */
    uint32_t s_max_size;
    uint16_t s_magic;           /* 0x137F */
    uint16_t s_state;
} __attribute__((packed));

struct minix_inode {
    uint16_t i_mode;            /* 0o40755 directory, 0o100644 file */
    uint16_t i_uid;
    uint32_t i_size;            /* i BYTE, ed e' l'unica cosa che dice dove
                                   finisce un file */
    uint32_t i_time;
    uint8_t  i_gid;
    uint8_t  i_nlinks;
    uint16_t i_zone[9];         /* 7 dirette, 1 indiretta, 1 doppia indiretta.
                                   uint16 e non uint32: quella e' la v2 */
} __attribute__((packed));

struct minix_dirent {
    uint16_t ino;               /* 0 = voce cancellata, NON fine dell'elenco */
    char     name[MINIX_NAME_MAX];  /* NON terminato se lungo esattamente 14 */
} __attribute__((packed));

/* ---- lo stato --------------------------------------------------------------

   struct inode del VFS non ha posto per le nove zone, e non deve averlo: e' lo
   stesso tipo per devfs, per minix e per ogni filesystem futuro, e "zona" e' una
   parola di minix. Quello che riguarda solo noi va in priv.

   Da cui l'involucro. L'alternativa sarebbe un secondo array parallelo
   zone[MAX_INODES][9], che funziona e ha il difetto degli indici che scivolano —
   lo stesso di ino_devices[i] e device_at(i) in M9b. Qui inode e zone sono lo
   stesso oggetto, quindi non possono divergere. */
struct minode {
    struct inode vfs;           /* cio' che si consegna fuori */
    uint16_t     zone[9];       /* cio' che serve solo a noi  */
};

static struct blockdev        *disco;
static struct minix_superblock sb;            /* una COPIA, non un puntatore */
static uint32_t                blocco_inodi;  /* 2 + imap + zmap = 4 */
static int                     montato;

/* La cache. NON e' indicizzata per numero di inode: gli inode sull'immagine
   sono 96 e su un disco vero migliaia, mentre gli slot sono 64. Ogni slot
   ricorda quale inode contiene in vfs.ino, e lo si cerca scorrendo.

   E' la stessa forma di files[] in vfs.c — array a capacita' fissa, ricerca
   lineare, e il marcatore di "libero" e' il dato stesso: zero non e' un numero
   di inode valido, quindi non serve un campo "usato" a parte, che sarebbe una
   seconda verita' da tenere allineata.

   Non e' un'ottimizzazione ma correttezza: lookup restituisce un puntatore, che
   deve sopravvivere alla chiamata, e due lookup dello stesso file devono dare
   lo STESSO puntatore — con due copie, la size di una puo' divergere. */
static struct minode cache[MAX_INODES];

/* L'innesto: UNO slot, non una tabella di mount. Il nome e' un array e non un
   const char *, per la regola di device_register in M8: chi chiama puo' passare
   un letterale oggi e un buffer domani. */
static struct {
    char          nome[VFS_NAME_MAX + 1];
    struct inode *root;
} innesto;

static int minix_read(struct inode *ino, uint32_t off, void *buf, uint32_t n);
static int minix_lookup(struct inode *dir, const char *name,
                        struct inode **out);
static int minix_readdir(struct inode *dir, int idx, char *name,
                         uint32_t *ino_out);

/* write a zero: M11a e' di sola lettura, e "puntatore nullo uguale operazione
   non supportata" e' la convenzione di M8, ereditata due volte. */
static const struct inode_ops ops_minix = {
    minix_read, 0, minix_lookup, minix_readdir
};

/* ---- il disco --------------------------------------------------------------

   I tre buffer da un blocco sono static e SEPARATI, e la separazione non e'
   spreco: zona_di viene chiamata da minix_read, e con un buffer condiviso la
   lettura del blocco indiretto calpesterebbe i dati appena letti. Tre kilobyte
   di .bss contro un guasto che comparirebbe solo sui file oltre i 7 KB.

   static e non locali perche' 1024 byte su uno stack da 4096 sono un quarto, e
   le chiamate si annidano. Il prezzo e' la rientranza: in M16, con piu' shell,
   questo e' il primo posto da cambiare. */

static int blocco_leggi(uint32_t blocco, void *buf)
{
    if (disco == 0 || disco->read == 0)
        return -1;

    /* L'unico posto del file che sa che un blocco sono DUE settori. Se il * 2
       comparisse sparso, prima o poi uno dei tre sarebbe diverso. */
    if (disco->read(disco, blocco * SETTORI_PER_BLOCCO, buf,
                    SETTORI_PER_BLOCCO) != SETTORI_PER_BLOCCO)
        return -1;

    return 0;
}

/* ---- il mount -------------------------------------------------------------- */

static enum inode_type tipo_da_mode(uint16_t mode)
{
    switch (mode & 0170000) {
    case 0040000: return INODE_DIR;
    case 0100000: return INODE_FILE;
    case 0020000: return INODE_CHARDEV;
    default:      return INODE_NONE;
    }
}

int minixfs_init(struct blockdev *dev)
{
    static uint8_t buf[BLOCK_SIZE];
    const struct minix_superblock *d;

    /* Si invalida SUBITO e si rimonta solo alla fine: fra queste due righe lo
       stato e' "non montato", che e' esattamente cio' che serve se si esce da
       uno dei return in mezzo. Un mount fallito a meta', con il disco impostato
       e il superblocco a zeri, e' peggio di uno fallito del tutto. */
    montato = 0;
    disco   = 0;

    if (dev == 0)
        return -1;

    disco = dev;                    /* blocco_leggi lo usa */

    /* Il BLOCCO 1, cioe' i settori 2 e 3. Il settore 0 e' il boot block ed e'
       tutto zeri: leggendo li', il magic verrebbe 0 e il mount fallirebbe. */
    if (blocco_leggi(1, buf) < 0) {
        disco = 0;
        return -1;
    }

    d = (const struct minix_superblock *)buf;

    /* Solo 0x137F. La 0x138F e' la variante con nomi da 30, dove la voce di
       directory e' lunga 32 byte invece di 16: leggerla come da 16 non produce
       nessun errore, produce nomi finti e numeri di inode presi dal mezzo di un
       nome. Cioe' un filesystem che sembra funzionare.

       s_log_zone_size diverso da zero significa che una zona non e' un blocco,
       e ogni numero di zona andrebbe spostato. Si rifiuta invece di ignorarlo,
       per la stessa ragione. */
    if (d->s_magic != MINIX_MAGIC || d->s_log_zone_size != 0) {
        disco = 0;
        return -1;
    }

    /* Una COPIA: buf viene riusato da ogni lettura successiva. */
    memcpy(&sb, d, sizeof(struct minix_superblock));

    /* Derivato una volta e tenuto: compare in ogni accesso a un inode, e
       ricalcolarlo altrove sarebbe la seconda verita' che diverge. Il 2 sono
       boot block e superblocco, che ci sono sempre; le due bitmap hanno
       lunghezza variabile e il superblocco la dichiara.

       Controllo che torna: 96 inode a 32 per blocco sono 3 blocchi, e
       4 + 3 = 7 = s_firstdatazone. */
    blocco_inodi = 2u + sb.s_imap_blocks + sb.s_zmap_blocks;

    /* Azzerare non e' pedanteria: in .bss parte gia' a zeri, quindi
       dimenticarlo funziona al PRIMO mount e si rompe al secondo. E' il
       tranello di device_init in M8, e c'e' un test apposta. */
    memset(cache, 0, sizeof(cache));
    memset(&innesto, 0, sizeof(innesto));

    montato = 1;
    return 0;
}

/* ---- gli inode ------------------------------------------------------------- */

static struct inode *inode_carica(uint32_t ino)
{
    static uint8_t buf[BLOCK_SIZE];
    const struct minix_inode *d;
    struct minode *m = 0;
    uint32_t blocco, offset;
    int i, k;

    if (!montato)
        return 0;

    /* Lo zero non e' un inode: significa "nessuno", ed e' il valore con cui una
       voce di directory dice "cancellata". */
    if (ino == 0 || ino > sb.s_ninodes)
        return 0;

    /* Se c'e' gia', si restituisce QUELLO. Non per velocita': due struct inode
       per lo stesso file avrebbero due size che possono divergere. */
    for (i = 0; i < MAX_INODES; i++)
        if (cache[i].vfs.ino == ino)
            return &cache[i].vfs;

    for (i = 0; i < MAX_INODES; i++)
        if (cache[i].vfs.ino == 0) {
            m = &cache[i];
            break;                  /* il PRIMO libero, e si esce */
        }

    if (m == 0)
        return 0;                   /* slot esauriti: deterministico e visibile */

    /* L'unica sottrazione da non sbagliare: la radice e' l'inode 1, quindi il
       primo inode sta all'offset 0 della tabella. Con ino * 32 tutto slitta di
       uno, e la radice sembrerebbe funzionare comunque — l'inode 0 letto e'
       tutto zeri, cioe' una directory vuota, che non e' un errore. Il guasto
       comparirebbe al primo file.

       32 inode per blocco, non 16: 1024 / 32. */
    blocco = blocco_inodi + (ino - 1) / INODE_PER_BLOCCO;
    offset = ((ino - 1) % INODE_PER_BLOCCO) * INODE_SIZE;

    if (blocco_leggi(blocco, buf) < 0)
        return 0;

    d = (const struct minix_inode *)(buf + offset);

    m->vfs.type = tipo_da_mode(d->i_mode);

    if (m->vfs.type == INODE_NONE)
        return 0;

    m->vfs.size  = d->i_size;
    m->vfs.major = 0;
    m->vfs.minor = 0;
    m->vfs.ops   = &ops_minix;
    m->vfs.priv  = m->zone;

    for (k = 0; k < 9; k++)
        m->zone[k] = d->i_zone[k];

    /* ino si scrive per ULTIMO, perche' e' il marcatore di "slot occupato":
       scrivendolo prima, un ritorno anticipato lascerebbe uno slot preso da un
       inode riempito a meta'. */
    m->vfs.ino = ino;

    return &m->vfs;
}

struct inode *minixfs_root(void)
{
    if (!montato)
        return 0;

    return inode_carica(MINIX_ROOT_INO);
}

/* ---- le zone ---------------------------------------------------------------

   Data la zona logica n di un file — il byte n * 1024 — quale zona fisica la
   contiene. E' il cuore di M11a.

   Misurato su enorme.txt, 20000 byte: zone[0..6] = 16..22 sono i primi 7168
   byte, e zone[7] = 23 e' il blocco INDIRETTO, che contiene 24 25 ... 36. */

static uint32_t zona_di(struct inode *ino, uint32_t n)
{
    static uint8_t buf[BLOCK_SIZE];
    const uint16_t *ptr  = (const uint16_t *)buf;
    const uint16_t *zone = (const uint16_t *)ino->priv;
    uint32_t z;

    if (zone == 0)
        return 0;

    /* Il caso comune — ogni file sotto i 7 KB — e non legge niente in piu'. */
    if (n < ZONE_DIRETTE)
        return zone[n];

    n -= ZONE_DIRETTE;

    if (n < PTR_PER_BLOCCO) {
        /* zone[7] a zero significa che l'intero intervallo e' un buco. NON si
           legge il blocco 0, che sarebbe il boot block interpretato come una
           tabella di puntatori. */
        if (zone[ZONA_INDIRETTA] == 0)
            return 0;

        if (blocco_leggi(zone[ZONA_INDIRETTA], buf) < 0)
            return 0;

        return ptr[n];
    }

    n -= PTR_PER_BLOCCO;

    if (n < PTR_PER_BLOCCO * PTR_PER_BLOCCO) {
        if (zone[ZONA_DOPPIA] == 0)
            return 0;

        if (blocco_leggi(zone[ZONA_DOPPIA], buf) < 0)
            return 0;

        z = ptr[n / PTR_PER_BLOCCO];

        if (z == 0)
            return 0;

        if (blocco_leggi(z, buf) < 0)
            return 0;

        return ptr[n % PTR_PER_BLOCCO];
    }

    return 0;
}

/* ---- leggere --------------------------------------------------------------- */

static int minix_read(struct inode *ino, uint32_t off, void *buf, uint32_t n)
{
    static uint8_t blocco[BLOCK_SIZE];
    uint8_t *dest = (uint8_t *)buf;
    uint32_t fatti = 0;
    uint32_t z, dentro, quanti, i;

    if (ino == 0 || buf == 0)
        return -1;

    /* Lo zero significa "finito", non un errore: e' la convenzione di M8, e su
       un file regolare vuol dire che la posizione ha raggiunto size. */
    if (off >= ino->size)
        return 0;

    /* IL troncamento. Senza, un file da 26 byte ne restituisce 1024, e i 998 in
       piu' non sono spazzatura casuale: sono dati veri di qualcun altro, gia'
       presenti sul disco, quindi hanno l'aria di essere giusti.

       Per sottrazione e non off + n > size, che con off vicino a 2^32
       girerebbe: la stessa lezione di ata_range_ok in M10. */
    if (n > ino->size - off)
        n = ino->size - off;

    while (fatti < n) {
        dentro = off % BLOCK_SIZE;
        quanti = BLOCK_SIZE - dentro;

        if (quanti > n - fatti)
            quanti = n - fatti;

        z = zona_di(ino, off / BLOCK_SIZE);

        if (z == 0) {
            /* Buco: un file sparso ha zone non allocate in mezzo, e valgono
               zeri. Non e' la fine del file — quella la dice size. */
            for (i = 0; i < quanti; i++)
                dest[fatti + i] = 0;
        } else {
            if (blocco_leggi(z, blocco) < 0)
                return fatti > 0 ? (int)fatti : -1;

            memcpy(dest + fatti, blocco + dentro, quanti);
        }

        off   += quanti;
        fatti += quanti;
    }

    return (int)fatti;
}

/* ---- le directory ----------------------------------------------------------

   Una directory e' un FILE NORMALE il cui contenuto sono voci da 16 byte.
   Quindi queste due funzioni non leggono il disco da se': scorrono la directory
   con la stessa minix_read che serve i file. */

/* Il nome su disco e' lungo al massimo 14 e NON e' terminato se ne occupa
   esattamente 14: un strcmp diretto sul campo camminerebbe nella voce
   successiva. */
static void copia_nome(char *dest, const char *src)
{
    int i;

    for (i = 0; i < MINIX_NAME_MAX && src[i] != '\0'; i++)
        dest[i] = src[i];

    dest[i] = '\0';
}

static int minix_lookup(struct inode *dir, const char *name,
                        struct inode **out)
{
    struct minix_dirent voce;
    char nome[MINIX_NAME_MAX + 1];
    struct inode *trovato;
    uint32_t off;

    if (dir == 0 || name == 0 || out == 0)
        return -1;

    if (dir->type != INODE_DIR)
        return -1;

    /* L'innesto, e solo sulla radice: si controlla PRIMA del disco. E' tutto
       cio' che c'e' di un mount — una domanda a cui lookup risponde senza
       guardare il filesystem. */
    if (dir->ino == MINIX_ROOT_INO && innesto.root != 0 &&
        strcmp(name, innesto.nome) == 0) {
        *out = innesto.root;
        return 0;
    }

    for (off = 0; off < dir->size; off += DIRENT_SIZE) {
        if (minix_read(dir, off, &voce, DIRENT_SIZE) != DIRENT_SIZE)
            break;

        /* Voce CANCELLATA, non fine dell'elenco: fermarsi qui farebbe sparire
           tutto quello che segue. mkfs.minix non ne produce, ma dopo il primo
           unlink di M11b si'. */
        if (voce.ino == 0)
            continue;

        copia_nome(nome, voce.name);

        if (strcmp(nome, name) == 0) {
            trovato = inode_carica(voce.ino);

            if (trovato == 0)
                return -1;

            /* *out si scrive SOLO in caso di successo, ed e' il bug di
               root_lookup in M9b: vfs_resolve controlla < 0, quindi un valore
               di ritorno sbagliato gli fa credere di aver trovato e lo manda a
               camminare su un puntatore mai inizializzato. */
            *out = trovato;
            return 0;
        }
    }

    return -1;
}

static int minix_readdir(struct inode *dir, int idx, char *name,
                         uint32_t *ino_out)
{
    struct minix_dirent voce;
    uint32_t voci;

    if (dir == 0 || name == 0 || ino_out == 0 || idx < 0)
        return -1;

    if (dir->type != INODE_DIR)
        return -1;

    voci = dir->size / DIRENT_SIZE;

    if ((uint32_t)idx < voci) {
        /* idx * 16 e' direttamente la posizione: non serve rileggere la
           directory dall'inizio per arrivare alla voce idx. */
        if (minix_read(dir, (uint32_t)idx * DIRENT_SIZE, &voce,
                       DIRENT_SIZE) != DIRENT_SIZE)
            return -1;

        /* Le voci cancellate si consegnano cosi' come sono, con ino a zero e il
           nome vuoto, e NON si saltano: idx e' una posizione, e saltare
           renderebbe gli indici instabili — chi enumera chiede 0, 1, 2, e se la
           voce 1 sparisse la 2 diventerebbe la 1. Tocca a chi elenca ignorare
           ino_out == 0. */
        copia_nome(name, voce.name);
        *ino_out = voce.ino;
        return 1;
    }

    /* L'innesto e' la voce subito dopo quelle su disco. Deve comparire qui,
       altrimenti si ottiene un /dev che cat apre e ls non mostra: lookup e
       readdir descrivono lo stesso insieme, e niente le costringe a essere
       d'accordo se non questa riga. */
    if (dir->ino == MINIX_ROOT_INO && innesto.root != 0 &&
        (uint32_t)idx == voci) {
        copia_nome(name, innesto.nome);
        *ino_out = innesto.root->ino;
        return 1;
    }

    /* Zero, non -1: "le voci sono finite" e "la domanda non aveva senso" sono
       due cose diverse, e un ciclo che si fermasse su entrambe sembrerebbe
       funzionare fino al giorno in cui readdir fallisce davvero. */
    return 0;
}

/* ---- l'innesto -------------------------------------------------------------- */

int minixfs_graft(const char *nome, struct inode *root)
{
    size_t len, i;

    /* Dopo il mount, che azzera l'innesto: l'ordine sbagliato lo cancellerebbe
       in silenzio. */
    if (!montato || nome == 0 || root == 0)
        return -1;

    /* Uno slot solo. Si RIFIUTA invece di sostituire, perche' una sostituzione
       silenziosa sarebbe una directory che cambia sotto i piedi. */
    if (innesto.root != 0)
        return -1;

    len = strlen(nome);

    if (len == 0 || len > VFS_NAME_MAX)
        return -1;

    /* Si COPIA il nome, non si tiene il puntatore: la regola di
       device_register in M8, perche' chi chiama puo' passare un letterale oggi
       e un buffer domani. */
    for (i = 0; i < len; i++)
        innesto.nome[i] = nome[i];

    innesto.nome[len] = '\0';
    innesto.root = root;

    return 0;
}
