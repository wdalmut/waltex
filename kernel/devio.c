#include "devio.h"
#include "memory.h"
#include "types.h"
#include "dev.h"
#include "chardev.h"
#include "blockdev.h"
#include "vfs.h"

/* --- la vista a FILE di un dispositivo a caratteri ----------------------------

   Queste due stavano in devfs.c fino a M11e. Sono qui perche' e' qui che
   devio_fill_inode le INSTALLA, e l'invariante che le rende sicure vuole un
   custode solo:

       ino->ops == &ops_chardev   ⟹   ino->priv e' un struct chardev *

   Il cast dentro non e' un controllo: e' la conseguenza del controllo su kind che
   devio_fill_inode fa una volta sola. Chiedersi "e se priv non fosse un chardev?"
   equivale a chiedersi "e se qualcuno avesse installato la tabella sbagliata?", e
   la tabella e' static: l'unico che puo' farlo sta venti righe piu' sotto.

   Il puntatore a funzione E' il tag di tipo, gia' risolto — la' dove
   l'alternativa sarebbe tenere kind nell'inode e rifare lo switch a ogni read. */

static int chr_read(struct inode *ino, uint32_t off, void *buf, uint32_t n)
{
    struct chardev *c = (struct chardev *)ino->priv;

    /* off si IGNORA, e non e' una semplificazione: un dispositivo a caratteri non
       ha una posizione, quindi una lseek su /dev/kbd non significa niente e va
       ignorata invece che rifiutata. E' l'opposto di blk_read, dove off e' tutto. */
    (void)off;

    /* "Non supportata" rende -1 e NON 0, e questa riga vale i tre bug di M9b.

       La convenzione del puntatore nullo descrive il DISPOSITIVO — console non si
       legge — ma il valore consegnato a chi ha chiesto di leggere dev'essere -1:
       uno zero significherebbe "adesso non c'e' niente", cioe' un'attesa invece
       che un rifiuto, e cat continuerebbe a girare per sempre su un dispositivo
       che non sa leggere. */
    if (c == 0 || c->read == 0) {
        return -1;
    }

    /* c come primo argomento: e' cio' che permette a un driver di iscrivere due
       dispositivi con la STESSA funzione e sapere quale sta servendo. */
    return c->read(c, buf, n);
}

static int chr_write(struct inode *ino, uint32_t off, const void *buf, uint32_t n)
{
    struct chardev *c = (struct chardev *)ino->priv;

    (void)off;

    if (c == 0 || c->write == 0) {
        return -1;
    }

    return c->write(c, buf, n);
}
/* --- la vista a BYTE di un dispositivo a BLOCCHI ------------------------------

   E' l'unica logica vera di M11e: tradurre un offset in byte in un numero di
   settore piu' uno scostamento, e ricucire le fette.

   Il bounce buffer e' LOCALE, 512 byte sullo stack, e non static. Statico
   costerebbe meno memoria e farebbe mescolare due letture prelazionate a meta':
   fra il "leggo il settore" e il "copio la fetta" ci sta un tick del timer. E' la
   lezione di procfs, dove il buffer di generazione e' locale per la stessa
   ragione.

   Il costo va dichiarato: gli stack dei task sono 4096 byte, quindi questo buffer
   e' UN OTTAVO dello stack, dentro la catena shell_cat -> vfs_read -> blk_read.
   E' il candidato numero uno a spostarsi quando M12 porta kmalloc.

   b->read viene chiamata in UN PUNTO SOLO, e sempre con count == 1. Non e'
   pigrizia: e' la riga in cui la buffer cache si infilera', e diventera'
   cache_get(b, lba). Con una via rapida per le letture allineate — che leggerebbe
   direttamente nel buffer del chiamante saltando il bounce — ci sarebbero DUE punti
   da sostituire, e quella via non si eserciterebbe comunque mai: cat legge a
   blocchi di 64 byte, quindi sarebbe codice mai eseguito. E' la stessa ragione per
   cui il doppio indiretto si rifiuta in scrittura in minixfs. */

