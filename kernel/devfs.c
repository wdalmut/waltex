#include "devfs.h"
#include "vfs.h"
#include "device.h"
#include "memory.h"

static struct inode ino_root;                 /* "/"        ino 1 */
static struct inode ino_dev;                  /* "/dev"     ino 2 */
static struct inode ino_devices[MAX_DEVICES]; /* uno per dispositivo, ino 3+ */
static int ready = 0;                            /* devfs_init e' stata chiamata? */

static int root_lookup(struct inode *dir, const char *name, struct inode **out);
static int root_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out);
static int dev_lookup(struct inode *dir, const char *name, struct inode **out);
static int dev_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out);
static int chardev_read(struct inode *ino, uint32_t off, void *buf, uint32_t n);
static int chardev_write(struct inode *ino, uint32_t off, const void *buf, uint32_t n);

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

static const struct inode_ops ops_chardev = {
    .read = chardev_read, .write = chardev_write
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

static int dev_lookup(struct inode *dir, const char *name, struct inode **out)
{
    (void)dir;

    for (int i=0; i<MAX_DEVICES; i++) {
        struct device *d = device_at(i);

        if (d != 0) {
            if (strcmp(d->name, name) == 0) {
                *out = &ino_devices[i];
                return 0;
            }
        }

    }
    return -1;
}

static int dev_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out)
{
    (void)dir;

    if (idx < 0 || idx >= device_count()) {
        return 0;
    }

    struct device *d = device_at(idx);

    memcpy(name, d->name, VFS_NAME_MAX);
    name[VFS_NAME_MAX] = '\0';

    *ino_out = ino_devices[idx].ino;

    return 1;
}

static int chardev_read(struct inode *ino, uint32_t off, void *buf, uint32_t n)
{
    (void)off;

    struct device *d = (struct device *)ino->priv;

    if (d == 0 || d->read == 0) {
        return -1;
    }

    return d->read(d, buf, n);
}

static int chardev_write(struct inode *ino, uint32_t off, const void *buf, uint32_t n)
{
    (void)off;
    
    struct device *d = (struct device *)ino->priv;

    if (d == 0 || d->write == 0) {
        return -1;
    }

    return d->write(d, buf, n);
}

void devfs_init(void)
{
    int i=0;

    for (i=0; i<device_count(); i++) {
            struct device *d = device_at(i);
            ino_devices[i].ino = 3 + i;

            ino_devices[i].ops = &ops_chardev;
            ino_devices[i].type = INODE_CHARDEV;

            ino_devices[i].priv = d;
            ino_devices[i].major = d->major;
            ino_devices[i].minor = d->minor;
            ino_devices[i].size = 0;
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