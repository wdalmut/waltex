#include "vfs.h"
#include "task.h"
#include "memory.h"
#include "irq.h"

static struct inode  *root;
static struct file   files[MAX_OPEN_FILES];
static int           fds[MAX_TASKS][TASK_FDS];

static struct file *get_file_from_fd(int fd)
{
    if (fd < 0 || fd >= TASK_FDS) {
        return 0;
    }

    int t = task_current();

    if (t < 0 || t >= MAX_TASKS) {
            return 0;
    }

    int f = fds[t][fd];

    if (f == -1) {
        return 0;
    }

    return &(files[f]);
}

void vfs_init(struct inode *r)
{
    root = r;

    for (uint8_t i=0; i<MAX_OPEN_FILES; i++) {
        files[i].inode = 0;
    }

    for (uint8_t i=0; i<MAX_TASKS; i++) {
        for (uint8_t k=0; k<TASK_FDS; k++) {
            fds[i][k] = -1;
        }
    }
}

/* Traduce una stringa in un inode. Non alloca, non apre, non legge: traduce.

   L'ultimo componente NON e' un caso speciale. Si fa la stessa lookup su ognuno,
   e cio' che torna dall'ultima e' la risposta — che sia un file, una directory o
   un dispositivo, a questa funzione non interessa. Cambia solo COME si trova la
   fine del componente, non cosa se ne fa. */
int vfs_resolve(const char *path, struct inode **out)
{
    struct inode *current = root;
    struct inode *prossimo = 0;
    const char *p;
    char nome[VFS_NAME_MAX + 1];    /* +1 per il terminatore: un nome di
                                       VFS_NAME_MAX caratteri vuole un byte in
                                       piu' dove metterlo */
    size_t lunghezza, i;

    /* Solo path assoluti. Senza una directory corrente non c'e' niente rispetto
       a cui risolvere un path relativo — la cwd arriva in M14 con i processi.

       Questo controllo copre anche il path vuoto: '\0' non e' '/'. */
    if (path[0] != '/')
        return -1;

    if (strlen(path) >= VFS_PATH_MAX)
        return -1;

    p = path + 1;       /* la barra iniziale si salta UNA volta, qui */

    for (;;) {
        const char *fine;

        /* Il salto delle barre sta in cima a OGNI giro, e da questa scelta
           cadono tutti e tre i casi che si dimenticano:

             "/"     dopo il salto siamo al terminatore  →  esce, ritorna root
             "//a"   il giro le salta entrambe           →  come "/a"
             "/d/"   dopo "d" resta una barra, la salta  →  esce con la directory

           Scritto cosi' non serve nessun caso speciale. */
        while (*p == '/')
            ++p;

        if (*p == '\0')
            break;

        /* La fine del componente: la prossima barra, oppure il terminatore se
           barre non ce ne sono piu'. E' l'unica differenza fra un componente
           intermedio e l'ultimo. */
        fine = strchr(p, '/');
        lunghezza = (fine != 0) ? (size_t)(fine - p) : strlen(p);

        /* Si RIFIUTA, non si tronca: troncare farebbe risolvere due nomi
           diversi allo stesso file, che e' l'errore del nome di dispositivo in
           M8. E si controlla prima di copiare, non dopo. */
        if (lunghezza > VFS_NAME_MAX)
            return -1;

        for (i = 0; i < lunghezza; i++)
            nome[i] = p[i];
        nome[lunghezza] = '\0';

        /* Per cercare qualcosa DENTRO current, current deve essere una
           directory. Il controllo e' su current PRIMA della lookup, quindi vale
           per i componenti intermedi e non impone niente sul risultato
           dell'ultimo: e' la semantica di Unix, e la ragione per cui "/a/b"
           fallisce quando "a" e' un file. */
        if (current->type != INODE_DIR)
            return -1;

        /* Puntatore nullo uguale "operazione non supportata": la convenzione di
           M8, ereditata. Anche una directory potrebbe non avere lookup. */
        if (current->ops == 0 || current->ops->lookup == 0)
            return -1;

        if (current->ops->lookup(current, nome, &prossimo) < 0)
            return -1;

        /* current si sposta solo DOPO che lookup ha risposto 0. Passandole
           &current direttamente, una lookup fallita lascerebbe current valido e
           il ciclo continuerebbe come se avesse trovato qualcosa. */
        current = prossimo;

        /* Avanzare di lunghezza porta sulla barra, o sul terminatore. Le barre
           le mangia il salto in cima al giro seguente — cosi' non serve
           distinguere il caso "fine == 0". */
        p += lunghezza;
    }

    /* *out si scrive SOLO qui, cioe' solo in caso di successo: su ogni uscita di
       errore il chiamante tiene il valore che aveva. Stessa regola di
       shell_parse_hex. */
    *out = current;
    return 0;
}