static int blk_read(struct inode *ino, uint32_t off, void *buf, uint32_t n)
{
    struct blockdev *b = (struct blockdev *)ino->priv;
    uint8_t         *dst = (uint8_t *)buf;
    uint8_t          bounce[SECTOR_SIZE];
    uint32_t         fatti = 0;

    /* La capacita' del dispositivo si guarda PRIMA di tutto: e' una sua proprieta',
       non una proprieta' della richiesta. Un disco che non si legge rende -1 anche
       a n == 0 e anche oltre la fine, perche' la domanda non e' "quanto hai letto"
       ma "sai leggere". */
    if (b == 0 || b->read == 0) {
        return -1;
    }

    /* EOF VERO, e qui lo zero e' ONESTO: un disco ha una dimensione, quindi la fine
       esiste. Su un dispositivo a caratteri lo stesso zero significherebbe "adesso
       non c'e' niente" — ed e' la ragione per cui shell_cat guarda il TIPO
       dell'inode invece del valore di ritorno per decidere quando smettere. */
    if (off >= ino->size) {
        return 0;
    }

    /* Il clamp si fa per SOTTRAZIONE.

       "off + n > ino->size" GIRA: con off vicino a 2^32 la somma torna piccola, e il
       controllo crederebbe di essere dentro. Sottrarre e' lecito perche' il
       controllo sopra garantisce off < ino->size, quindi la differenza non va sotto
       zero. E' la regola di M10 — «lba + count > nsectors e' il controllo
       SBAGLIATO» — applicata ai byte invece che ai settori. */
    if (n > ino->size - off) {
        n = ino->size - off;
    }

    while (fatti < n) {
        uint32_t lba   = (off + fatti) / SECTOR_SIZE;
        uint32_t skip  = (off + fatti) % SECTOR_SIZE;
        uint32_t chunk = SECTOR_SIZE - skip;

        if (chunk > n - fatti) {
            chunk = n - fatti;
        }

        /* Il confronto e' con 1 e non con > 0: read rende SETTORI, e un
           trasferimento parziale di settore non esiste. E' la trappola di M10, che
           in shell.c si e' presentata tre volte. */
        if (b->read(b, lba, bounce, 1) != 1) {
            /* I due rami dell'errore, e la distinzione e' la convenzione di read
               portata dentro: chi ha ricevuto 300 byte buoni deve saperlo, e dirgli
               -1 glieli fa buttare. Chi non ne ha ricevuto nessuno non ha nulla da
               salvare, e uno zero gli direbbe EOF — cioe' una bugia. */
            return (fatti > 0) ? (int)fatti : -1;
        }

        memcpy(dst + fatti, bounce + skip, chunk);
        fatti += chunk;
    }

    return (int)fatti;
}

static int blk_write(struct inode *ino, uint32_t off, const void *buf, uint32_t n)
{
    struct blockdev *b = (struct blockdev *)ino->priv;
    const uint8_t   *src = (const uint8_t *)buf;
    uint8_t          bounce[SECTOR_SIZE];
    uint32_t         fatti = 0;

    /* write nulla e' un disco READ-ONLY, che e' uno stato legittimo — e
       blockdev_register lo accetta di proposito. Il rifiuto e' -1 e non 0, per la
       stessa ragione di chr_write: uno zero direbbe "non c'era posto". */
    if (b == 0 || b->write == 0) {
        return -1;
    }

    /* UN DISCO NON CRESCE, e qui sta la differenza con un file regolare su minix:
       la' scrivere oltre la fine lo allunga, qui la dimensione e' quella del
       supporto. Quindi off >= size non e' "estendi", e' "finito". */
    if (off >= ino->size) {
        return 0;
    }

    if (n > ino->size - off) {
        n = ino->size - off;
    }

    while (fatti < n) {
        uint32_t lba   = (off + fatti) / SECTOR_SIZE;
        uint32_t skip  = (off + fatti) % SECTOR_SIZE;
        uint32_t chunk = SECTOR_SIZE - skip;

        if (chunk > n - fatti) {
            chunk = n - fatti;
        }

        /* READ-MODIFY-WRITE, ed e' LA trappola di questa funzione.

           Una scrittura che non copre il settore intero deve prima LEGGERLO:
           scrivere 10 byte all'offset 5 vuole leggere il settore, ritoccarne 10 byte
           e riscriverlo. Senza la lettura, i 502 byte intorno finirebbero con
           quello che c'era nel bounce buffer — e non e' spazzatura casuale, sono
           dati veri di un altro settore letto un giro prima, quindi hanno l'aria di
           essere giusti. E' la zona non azzerata di M11b.

           "chunk != SECTOR_SIZE" copre entrambi i casi parziali: con skip > 0 il
           chunk e' per costruzione minore di un settore, e con skip == 0 lo e'
           quando ne resta da scrivere meno di uno. */
        if (chunk != SECTOR_SIZE) {
            if (b->read == 0 || b->read(b, lba, bounce, 1) != 1) {
                return (fatti > 0) ? (int)fatti : -1;
            }
        }

        memcpy(bounce + skip, src + fatti, chunk);

        if (b->write(b, lba, bounce, 1) != 1) {
            return (fatti > 0) ? (int)fatti : -1;
        }

        fatti += chunk;
    }

    return (int)fatti;
}

