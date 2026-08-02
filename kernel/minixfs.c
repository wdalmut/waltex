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

    /* Due campi che da M11b servono e che struct inode non ha.

       mode e' il i_mode COMPLETO, permessi inclusi: tenerlo qui invece di
       ricostruirlo dal solo tipo evita che riscrivere un inode gli cambi i
       permessi sotto il naso.

       nlinks lo controlla fsck, ed e' il numero che si sbaglia su mkdir: una
       directory nasce a 2 — "." piu' la voce nel genitore — e il genitore ne
       guadagna uno per via di "..". */
    uint16_t     mode;
    uint8_t      nlinks;
};

/* Da un struct inode del VFS al nostro involucro. vfs e' il PRIMO membro, quindi
   i due indirizzi coincidono — ma il cast si fa solo dopo aver verificato che
   l'inode sia nostro: potrebbe essere quello dell'innesto, che e' di devfs e non
   ha nessun minode intorno. */
static struct minode *minode_di(struct inode *ino);

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

static int minix_create(struct inode *dir, const char *name,
                            enum inode_type tipo, struct inode **out);
static int minix_write(struct inode *ino, uint32_t off, const void *buf, uint32_t n);
static int minix_read(struct inode *ino, uint32_t off, void *buf, uint32_t n);
static int minix_lookup(struct inode *dir, const char *name,
                        struct inode **out);
static int minix_readdir(struct inode *dir, int idx, char *name,
                         uint32_t *ino_out);

/* write a zero: M11a e' di sola lettura, e "puntatore nullo uguale operazione
   non supportata" e' la convenzione di M8, ereditata due volte. */