/* ---- i due allocatori ------------------------------------------------------
   Una nota prima: solo UNO dei due ha bisogno di una sezione critica, e la
   differenza e' strutturale.

   files[] e' globale: due task che cercassero uno slot libero insieme
   troverebbero lo stesso. Serve proteggere.

   fds[t] e' la riga del task corrente, e l'unico che ci scrive e' il task
   corrente — perche' task_current() restituisce chi sta girando. Un solo
   scrittore per riga, quindi niente da proteggere: e' la stessa proprieta' del
   ring buffer di M5, dove head lo scrive solo il produttore e tail solo il
   consumatore.

   La struttura compra la correttezza per una delle due tabelle e non per
   l'altra, ed e' il motivo per cui vale la pena guardarle separatamente invece
   di mettere un cli intorno a tutto. */

/* Prende uno slot libero e lo OCCUPA nello stesso passo. Ritorna l'indice, o -1
   se la tabella e' piena.

   Riceve l'inode invece di essere una file_alloc(void), e la ragione e' che il
   marcatore di "libero" E' il campo inode: per occupare lo slot bisogna
   scriverci l'inode dentro, quindi la funzione ha bisogno di averlo. Trovare lo
   slot e uscire per riempirlo dopo lascerebbe la finestra aperta esattamente
   dove la sezione critica la voleva chiudere. */
static int file_alloc(struct inode *ino, int flags)
{
    uint32_t irqf;
    int i, slot = -1;

    irqf = irq_save();

    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (files[i].inode == 0) {
            files[i].inode = ino;      /* da questa riga lo slot non e' piu' libero */
            files[i].off   = 0;        /* azzerare conta: lo slot puo' venire da un
                                          file chiuso prima, con la sua posizione */
            files[i].flags = flags;
            slot = i;
            break;
        }
    }

    irq_restore(irqf);

    return slot;
}

/* Il descrittore piu' BASSO libero del task corrente, o -1 se sono finiti.

   Il piu' basso e non il primo mai usato: dopo una close il numero torna
   disponibile, e in M15 i primi tre descrittori di un processo saranno 0, 1 e 2
   — stdin, stdout, stderr — proprio perche' nessun altro li ha presi. */
static int fd_alloc(int slot)
{
    int t = task_current();
    int i;

    for (i = 0; i < TASK_FDS; i++) {
        if (fds[t][i] == -1) {
            fds[t][i] = slot;
            return i;
        }
    }

    return -1;
}

/* Spezza un path assoluto nel suo GENITORE e nell'ultimo componente.
   "/etc/motd" diventa "/etc" e "motd"; "/f.txt" diventa "/" e "f.txt".

   Serve perche' creare vuole il genitore, e vfs_resolve restituisce solo la fine
   del cammino. Si copia e si tronca invece di aggiungere un parametro a
   vfs_resolve: quella funzione ha settantacinque test addosso e non c'e' ragione
   di toccarla.

   Ritorna 0, oppure -1 se il path non e' assoluto, se e' troppo lungo, se
   l'ultimo componente e' vuoto — "/etc/" — o se non ci sta in VFS_NAME_MAX.

   Il caso da non sbagliare e' "/f.txt": dopo il troncamento il genitore sarebbe
   la stringa vuota, che vfs_resolve rifiuta perche' non comincia con '/'. Il
   genitore giusto e' "/". */
static int spezza_path(const char *path, char *genitore, char *ultimo)
{
    size_t len, i, taglio;

    if (path == 0 || path[0] != '/')
        return -1;

    len = strlen(path);

    if (len == 0 || len >= VFS_PATH_MAX)
        return -1;

    /* L'ultima barra: da li' in poi c'e' il nome. */
    taglio = 0;
    for (i = 0; i < len; i++)
        if (path[i] == '/')
            taglio = i;

    if (len - taglio - 1 == 0 || len - taglio - 1 > VFS_NAME_MAX)
        return -1;

    for (i = 0; i < len - taglio - 1; i++)
        ultimo[i] = path[taglio + 1 + i];

    ultimo[len - taglio - 1] = '\0';

    /* taglio == 0 significa che l'unica barra e' quella iniziale, cioe' un file
       nella radice. */
    if (taglio == 0) {
        genitore[0] = '/';
        genitore[1] = '\0';
        return 0;
    }

    for (i = 0; i < taglio; i++)
        genitore[i] = path[i];

    genitore[taglio] = '\0';
    return 0;
}

/* La parte comune di vfs_open con O_CREAT e di vfs_mkdir: risolvi il genitore,
   chiedigli di creare.

   NON apre niente ed e' voluto — mkdir non lascia nulla di aperto, e facendola
   passare da vfs_open dopo trentadue mkdir il sistema non aprirebbe piu'. */
static int crea_ultimo(const char *path, enum inode_type tipo,
                       struct inode **out)
{
    char genitore[VFS_PATH_MAX];
    char ultimo[VFS_NAME_MAX + 1];
    struct inode *dir;

    if (spezza_path(path, genitore, ultimo) < 0)
        return -1;

    /* Solo l'ULTIMO componente si crea: "mkdir -p" non esiste, e un componente
       intermedio mancante resta un errore. */
    if (vfs_resolve(genitore, &dir) < 0)
        return -1;

    if (dir->type != INODE_DIR)
        return -1;