static const struct inode_ops ops_blockdev = {
    .read = blk_read, .write = blk_write
};

/* Inizializzatori designati, e non e' stile: inode_ops ha cinque campi e qui se
   ne usano due. Con la forma posizionale il compilatore segnalerebbe "missing
   initializer for field 'create'" a ogni build — un avviso permanente e giusto,
   cioe' un avviso che si smette di leggere. Cosi' invece i campi assenti restano
   a zero DICHIARANDOLO, che e' la convenzione di M8: puntatore nullo uguale
   operazione non supportata.

   lookup, readdir e create restano a zero perche' un dispositivo non e' una
   directory e non si crea niente dentro di lui. E' il motivo per cui
   mkdir /dev/x fallisce da se', senza un caso a parte da nessuna parte.

   static: nessuno da fuori puo' installarla, quindi nessuno da fuori puo' rompere
   l'invariante scritto sopra. */
static const struct inode_ops ops_chardev = {
    .read = chr_read, .write = chr_write
};

int devio_caps(const struct dev_entry *e)
{
    int mask = 0;

    if (e == 0 || e->impl == 0) {
        return 0;
    }

    /* I due puntatori si dichiarano DENTRO il ramo che li usa, e non e' stile:
       dichiarati fuori, il ramo a blocchi puo' leggere quello a caratteri — che e'
       il bug che c'era qui, e il compilatore lo diceva due volte ("blockdev set
       but not used" e "chardev may be used uninitialized"). Un (void) che zittisce
       l'avviso non tocca il guasto: lo rende solo invisibile.

       Dichiarandoli nel ramo, quel bug non e' piu' esprimibile. */
    if (e->kind == DEV_BLOCK) {
        const struct blockdev *b = (const struct blockdev *)e->impl;

        mask |= b->read  ? DEVIO_CAN_READ  : 0;
        mask |= b->write ? DEVIO_CAN_WRITE : 0;
    } else if (e->kind == DEV_CHAR) {
        const struct chardev *c = (const struct chardev *)e->impl;

        mask |= c->read  ? DEVIO_CAN_READ  : 0;
        mask |= c->write ? DEVIO_CAN_WRITE : 0;
    }

    return mask;
}

int chardev_register (const char *name, uint16_t major, uint16_t minor,
                      struct chardev  *c)
{
    struct dev_entry e;
    int i;

    if (name == 0 || c == 0) {
        return -1;
    }

    if (c->read == 0 && c->write == 0) {
        return -1;
    }

    memset(&e, 0, sizeof(e));

    for (i = 0; i < DEV_NAME_MAX && name[i] != '\0'; i++) {
        e.name[i] = name[i];
    }

    e.kind  = DEV_CHAR;
    e.major = major;
    e.minor = minor;
    e.impl  = c;

    return dev_register(&e);
}

int blockdev_register(const char *name, uint16_t major, uint16_t minor,
                      struct blockdev *b)
{
    struct dev_entry e;
    int i;

    if (name == 0 || b == 0) {
        return -1;
    }

    /* ASIMMETRICO rispetto a chardev_register, e di proposito: la' e' "almeno uno
       dei due", perche' console non si legge e kbd non si scrive. Qui basta read
       nulla per rifiutare — un disco da cui non si legge non ha senso, mentre un
       disco su cui non si scrive e' un read-only perfettamente legittimo.

       E' il rifiuto che dev_register non puo' fare, perche' non conosce i metodi.
       Perderlo la' e' stato un guadagno: un controllo condiviso distingue solo
       "almeno uno", e non potrebbe esprimere questa differenza. */
    if (b->read == 0) {
        return -1;
    }