static const struct inode_ops ops_minix = {
    .read = minix_read, .lookup = minix_lookup, .readdir = minix_readdir,
    .write = minix_write, .create = minix_create
    /* .write e .create restano a zero fino al Task 3 e al Task 4: e' la
       convenzione di M8, e in M11a e' letteralmente vero — il filesystem e' di
       sola lettura. */
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

/* Lo specchio di blocco_leggi, e vale la stessa regola: il * 2 dei settori sta
   qui e in nessun altro posto. */
static int blocco_scrivi(uint32_t blocco, const void *buf)
{
    if (disco == 0 || disco->write == 0)
        return -1;

    if (disco->write(disco, blocco * SETTORI_PER_BLOCCO, buf,
                     SETTORI_PER_BLOCCO) != SETTORI_PER_BLOCCO)
        return -1;

    return 0;
}

static struct minode *minode_di(struct inode *ino)
{
    if (ino == 0 || ino->ops != &ops_minix)
        return 0;

    return (struct minode *)ino;
}

/* ---- le bitmap --------------------------------------------------------------

   Due allocatori identici su due bitmap diverse, e la sola cosa che li
   distingue e' l'indice. VERIFICATO con od sull'immagine di riferimento:

     imap, blocco 2, primo byte    ff              7 inode in uso piu' il bit 0
     zmap, blocco 3, primi 4 byte  ff ff ff 7f     30 zone in uso piu' il bit 0

   Da cui i due indici, che NON si calcolano allo stesso modo — ed e' la trappola
   numero uno della milestone, perche' la tabella degli inode ne usa un terzo:

     tabella degli inode:  inode i  ->  offset (i - 1) * 32
     bitmap degli inode:   inode i  ->  bit i                 NIENTE meno uno
     bitmap delle zone:    zona z   ->  bit z - firstdatazone + 1

   Il primo byte della imap lo dimostra: con l'indice i-1 sarebbe 7f, non ff.
   E la zmap: firstdatazone e' 7, le zone occupate vanno dalla 7 alla 36, e i bit
   accesi sono da 0 a 30 — quindi la zona 7 e' il bit 1.

   Il BIT 0 e' riservato in entrambe e vale sempre 1. E' il motivo per cui
   l'inode 0 non esiste, e per cui lo zero puo' fare da "non trovato" qui
   sotto. */

#define BLOCCO_IMAP   2u
#define BLOCCO_ZMAP   (2u + sb.s_imap_blocks)
#define BIT_PER_BLOCCO (BLOCK_SIZE * 8)      /* 8192 */

/* Il primo bit a zero, cercando dal bit 1. Zero significa "pieno", e funziona da
   errore proprio perche' il bit 0 non e' mai allocabile.

   Il ciclo e' sui BIT e rilegge il blocco solo quando cambia: cosi' una bitmap
   che occupa piu' di un blocco funziona senza un caso a parte. Sull'immagine di
   prova ne occupa uno solo, quindi un codice che lo assumesse passerebbe tutti i
   test — e si romperebbe alla prima immagine piu' grande. */
static uint32_t bitmap_trova_libero(uint32_t primo_blocco, uint32_t max_bit)
{
    static uint8_t buf[BLOCK_SIZE];
    uint32_t bit, blocco, corrente = 0xFFFFFFFFu, byte;

    for (bit = 1; bit <= max_bit; bit++) {
        blocco = primo_blocco + bit / BIT_PER_BLOCCO;

        if (blocco != corrente) {
            if (blocco_leggi(blocco, buf) < 0)
                return 0;

            corrente = blocco;
        }

        byte = (bit % BIT_PER_BLOCCO) / 8;

        if ((buf[byte] & (1u << (bit % 8))) == 0)
            return bit;
    }

    return 0;
}

/* Legge il blocco che contiene quel bit, lo accende, riscrive il blocco.

   |= e non =, che spegnerebbe i sette bit vicini: sette inode in uso che
   tornano liberi in un colpo, e la prossima allocazione li riusa. */
static int bitmap_accendi(uint32_t primo_blocco, uint32_t bit)
{
    static uint8_t buf[BLOCK_SIZE];
    uint32_t blocco = primo_blocco + bit / BIT_PER_BLOCCO;
    uint32_t byte   = (bit % BIT_PER_BLOCCO) / 8;

    if (blocco_leggi(blocco, buf) < 0)
        return -1;

    buf[byte] |= (uint8_t)(1u << (bit % 8));

    return blocco_scrivi(blocco, buf);
}

/* Spegne un bit, e serve per una cosa sola: ANNULLARE un'allocazione fallita a
   meta'.

   Non e' la free di unlink, che in M11b sta deliberatamente fuori — quella
   toccherebbe le voci di directory e le zone di file vivi, e girerebbe sul
   percorso normale rendendo ambiguo ogni disaccordo di fsck. Questa gira solo
   sul percorso d'errore, e serve a mantenere l'invariante che vale su tutto il
   resto: un bit acceso corrisponde a qualcosa che esiste davvero.

   Senza, un create che fallisce dopo aver allocato lascia un inode che nessuno
   nomina — e fsck lo dice: "Inode 11 not used, marked used in the bitmap". */
static int bitmap_spegni(uint32_t primo_blocco, uint32_t bit)
{
    static uint8_t buf[BLOCK_SIZE];
    uint32_t blocco = primo_blocco + bit / BIT_PER_BLOCCO;
    uint32_t byte   = (bit % BIT_PER_BLOCCO) / 8;

    if (blocco_leggi(blocco, buf) < 0)
        return -1;

    buf[byte] &= (uint8_t)~(1u << (bit % 8));

    return blocco_scrivi(blocco, buf);
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

/* L'inversa, e serve da M11b: creare e riscrivere un inode vogliono i bit di
   tipo nel verso opposto. Tenerle vicine e' l'unico modo di accorgersi se una
   delle due dimentica un caso. */
static uint16_t mode_da_tipo(enum inode_type tipo)
{
    switch (tipo) {
    case INODE_DIR:     return 0040000;
    case INODE_FILE:    return 0100000;
    case INODE_CHARDEV: return 0020000;
    default:            return 0;
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

    /* I due campi che il VFS non ha e che da M11b servono per riscrivere
       l'inode senza inventarsi i permessi e senza perdere il conteggio dei
       link. */
    m->mode   = d->i_mode;
    m->nlinks = d->i_nlinks;

    for (k = 0; k < 9; k++)
        m->zone[k] = d->i_zone[k];

    /* ino si scrive per ULTIMO, perche' e' il marcatore di "slot occupato":
       scrivendolo prima, un ritorno anticipato lascerebbe uno slot preso da un
       inode riempito a meta'. */
    m->vfs.ino = ino;

    return &m->vfs;
}

/* Rimanda sul disco un inode che sta in cache: legge il blocco della tabella,
   sovrascrive i suoi 32 byte, riscrive il blocco.

   E' il gemello di inode_carica e ne condivide l'aritmetica — che per questo sta
   scritta uguale in due posti, ed e' l'unica duplicazione che mi sono permesso:
   estrarla in una funzione che restituisce due valori sarebbe stato piu' lungo
   di cosi'.

   I campi che il VFS non ha — i_uid, i_gid, i_time, e i bit dei permessi dentro
   i_mode — si PRESERVANO rileggendoli, non si azzerano: sono di qualcun altro, e
   un file che cambia proprietario quando il kernel lo tocca e' un sintomo che
   sembra scollegato dalla causa.

   E' la funzione che si dimentica di chiamare, ed e' il bug piu' frequente della
   milestone: tutto funziona finche' il filesystem resta montato, perche' le
   modifiche sono in cache. Al rimontaggio spariscono. */
static int inode_scrivi(struct inode *ino)
{
    static uint8_t buf[BLOCK_SIZE];
    struct minix_inode *d;
    struct minode *m = minode_di(ino);
    uint32_t blocco, offset;
    int k;

    /* minode_di rifiuta gli inode che non sono nostri: ino potrebbe essere
       quello dell'innesto, che appartiene a devfs, e scriverlo qui vorrebbe dire
       metterlo nella tabella degli inode di minix. */
    if (!montato || m == 0 || ino->ino == 0 || ino->ino > sb.s_ninodes)
        return -1;

    blocco = blocco_inodi + (ino->ino - 1) / INODE_PER_BLOCCO;
    offset = ((ino->ino - 1) % INODE_PER_BLOCCO) * INODE_SIZE;

    /* Si RILEGGE prima di scrivere, per due ragioni distinte: preservare i campi
       che non conosciamo — i_uid, i_gid, i_time — e preservare gli altri 31
       inode che stanno nello stesso blocco. La seconda da sola basterebbe.

       Azzerare i_uid e i_time non farebbe protestare fsck, ma su ls -l
       sull'host i file cambierebbero proprietario appena il kernel li tocca:
       un sintomo che sembra scollegato dalla causa. */
    if (blocco_leggi(blocco, buf) < 0)
        return -1;

    d = (struct minix_inode *)(buf + offset);

    /* mode viene dallo slot, permessi inclusi: ricostruirlo dal solo tipo
       perderebbe i bit rwx. */
    d->i_mode   = m->mode;
    d->i_size   = ino->size;
    d->i_nlinks = m->nlinks;

    for (k = 0; k < 9; k++)
        d->i_zone[k] = m->zone[k];

    return blocco_scrivi(blocco, buf);
}

/* Un inode nuovo: bit acceso sulla bitmap, 32 byte scritti sul disco, slot di
   cache riempito.

   L'ORDINE dei passi non e' negoziabile, e ha una sfumatura in piu' di quella
   che avevo scritto nel piano. Prima si cerca lo slot di CACHE, che non ha
   effetti sul disco: se non ce n'e', si esce senza aver toccato niente. Poi il
   bit, poi i byte. Al contrario, un fallimento in mezzo lascerebbe un inode
   marcato in uso e mai scritto — che e' quello che fsck chiama
   "Inode N marked unused, but used for file". */
static struct inode *inode_alloca(enum inode_type tipo)
{
    static uint8_t buf[BLOCK_SIZE];
    struct minix_inode *d;
    struct minode *m = 0;
    uint32_t bit, blocco, offset;
    int i, k;

    if (!montato)
        return 0;

    if (tipo != INODE_FILE && tipo != INODE_DIR)
        return 0;

    for (i = 0; i < MAX_INODES; i++)
        if (cache[i].vfs.ino == 0) {
            m = &cache[i];
            break;
        }

    if (m == 0)
        return 0;               /* cache piena, e non si e' toccato il disco */

    bit = bitmap_trova_libero(BLOCCO_IMAP, sb.s_ninodes);

    if (bit == 0)
        return 0;               /* disco pieno di inode */

    if (bitmap_accendi(BLOCCO_IMAP, bit) < 0)
        return 0;

    /* I 32 byte si AZZERANO prima di riempirli: lo slot sul disco potrebbe
       contenere i resti di un inode di prima, e i campi che inode_scrivi
       preserva — i_uid, i_gid, i_time — sarebbero quelli di qualcun altro. */
    blocco = blocco_inodi + (bit - 1) / INODE_PER_BLOCCO;
    offset = ((bit - 1) % INODE_PER_BLOCCO) * INODE_SIZE;

    if (blocco_leggi(blocco, buf) < 0)
        return 0;

    d = (struct minix_inode *)(buf + offset);
    memset(d, 0, INODE_SIZE);

    if (blocco_scrivi(blocco, buf) < 0)
        return 0;

    /* Lo slot. Le nove zone si azzerano ESPLICITAMENTE: lo slot di cache viene
       riusato, quindi contiene le zone del file di prima — e un file nuovo che
       nasce lungo zero ma con le zone di un altro sovrascrive i suoi dati alla
       prima scrittura. */
    m->vfs.type  = tipo;
    m->vfs.size  = 0;
    m->vfs.major = 0;
    m->vfs.minor = 0;
    m->vfs.ops   = &ops_minix;
    m->vfs.priv  = m->zone;

    for (k = 0; k < 9; k++)
        m->zone[k] = 0;

    /* I permessi: 0755 per una directory, 0644 per un file. Non e' cosmetica —
       una directory senza il bit x non si attraversa, e sull'host "cd" dentro
       quello che il kernel ha creato smetterebbe di funzionare. */
    m->mode = (uint16_t)(mode_da_tipo(tipo) |
                         (tipo == INODE_DIR ? 0755 : 0644));

    /* Una directory nasce a DUE: "." che punta a se stessa, piu' la voce nel
       genitore. Un file a uno. E' il primo numero che fsck controlla. */
    m->nlinks = (tipo == INODE_DIR) ? 2 : 1;

    /* ino per ultimo: e' il marcatore di slot occupato. */
    m->vfs.ino = bit;

    if (inode_scrivi(&m->vfs) < 0) {
        m->vfs.ino = 0;
        return 0;
    }

    return &m->vfs;
}

/* Una zona libera, marcata sulla bitmap e AZZERATA.

   L'azzeramento non e' cortesia: il blocco contiene quello che c'era prima. Se
   diventa un blocco di dati, il file ha in coda i resti di un altro; se diventa
   un blocco INDIRETTO, quei resti vengono letti come puntatori a zone — e il
   danno si sposta su file che non c'entrano. */
static uint32_t zona_alloca(void)
{
    static uint8_t vuoto[BLOCK_SIZE];
    uint32_t bit, zona, max_bit;
    uint32_t i;

    if (!montato || sb.s_nzones <= sb.s_firstdatazone)
        return 0;

    /* Le zone allocabili sono quelle dei dati: dalla firstdatazone in poi. */
    max_bit = (uint32_t)sb.s_nzones - sb.s_firstdatazone;

    bit = bitmap_trova_libero(BLOCCO_ZMAP, max_bit);

    if (bit == 0)
        return 0;

    if (bitmap_accendi(BLOCCO_ZMAP, bit) < 0)
        return 0;

    /* L'inversa dell'indice della zmap. Confondere il numero del BIT con quello
       della ZONA sposta ogni scrittura di sei posizioni su questa immagine —
       cioe' sopra la tabella degli inode. */
    zona = bit + sb.s_firstdatazone - 1;

    for (i = 0; i < BLOCK_SIZE; i++)
        vuoto[i] = 0;

    if (blocco_scrivi(zona, vuoto) < 0)
        return 0;

    return zona;
}

/* Mette la zona z come zona logica n del file. E' l'inversa di zona_di, e ne
   condivide la struttura. */
static int zona_assegna(struct inode *ino, uint32_t n, uint32_t z)
{
    static uint8_t buf[BLOCK_SIZE];
    struct minode *m = minode_di(ino);
    uint16_t *ptr = (uint16_t *)buf;
    uint32_t ind;

    if (m == 0 || z == 0)
        return -1;

    if (n < ZONE_DIRETTE) {
        m->zone[n] = (uint16_t)z;
        return inode_scrivi(ino);
    }

    n -= ZONE_DIRETTE;

    if (n < PTR_PER_BLOCCO) {
        ind = m->zone[ZONA_INDIRETTA];

        /* Il caso interessante: la prima volta che un file supera la settima
           zona, il blocco dei puntatori non esiste e si alloca adesso —
           zona_alloca lo azzera, che qui e' indispensabile perche' i suoi 512
           uint16 sono puntatori e devono partire tutti a "buco".

           Due scritture, e saltare la seconda da' un file che funziona fino al
           rimontaggio: la zona indiretta e' nello slot di cache e non
           sull'inode. */
        if (ind == 0) {
            ind = zona_alloca();

            if (ind == 0)
                return -1;

            m->zone[ZONA_INDIRETTA] = (uint16_t)ind;

            if (inode_scrivi(ino) < 0)
                return -1;
        }

        if (blocco_leggi(ind, buf) < 0)
            return -1;

        /* n - 7 e non n: le prime sette zone stanno nell'inode, e scrivendo in
           posizione n si duplicherebbero nell'indiretto perdendo le ultime
           sette del blocco. Il -7 e' gia' stato applicato sopra. */
        ptr[n] = (uint16_t)z;

        return blocco_scrivi(ind, buf);
    }

    /* Il doppio indiretto si RIFIUTA in scrittura, con un errore esplicito.
       Sull'immagine da 256 KB non ci si arriva nemmeno — servirebbero file oltre
       519 KB — quindi sarebbe codice mai eseguito. L'asimmetria con la lettura,
       che lo gestisce, e' voluta: leggere un'immagine fatta da altri e' un caso
       reale, scriverne una cosi' no. */
    return -1;
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

/* Aggiunge una voce da 16 byte a una directory.

   Riusa un posto libero se c'e', altrimenti cresce. Il ramo del riuso in M11b
   non si esercita mai — senza unlink non esistono voci cancellate — e va scritto
   lo stesso: il giorno che arriva la cancellazione, e' la differenza fra una
   directory che cresce all'infinito e una che no. */
static int dirent_inserisci(struct inode *dir, const char *nome, uint32_t ino)
{
    struct minix_dirent voce;
    uint32_t off;
    size_t len, i;

    if (dir == 0 || nome == 0 || dir->type != INODE_DIR)
        return -1;

    len = strlen(nome);

    /* Si RIFIUTA invece di troncare: troncare farebbe collidere due file
       diversi, che e' l'errore del nome di dispositivo in M8. */
    if (len == 0 || len > MINIX_NAME_MAX)
        return -1;

    /* Cerca un posto libero. Se il ciclo arriva in fondo, off vale dir->size e
       la scrittura piu' sotto fa crescere la directory — e' minix_write ad
       aggiornare size e a riscrivere l'inode, quindi qui non serve farlo. */
    for (off = 0; off < dir->size; off += DIRENT_SIZE) {
        if (minix_read(dir, off, &voce, DIRENT_SIZE) != DIRENT_SIZE)
            return -1;

        if (voce.ino == 0)
            break;
    }

    voce.ino = (uint16_t)ino;

    /* I 14 byte si riempiono di zeri e poi si copia: un nome lungo esattamente
       14 resta NON terminato, ed e' corretto — e' il formato. */
    for (i = 0; i < MINIX_NAME_MAX; i++)
        voce.name[i] = (i < len) ? nome[i] : '\0';

    if (minix_write(dir, off, &voce, DIRENT_SIZE) != DIRENT_SIZE)
        return -1;

    return 0;
}

/* Mette insieme le quattro operazioni precedenti. */
static int minix_create(struct inode *dir, const char *name,
                        enum inode_type tipo, struct inode **out)
{
    struct inode *nuovo;
    struct inode *esiste = 0;
    struct minode *md;

    if (dir == 0 || name == 0 || out == 0)
        return -1;

    /* dir potrebbe essere l'inode dell'innesto, che appartiene a devfs: creare
       dentro /dev deve fallire, e il controllo e' che l'inode sia nostro. */
    if (dir->type != INODE_DIR || minode_di(dir) == 0)
        return -1;

    if (tipo != INODE_FILE && tipo != INODE_DIR)
        return -1;

    /* IL nome si valida PRIMA di allocare, e la riga esiste per un guasto vero:
       senza, un nome di 15 caratteri passava di qui, faceva allocare un inode, e
       poi dirent_inserisci lo rifiutava — lasciando sul disco un inode marcato
       in uso che nessuno nomina. Tutti i controlli passavano, e solo fsck lo
       vedeva:  "Inode 11 not used, marked used in the bitmap".

       La regola generale: non si tocca il disco finche' non si sa che
       l'operazione puo' riuscire. */
    if (strlen(name) == 0 || strlen(name) > MINIX_NAME_MAX)
        return -1;

    /* Il nome che esiste gia' fa fallire. Sta al chiamante decidere cosa
       farne: vfs_open con O_CREAT apre quello che c'e', vfs_mkdir fallisce.

       Si usa lookup, cioe' codice di M11a che gia' funziona: riscrivere la
       scansione qui sarebbe una seconda implementazione dello stesso confronto
       di nomi, e le due divergerebbero. */
    if (minix_lookup(dir, name, &esiste) == 0)
        return -1;

    nuovo = inode_alloca(tipo);

    if (nuovo == 0)
        return -1;

    /* Se da qui in poi qualcosa fallisce — tipicamente il disco pieno mentre la
       directory cresce — l'inode va RESTITUITO. La validazione preventiva copre
       il caso prevedibile; questo copre gli altri. */
    if (dirent_inserisci(dir, name, nuovo->ino) < 0) {
        bitmap_spegni(BLOCCO_IMAP, nuovo->ino);
        nuovo->ino = 0;             /* libera anche lo slot di cache */
        return -1;
    }

    if (tipo == INODE_DIR) {
        /* I tre numeri che fsck controlla, ed e' qui che si sbaglia mkdir:
           la directory nuova ha nlinks 2 — glielo ha dato inode_alloca — e le
           sue due voci sono "." e ".."; il genitore ne guadagna uno per via
           del "..". Dimenticare l'ultimo da' un filesystem che si monta, un ls
           che funziona, e un fsck che dice "Inode 1 has 3 links, counted 4". */
        if (dirent_inserisci(nuovo, ".", nuovo->ino) < 0)
            return -1;

        if (dirent_inserisci(nuovo, "..", dir->ino) < 0)
            return -1;

        md = minode_di(dir);

        if (md == 0)
            return -1;

        md->nlinks++;

        if (inode_scrivi(dir) < 0)
            return -1;
    }

    /* *out solo in caso di successo, la convenzione di lookup. */
    *out = nuovo;
    return 0;
}

/* Il gemello di minix_read, e conviene leggerli affiancati: il ciclo e'
   identico, e cambiano tre cose.

   1.  NON c'e' il troncamento su size. E' la riga piu' importante di read e la
       piu' sbagliata da portare qui: con lei, scrivere oltre la fine non
       farebbe niente e un file non crescerebbe mai.
   2.  Il blocco si LEGGE prima di scriverlo, anche quando si sovrascrive: la
       granularita' del disco e' il blocco, e scrivendo 4 byte in mezzo bisogna
       restituire anche gli altri 1020. Senza la lettura, si azzerano.
   3.  Dove read trova una zona a zero e restituisce zeri, write la alloca.

   Il punto 3 non c'e' ancora: vuole zona_alloca e zona_assegna, cioe' le bitmap
   del Task 2. Fino ad allora il ciclo si ferma sul buco e riporta quanti byte e'
   riuscito a scrivere — che e' il comportamento corretto di "disco pieno",
   raggiunto per un motivo diverso. Quando arrivera' l'allocatore, quella riga
   resta come caso di fallimento. */
static int minix_write(struct inode *ino, uint32_t off, const void *buf,
                       uint32_t n)
{
    static uint8_t blocco[BLOCK_SIZE];
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t fatti = 0;
    uint32_t z, dentro, quanti;
    uint32_t fine;

    if (ino == 0 || buf == 0)
        return -1;

    if (ino->type == INODE_DIR && ino->ops != &ops_minix)
        return -1;

    /* Scrivere zero byte non e' un errore e non deve toccare niente — in
       particolare non deve aggiornare size. */
    if (n == 0)
        return 0;

    while (fatti < n) {
        dentro = off % BLOCK_SIZE;
        quanti = BLOCK_SIZE - dentro;

        if (quanti > n - fatti)
            quanti = n - fatti;

        z = zona_di(ino, off / BLOCK_SIZE);

        if (z == 0) {
            /* Dove read restituisce zeri, write alloca. E' l'unica differenza
               di sostanza fra le due funzioni.

               La zona si ASSEGNA prima di scriverci: se ci si scrivesse e basta,
               la write seguente per lo stesso posto ne allocherebbe un'altra, e
               la prima resterebbe occupata senza appartenere a nessuno — una
               perdita che vede solo fsck.

               Uscire dal ciclo qui non e' un errore: significa disco pieno, e
               si riportano i byte che sono passati. Dire di non aver scritto
               niente sarebbe una bugia, perche' il file resta cresciuto a
               meta'. */
            z = zona_alloca();

            if (z == 0)
                break;

            if (zona_assegna(ino, off / BLOCK_SIZE, z) < 0)
                break;
        }

        if (blocco_leggi(z, blocco) < 0)
            break;

        memcpy(blocco + dentro, src + fatti, quanti);

        if (blocco_scrivi(z, blocco) < 0)
            break;

        off   += quanti;
        fatti += quanti;
    }

    /* Zero byte passati e' un errore solo se ne erano stati chiesti: qui n > 0
       per il controllo in cima, quindi non aver scritto niente significa che il
       primo blocco non c'era o non si e' potuto scrivere. */
    if (fatti == 0)
        return -1;

    /* La size si aggiorna SOLO se il file e' cresciuto. Scrivere in mezzo non lo
       accorcia, e una size che diminuisce farebbe sparire la coda.

       E si riscrive l'inode: senza, il file ha il contenuto giusto e vale zero
       byte al prossimo mount — cat non stampa niente e la diagnosi va a finire
       su read, che e' innocente. */
    fine = off;

    if (fine > ino->size) {
        ino->size = fine;

        if (inode_scrivi(ino) < 0)
            return -1;
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
//TODO: sostituiamola con memcpy o creiamo strcpy
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

    /* Zero, non -1: "le voci sono finite" e "la domanda non aveva senso" sono
       due cose diverse, e un ciclo che si fermasse su entrambe sembrerebbe
       funzionare fino al giorno in cui readdir fallisce davvero. */
    return 0;
}
