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
    if (e == 0 || in == 0) {
        return -1;
    }

    if (e->kind == DEV_BLOCK) {
        return -1;
    } else if (e->kind == DEV_CHAR) {
        in->type = INODE_CHARDEV;
        in->ops  = &ops_chardev;
        in->size = 0;
    } else {
        return -1;
    }

    in->priv  = e->impl;
    in->major = e->major;
    in->minor = e->minor;

    return 0;
}