    memset(&e, 0, sizeof(e));

    for (i = 0; i < DEV_NAME_MAX && name[i] != '\0'; i++) {
        e.name[i] = name[i];
    }

    e.kind  = DEV_BLOCK;
    e.major = major;
    e.minor = minor;
    e.impl  = b;

    return dev_register(&e);
}

/* La voce che si chiama cosi', oppure 0.

   i e' un INT e non un uint8_t, ed e' la riga che sembrava funzionare: con un
   uint8_t il -1 di dev_lookup_index diventa 255, quindi "i < 0" e' sempre falso —
   gcc lo dice, "comparison is always false due to limited range" — e il caso di
   nome assente veniva preso solo di rimbalzo da 255 >= DEV_MAX. Con DEV_MAX a 256
   avrebbe smesso di funzionare. E' la stessa specie della guardia morta di strpos
   che questa milestone chiude.

   Il controllo su i >= DEV_MAX non serve piu': dev_get lo fa da se', e rende 0
   sia sui negativi sia oltre dev_count(). Le due funzioni si incastrano per
   costruzione, ed e' meglio di due controlli che devono restare d'accordo. */
static const struct dev_entry *_dev(const char *name)
{
    return dev_get(dev_lookup_index(name));
}

struct chardev  *dev_chardev (const char *name)
{
    const struct dev_entry *e;

    e = _dev(name);

    if (e == 0) {
        return 0;
    }

    if (e->kind != DEV_CHAR) {
        return 0;
    }

    return (struct chardev *)e->impl;
}

struct blockdev *dev_blockdev(const char *name)
{
    const struct dev_entry *e;

    e = _dev(name);

    if (e == 0) {
        return 0;
    }

    if (e->kind != DEV_BLOCK) {
        return 0;
    }

    return (struct blockdev *)e->impl;
}

int devio_fill_inode(const struct dev_entry *e, struct inode *in)
{
    /* impl nullo si rifiuta, e non e' paranoia: il ramo a blocchi lo DEREFERENZIA
       per leggere nsectors, quindi senza questo controllo una voce malformata non
       darebbe -1 ma una lettura all'indirizzo 0.

       I due wrapper lo escludono gia', ma dev_register no — e non puo', perche' e'
       agnostico e non sa cosa impl debba essere. Chi dereferenzia difende. E' la
       stessa scelta che devio_caps fa gia' nella sua prima riga: le due funzioni
       hanno lo stesso rischio, quindi devono avere la stessa guardia. */
    if (e == 0 || in == 0 || e->impl == 0) {
        return -1;
    }

    if (e->kind == DEV_CHAR) {
        in->type = INODE_CHARDEV;
        in->ops  = &ops_chardev;

        /* Un dispositivo a caratteri non ha dimensione, e lo zero qui e' corretto
           invece che ignoto: vfs_read non consulta size, e chi legge sa che la fine
           non esiste perche' guarda il TIPO. E' anche cio' che fa Linux — ls -l su
           un tty mostra 0 byte. */
        in->size = 0;
    } else if (e->kind == DEV_BLOCK) {
        const struct blockdev *b = (const struct blockdev *)e->impl;

        in->type = INODE_BLOCKDEV;
        in->ops  = &ops_blockdev;

        /* La dimensione E' la capacita' del supporto, ed e' cio' che trasforma un
           disco in un file con una fine. Senza, blk_read non saprebbe quando
           rendere zero e cat non si fermerebbe mai.

           ATTENZIONE: il prodotto gira in uint32_t a 4 GiB, cioe' 8388608 settori,
           e LBA28 arriva a 128 GiB — quindi e' raggiungibile in principio. Sui
           nostri dischi da 2048 e 512 settori non si vede. Sistemarlo vuole una
           dimensione a 64 bit in struct inode, che e' un problema di struct stat in
           M14: e' annotato fra i debiti. */
        in->size = b->nsectors * SECTOR_SIZE;
    } else {
        /* DEV_NONE e qualunque valore fuori dai due: non sappiamo interpretare
           impl, quindi non fabbrichiamo un inode. E su -1 *in NON viene toccato,
           che e' la convenzione di lookup e di create. */
        return -1;
    }

    in->priv  = e->impl;
    in->major = e->major;
    in->minor = e->minor;

    return 0;
}