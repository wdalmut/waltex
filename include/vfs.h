#ifndef WALTEX_VFS_H
#define WALTEX_VFS_H

#include "types.h"

/* Capacita' fisse, come tutto il resto: l'allocatore arriva in M12. */
#define VFS_PATH_MAX    64
#define VFS_NAME_MAX    14      /* la lunghezza di minix v1, che arriva in M11 */
#define MAX_OPEN_FILES  32
#define TASK_FDS         8

/* I valori sono quelli POSIX, verificati dagli header dell'host e non
   ricordati. Non ci obbliga nessuno, costa zero, e in M14 questi diventano gli
   argomenti di syscall che una libc compilata per Linux passa. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

enum inode_type { INODE_NONE = 0, INODE_FILE, INODE_DIR, INODE_CHARDEV };

struct inode;

/* Le quattro operazioni che un filesystem concreto deve saper fare.

   Il VFS non sa quale filesystem sta parlando: chiama attraverso questi
   puntatori, ed e' tutto il polimorfismo che serve. Un puntatore NULLO significa
   "questo inode non fa quella cosa" — la convenzione di M8, ereditata.

   read e write ricevono un OFFSET esplicito e non lo tengono. E' la decisione
   piu' importante di questa interfaccia: la posizione vive nella tabella dei
   file aperti, non nell'inode, quindi due descrittori sullo stesso file hanno
   due posizioni e un solo inode. Se la posizione stesse qui, due open
   indipendenti se la ruberebbero a vicenda.

   Tutte ritornano un intero non negativo in caso di successo e -1 in caso di
   errore. readdir ne ha tre: 1 se la voce esiste, 0 se l'indice e' oltre
   l'ultima, -1 se la domanda non aveva senso. */
struct inode_ops {
    int (*read   )(struct inode *ino, uint32_t off, void *buf, uint32_t n);
    int (*write  )(struct inode *ino, uint32_t off, const void *buf, uint32_t n);
    int (*lookup )(struct inode *dir, const char *name, struct inode **out);
    int (*readdir)(struct inode *dir, int idx, char *name, uint32_t *ino_out);
};

/* L'identita' di un file. Non ha un refcount: in M9b gli inode sono statici
   dentro devfs.c e nessuno viene mai liberato, quindi non c'e' niente da
   contare. Il campo arrivera' in M11, dove gli inode vengono dal disco e la
   cache e' forzata. */
struct inode {
    uint32_t         ino;            /* identita' dentro il suo filesystem  */
    enum inode_type  type;
    uint32_t         size;           /* significativo per INODE_FILE        */
    uint16_t         major, minor;   /* validi se INODE_CHARDEV             */
    const struct inode_ops *ops;
    void            *priv;           /* il filesystem concreto ci mette quel
                                        che vuole: in M9b il struct device   */
};

/* Un file aperto. Se struct inode e' IL FILE, struct file e' l'ATTO DI AVERLO
   APERTO — e la distinzione e' l'intera ragione per cui sono due strutture:

     struct inode   una per file, sempre. Sa che tipo e', quanto e' grande, e
                    come si legge. La crea il filesystem, e vive quanto il file.
     struct file    una per open(). Sa quale inode, e DOVE SI E' ARRIVATI.
                    La crea vfs_open, e vive dalla open alla close.

   La relazione e' molti a uno:

     struct file A (off = 0) ──┐
                               ├──> struct inode di /a  (size 4, ops)
     struct file B (off = 2) ──┘

   Due open sullo stesso path danno due struct file — due posizioni indipendenti
   — e UN SOLO struct inode, perche' il file e' uno. Per questo qui c'e' un
   puntatore e non una copia: questa struttura non possiede l'inode, lo
   riferisce.

   Il campo si chiama "inode" e non "ino" deliberatamente: "ino" dentro struct
   inode e' il NUMERO dell'inode, un uint32_t, e chiamare cosi' anche il
   puntatore rende impossibile capire quale dei due si sta leggendo.

   inode == 0 significa "slot libero": un puntatore nullo non e' un file aperto
   valido, quindi il campo che serve comunque fa da marcatore. Stesso
   ragionamento del registro di M8 senza flag per slot, e del ring buffer di M5
   senza contatore degli elementi. */