    /* Puntatore nullo uguale "non supportata", la convenzione di M8: su devfs
       create e' zero, quindi mkdir /dev/x fallisce da se'. */
    if (dir->ops == 0 || dir->ops->create == 0)
        return -1;

    return dir->ops->create(dir, ultimo, tipo, out);
}

int vfs_open(const char *path, int flags)
{
    struct inode *ino;
    int slot, fd;

    /* ino e' una LOCALE: quattro byte sullo stack, non un inode. vfs_resolve non
       crea niente, trova un inode che esiste gia' — statico in devfs — e ci
       scrive il suo indirizzo. Nessuna allocazione, e infatti non ce n'e' modo. */
    if (vfs_resolve(path, &ino) < 0) {
        /* & e non ==: O_CREAT e' un BIT, e flags vale spesso O_WRONLY|O_CREAT,
           cioe' 0101. Un == non lo riconoscerebbe. */
        if ((flags & O_CREAT) == 0)
            return -1;

        if (crea_ultimo(path, INODE_FILE, &ino) < 0)
            return -1;
    }

    /* Aprire una directory DEVE riuscire: vfs_readdir prende un fd, e quell'fd
       viene da qui. E' read su una directory che va vietata, non open. */

    slot = file_alloc(ino, flags);
    if (slot < 0)
        return -1;

    fd = fd_alloc(slot);
    if (fd < 0) {
        /* Il punto piu' facile da sbagliare di tutta la funzione: qui lo slot
           globale e' GIA' occupato, e se si ritornasse -1 senza liberarlo
           resterebbe occupato da nessuno per sempre.

           Il sintomo sarebbe "open fallisce e non capisco perche'" dopo
           trentadue aperture fallite, cioe' molto lontano dalla causa. Ogni
           allocazione che precede un possibile fallimento vuole il suo disfare. */
        files[slot].inode = 0;
        return -1;
    }

    /* Si ritorna il DESCRITTORE, non lo slot. Sembrano equivalenti — sono due
       numeri piccoli — ma restituire lo slot romperebbe l'isolamento: due task
       vedrebbero gli stessi numeri, e in M15 un processo utente riceverebbe un
       indice dentro una tabella del kernel. */
    return fd;
}

int vfs_read(int fd, void *buf, uint32_t n)
{
    int rbytes = 0;
    struct file *f;

    f = get_file_from_fd(fd);
    if (f == 0) {
        return -1;
    }

    if (f->flags == O_WRONLY) {
        return -1;
    }

    if (f->inode->ops->read == 0) {
        return -1;
    }

    if (f->inode->type == INODE_DIR) {
        return -1;
    }
    
    rbytes = f->inode->ops->read(f->inode, f->off, buf, n);
    if ((rbytes) < 0) {
        return -1;
    }
    f->off += rbytes;

    return rbytes;
}

int vfs_write(int fd, const void *buf, uint32_t n)
{
    int rbytes = 0;
    struct file *f;

    f = get_file_from_fd(fd);
    if (f == 0) {
        return -1;
    }

    if (f->flags == O_RDONLY) {
        return -1;
    }

    if (f->inode->ops->write == 0) {
        return -1;
    }

    if (f->inode->type == INODE_DIR) {
        return -1;
    }
    
    rbytes = f->inode->ops->write(f->inode, f->off, buf, n);
    if ((rbytes) < 0) {
        return -1;
    }
    f->off += rbytes;

    return rbytes;
}

int vfs_close(int fd)
{
    if (fd < 0) {
        return -1;
    }

    int t = task_current();

    if (fds[t][fd] != -1) {
        fds[t][fd] = -1;
        return 0;
    }

    return -1;
}

int vfs_lseek(int fd, int32_t off, int whence)
{
    int coff = -1;
    struct file *f = get_file_from_fd(fd);
    
    if (f == 0) {
        return -1;
    }

    if (whence == SEEK_SET && off < 0) {
        return -1;
    }

    switch (whence) {
        case SEEK_SET:
            if (off >= 0) {
                coff = f->off = off;
            }
            break;
        case SEEK_CUR:
            int32_t o = (int32_t)(f->off + off);
            if (o >= 0) {
                f->off += off;
                coff = f->off;
            }
            break;
        case SEEK_END:
            f->off = f->inode->size + off;
            coff = f->off;
            break;
        default:
            coff = -1;
            break;
    }

    return coff;
}

int vfs_readdir(int fd, int idx, char *name, uint32_t *ino_out)
{
    struct file *f = get_file_from_fd(fd);

    if (f == 0) {
        return -1;
    }

    if (f->inode->ops->readdir == 0) {
        return -1;
    }

    if (f->inode->type != INODE_DIR) {
        return -1;
    }

    int a = f->inode->ops->readdir(f->inode, idx, name, ino_out);

    return a;
}


int vfs_mkdir(const char *path)
{
    struct inode *nuovo;

    /* Se esiste gia' — file o directory — si fallisce. E' la differenza con
       O_CREAT, che invece apre quello che trova: la stessa create sotto, due
       politiche diverse sopra. */
    if (vfs_resolve(path, &nuovo) == 0)
        return -1;

    return crea_ultimo(path, INODE_DIR, &nuovo);
}