struct file {
    struct inode *inode;
    uint32_t      off;
    int           flags;
};

/* La radice arriva da FUORI, e non e' un dettaglio: e' la sola ragione per cui
   questa milestone si prova interamente sull'host. In M9b la passa devfs, nei
   test un albero finto. Lo stesso espediente del sink di eco di lineedit.

   Svuota anche le due tabelle. Attenzione al valore di "libero": nella tabella
   dei file aperti e' lo zero, nella tabella dei descrittori e' il -1, perche' li'
   lo zero e' un fd valido — sara' stdin in M15. */
void vfs_init(struct inode *root);

/* Risolve un path assoluto. 0 e *out se ci riesce, -1 altrimenti, e in caso di
   errore *out NON viene toccato.

   Solo path assoluti: senza una directory corrente non c'e' niente rispetto a
   cui risolvere un path relativo, e la cwd arriva in M14 con i processi.

   Il suo chiamante vero e' uno: vfs_open, di cui e' la PRIMA META' — open e'
   resolve piu' due allocazioni.

   Sta nell'header per la testabilita', e la ragione e' la stessa di task_slot in
   M6a: open fa due lavori che falliscono in modi diversi — trovare il file, e
   trovare posto nelle tabelle — e provare la risoluzione attraverso open
   significherebbe che il primo FAIL non dice quale dei due e' rotto.

   In M9b servira' anche a "ls", ma solo se ls mostra il TIPO di ogni voce:
   readdir da' il nome e il numero di inode, non l'inode. Per i soli nomi bastano
   vfs_open e vfs_readdir. */
int vfs_resolve(const char *path, struct inode **out);

/* Le cinque che in M14 diventano syscall senza cambiare firma:
   3 read, 4 write, 5 open, 6 close, 19 lseek. */

/* Il descrittore piu' basso libero, o -1. flags viene memorizzato perche' read e
   write lo consultino: non ci sono permessi da far valere. Aprire una directory
   riesce, e serve a vfs_readdir. O_CREAT non e' supportato — il filesystem di
   M9b e' di sola lettura per costruzione, e la creazione arriva in M11. */
int vfs_open(const char *path, int flags);

/* Quanti byte ha letto DAVVERO, da 0 a n, oppure -1.

   Zero non e' un errore e non e' fine del file: e' la convenzione di M8. Su un
   file regolare significa che la posizione ha raggiunto size; su un dispositivo
   a caratteri significa "adesso non c'e' niente". Chi legge distingue i due casi
   guardando il tipo dell'inode, non il valore di ritorno.

   La posizione avanza di quanto e' stato letto, non di n. */
int vfs_read(int fd, void *buf, uint32_t n);

int vfs_write(int fd, const void *buf, uint32_t n);

/* Libera DUE cose: la casella del descrittore e lo slot globale. Liberarne una
   sola fa sembrare tutto corretto finche' la tabella dei file aperti non si
   esaurisce in silenzio dopo 32 aperture. */
int vfs_close(int fd);

/* La posizione nuova, oppure -1. off e' firmato perche' SEEK_CUR possa tornare
   indietro; una posizione negativa non esiste e va rifiutata senza muovere
   niente. Oltre la fine e' permesso, come su Unix. */
int vfs_lseek(int fd, int32_t off, int whence);

/* 1 se la voce esiste, 0 se idx e' oltre l'ultima, -1 se l'fd non e' aperto, non
   e' una directory, o non ha readdir.

   Tre valori e non due: collassando 0 e -1, chi enumera non distingue "directory
   finita" da "questo fd non e' una directory", e un ciclo che si ferma su
   entrambi sembra funzionare finche' non gli passi un file.

   name e' del chiamante e vuole VFS_NAME_MAX + 1 byte. */
int vfs_readdir(int fd, int idx, char *name, uint32_t *ino_out);

#endif
