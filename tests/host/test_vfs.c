/* Test del nucleo del VFS, col gcc dell'host e senza QEMU.

   E' la prima milestone del progetto interamente fuori dalla VM, e la ragione e'
   una scelta di interfaccia: vfs_init RICEVE l'inode della radice. Qui gliene
   passiamo uno finto, e il VFS non sa che e' finto — che e' precisamente la
   proprieta' che stiamo verificando.

   L'albero di prova:

     /        dir      lookup/readdir: a, d, c, e
     /a       file     "ciao" (4 byte), legge ma non scrive
     /d       dir      lookup/readdir: b
     /d/b     file     "xy" (2 byte)
     /c       chardev  legge al massimo 3 byte per chiamata, e scrive
     /e       file     ops tutte a zero: ne' legge ne' scrive

   Ogni nodo e' uno struct inode statico con le sue inode_ops. /c da' MENO byte di
   quanti gliene si chiedano, deliberatamente: e' l'unico modo di verificare che
   la posizione avanzi di quanto e' stato letto davvero e non di n. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "vfs.h"
#include "task.h"

static int failures;

static void check(const char *name, int ok)
{
    printf(ok ? "ok   -- %s\n" : "FAIL -- %s\n", name);
    if (!ok)
        failures++;
}

/* Confronto scritto in casa: il VFS non dipende da memory.c, e non vogliamo che
   il test introduca una dipendenza che il codice sotto test non ha. */
static int same(const char *a, const char *b)
{
    int i;

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (a[i] != b[i])
            return 0;
    }

    return a[i] == b[i];
}

/* ---- lo stub che il VFS si aspetta ---------------------------------------- */

/* vfs.c indicizza la tabella dei descrittori con task_current(). E' la sua
   unica dipendenza esterna, e qui la pilotiamo per verificare che i descrittori
   siano davvero PER TASK. */
static int task_finto;

int task_current(void)
{
    return task_finto;
}

/* ---- l'albero finto ------------------------------------------------------- */

static struct inode ino_root, ino_a, ino_d, ino_db, ino_c, ino_e;

struct voce { const char *name; struct inode *ino; };

struct dati_dir { struct voce *v; int n; };

static struct voce voci_root[] = {
    { "a", &ino_a }, { "d", &ino_d }, { "c", &ino_c }, { "e", &ino_e }
};
static struct voce voci_d[] = { { "b", &ino_db } };

static struct dati_dir dati_root = { voci_root, 4 };
static struct dati_dir dati_d    = { voci_d, 1 };

/* ---- M11c: un secondo albero, da montare -------------------------------------

   Una radice con dentro "m", piu' TRE radici spoglie che servono a un controllo
   solo: riempire la tabella. Sono deliberatamente minime — quello che si prova
   qui e' il MECCANISMO del mount, non un filesystem.

   Quattro radici montabili e non due, perche' MAX_MOUNTS vale 4 e il controllo
   sulla tabella piena vuole quattro mount che riescano DAVVERO. Riusandone due
   a giro si costruirebbe mroot -> mroot2 e mroot2 -> mroot, cioe' un ciclo, e il
   quinto mount verrebbe rifiutato da "punto == radice" invece che dalla tabella
   piena: il controllo passerebbe per la ragione sbagliata. */
static struct inode ino_mroot, ino_m, ino_mroot2, ino_mroot3, ino_mroot4;

static struct voce voci_mroot[]  = { { "m", &ino_m } };
static struct voce voci_mroot2[] = { { "m2", &ino_m } };

static struct dati_dir dati_mroot  = { voci_mroot,  1 };
static struct dati_dir dati_mroot2 = { voci_mroot2, 1 };

static int dir_lookup(struct inode *dir, const char *name, struct inode **out)
{
    struct dati_dir *dd = (struct dati_dir *)dir->priv;
    int i;

    for (i = 0; i < dd->n; i++) {
        if (same(dd->v[i].name, name)) {
            *out = dd->v[i].ino;
            return 0;
        }
    }

    return -1;
}

static int dir_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out)
{
    struct dati_dir *dd = (struct dati_dir *)dir->priv;
    int i;

    if (idx < 0 || idx >= dd->n)
        return 0;

    for (i = 0; i < VFS_NAME_MAX && dd->v[idx].name[i] != '\0'; i++)
        name[i] = dd->v[idx].name[i];
    name[i] = '\0';

    *ino_out = dd->v[idx].ino->ino;
    return 1;
}

/* Un file: il contenuto sta in priv, la lunghezza in size. */
static int file_read(struct inode *ino, uint32_t off, void *buf, uint32_t n)
{
    const char *src = (const char *)ino->priv;
    char *dst = (char *)buf;
    uint32_t i;

    if (off >= ino->size)
        return 0;

    if (n > ino->size - off)
        n = ino->size - off;

    for (i = 0; i < n; i++)
        dst[i] = src[off + i];

    return (int)n;
}

/* Il dispositivo a caratteri. Non guarda l'offset — un dispositivo non ha
   posizione — e ritorna al massimo tre byte per chiamata. */
#define C_MAX 3

static char c_scritto[64];
static int  c_scritto_len;

static int c_read(struct inode *ino, uint32_t off, void *buf, uint32_t n)
{
    char *dst = (char *)buf;
    uint32_t i;

    (void)ino;
    (void)off;

    if (n > C_MAX)
        n = C_MAX;

    for (i = 0; i < n; i++)
        dst[i] = 'z';

    return (int)n;
}

static int c_write(struct inode *ino, uint32_t off, const void *buf, uint32_t n)
{
    const char *src = (const char *)buf;
    uint32_t i;

    (void)ino;
    (void)off;

    for (i = 0; i < n && c_scritto_len < (int)sizeof(c_scritto); i++)
        c_scritto[c_scritto_len++] = src[i];

    return (int)i;
}

/* Inizializzatori designati: da M11b inode_ops ha cinque campi, e la forma
   posizionale farebbe protestare -Wextra su ogni tabella incompleta. I campi
   assenti restano a zero DICHIARANDOLO — la convenzione di M8.

   ops_muto e' l'inode /e dell'albero finto: tutte e cinque a zero, cioe' un
   inode che non sa fare niente. Serve a provare che il VFS controlla prima di
   chiamare invece di fidarsi. */
static const struct inode_ops ops_dir  = {
    .lookup = dir_lookup, .readdir = dir_readdir
};
static const struct inode_ops ops_file = { .read = file_read };
static const struct inode_ops ops_c    = { .read = c_read, .write = c_write };
static const struct inode_ops ops_muto = { 0 };

/* Rimonta l'albero e riparte da un VFS vuoto. Ogni gruppo di controlli la
   chiama, cosi' nessuno eredita lo stato del gruppo precedente. */
static void albero(void)
{
    ino_root.ino = 1; ino_root.type = INODE_DIR;
    ino_root.size = 0; ino_root.ops = &ops_dir; ino_root.priv = &dati_root;

    ino_a.ino = 2; ino_a.type = INODE_FILE;
    ino_a.size = 4; ino_a.ops = &ops_file; ino_a.priv = (void *)"ciao";

    ino_d.ino = 3; ino_d.type = INODE_DIR;
    ino_d.size = 0; ino_d.ops = &ops_dir; ino_d.priv = &dati_d;

    ino_db.ino = 4; ino_db.type = INODE_FILE;
    ino_db.size = 2; ino_db.ops = &ops_file; ino_db.priv = (void *)"xy";

    ino_c.ino = 5; ino_c.type = INODE_CHARDEV;
    ino_c.size = 0; ino_c.ops = &ops_c; ino_c.priv = 0;
    ino_c.major = 13; ino_c.minor = 64;

    ino_e.ino = 6; ino_e.type = INODE_FILE;
    ino_e.size = 9; ino_e.ops = &ops_muto; ino_e.priv = 0;

    /* M11c. I numeri partono da 100 apposta: cosi' un controllo che passasse
       confrontando ino invece del puntatore si distinguerebbe a colpo d'occhio
       da uno che passa per la ragione giusta. */
    ino_mroot.ino = 100; ino_mroot.type = INODE_DIR;
    ino_mroot.size = 0; ino_mroot.ops = &ops_dir; ino_mroot.priv = &dati_mroot;

    ino_m.ino = 101; ino_m.type = INODE_FILE;
    ino_m.size = 3; ino_m.ops = &ops_file; ino_m.priv = (void *)"mmm";

    ino_mroot2.ino = 102; ino_mroot2.type = INODE_DIR;
    ino_mroot2.size = 0; ino_mroot2.ops = &ops_dir;
    ino_mroot2.priv = &dati_mroot2;

    ino_mroot3.ino = 103; ino_mroot3.type = INODE_DIR;
    ino_mroot3.size = 0; ino_mroot3.ops = &ops_dir;
    ino_mroot3.priv = &dati_mroot2;

    ino_mroot4.ino = 104; ino_mroot4.type = INODE_DIR;
    ino_mroot4.size = 0; ino_mroot4.ops = &ops_dir;
    ino_mroot4.priv = &dati_mroot2;

    c_scritto_len = 0;
    task_finto = 0;

    vfs_init(&ino_root);
}

/* ---- risoluzione dei path ------------------------------------------------- */

static void test_resolve(void)
{
    struct inode *p;
    char lungo[VFS_PATH_MAX + 20];
    char nome_lungo[VFS_NAME_MAX + 12];
    int i;

    albero();

    /* vfs_init non deve TOCCARE la radice: il numero di inode e' l'identita' del
       file dentro il suo filesystem, la decide chi costruisce il filesystem, e il
       VFS lo legge soltanto. Azzerarlo lo corrompe, e readdir riporta i numeri di
       inode — quindi in M9b "ls -i /" mostrerebbe 0. */
    check("vfs_init non modifica il numero di inode della radice",
          ino_root.ino == 1);

    check("\"/\" da' la radice",
          vfs_resolve("/", &p) == 0 && p == &ino_root);
    check("\"/a\" da' il file",
          vfs_resolve("/a", &p) == 0 && p == &ino_a);
    check("\"/d\" da' la directory",
          vfs_resolve("/d", &p) == 0 && p == &ino_d);
    check("\"/d/b\" da' il file annidato",
          vfs_resolve("/d/b", &p) == 0 && p == &ino_db);

    /* Senza directory corrente non c'e' niente rispetto a cui risolvere un path
       relativo: e' un errore, non un tentativo. La cwd arriva in M14. */
    check("un path relativo e' rifiutato", vfs_resolve("a", &p) == -1);
    check("il path vuoto e' rifiutato", vfs_resolve("", &p) == -1);

    check("un nome che non esiste e' rifiutato",
          vfs_resolve("/nonesiste", &p) == -1);

    /* /a e' un file: non ha lookup, e ci si arriva. Serve controllare il tipo
       PRIMA di chiamare, e il puntatore prima di dereferenziarlo. */
    check("attraversare un file e' rifiutato", vfs_resolve("/a/b", &p) == -1);
    check("un nome che non esiste dentro una sottodirectory e' rifiutato",
          vfs_resolve("/d/nonesiste", &p) == -1);

    /* I tre casi delle barre, che cadono tutti da una sola scelta: saltarle in
       cima al ciclo, e uscire se dopo averle saltate si e' al terminatore. */
    check("\"//a\" risolve come \"/a\"",
          vfs_resolve("//a", &p) == 0 && p == &ino_a);
    check("\"/d/\" da' la directory, non un errore",
          vfs_resolve("/d/", &p) == 0 && p == &ino_d);
    check("\"///\" da' la radice",
          vfs_resolve("///", &p) == 0 && p == &ino_root);

    /* Un path troppo lungo va rifiutato prima di copiarne pezzi in giro. */
    lungo[0] = '/';
    for (i = 1; i < (int)sizeof(lungo) - 1; i++)
        lungo[i] = 'x';
    lungo[sizeof(lungo) - 1] = '\0';
    check("un path piu' lungo di VFS_PATH_MAX e' rifiutato",
          vfs_resolve(lungo, &p) == -1);

    /* Un componente troppo lungo va RIFIUTATO, non troncato: troncare farebbe
       risolvere due nomi diversi allo stesso file, che e' l'errore del nome di
       dispositivo in M8. */
    nome_lungo[0] = '/';
    for (i = 1; i < (int)sizeof(nome_lungo) - 1; i++)
        nome_lungo[i] = 'a';
    nome_lungo[sizeof(nome_lungo) - 1] = '\0';
    check("un componente piu' lungo di VFS_NAME_MAX e' rifiutato",
          vfs_resolve(nome_lungo, &p) == -1);

    /* In caso di errore *out non va toccato: chi chiama deve poter tenere il
       valore che aveva. Stessa regola di shell_parse_hex. */
    p = &ino_a;
    vfs_resolve("/nonesiste", &p);
    check("in caso di errore *out non viene toccato", p == &ino_a);
}

/* ---- apertura e chiusura -------------------------------------------------- */

static void test_open_close(void)
{
    int fd, fd2, fd3, i, aperti;

    albero();

    fd = vfs_open("/a", O_RDONLY);
    check("open su un file da' un fd non negativo", fd >= 0);

    /* Il piu' basso libero, non il primo mai usato: e' cio' che in M15 fara' si'
       che i primi tre descrittori di un processo siano 0, 1 e 2. */
    check("il primo fd e' il piu' basso disponibile", fd == 0);

    fd2 = vfs_open("/a", O_RDONLY);
    check("due open danno due fd distinti", fd2 >= 0 && fd2 != fd);

    vfs_close(fd);
    fd3 = vfs_open("/a", O_RDONLY);
    check("un fd chiuso viene riusato", fd3 == fd);

    check("open su un path inesistente da' -1",
          vfs_open("/nonesiste", O_RDONLY) == -1);

    /* Aprire una directory deve riuscire: vfs_readdir prende un fd, e quell'fd
       viene da qui. */
    check("open su una directory riesce", vfs_open("/d", O_RDONLY) >= 0);

    /* Prima tabella a esaurirsi: i descrittori del task. */
    albero();
    aperti = 0;
    for (i = 0; i < TASK_FDS; i++) {
        if (vfs_open("/a", O_RDONLY) >= 0)
            aperti++;
    }
    check("entrano TASK_FDS descrittori per task", aperti == TASK_FDS);
    check("il descrittore in piu' e' rifiutato",
          vfs_open("/a", O_RDONLY) == -1);

    /* Seconda tabella, e ha una causa diversa: gli slot globali. Si arriva a
       esaurirla usando piu' task, perche' un task solo finisce prima i suoi
       descrittori. MAX_OPEN_FILES / TASK_FDS task pieni la riempiono. */
    albero();
    aperti = 0;
    for (task_finto = 0; task_finto < MAX_OPEN_FILES / TASK_FDS; task_finto++) {
        for (i = 0; i < TASK_FDS; i++) {
            if (vfs_open("/a", O_RDONLY) >= 0)
                aperti++;
        }
    }
    check("entrano MAX_OPEN_FILES file aperti in totale",
          aperti == MAX_OPEN_FILES);
    check("il file aperto in piu' e' rifiutato",
          vfs_open("/a", O_RDONLY) == -1);

    albero();
    fd = vfs_open("/a", O_RDONLY);
    check("close di un fd valido da' 0", vfs_close(fd) == 0);
    check("close due volte: la seconda da' -1", vfs_close(fd) == -1);
    check("close di un fd mai aperto da' -1", vfs_close(5) == -1);
    check("close di un fd negativo da' -1", vfs_close(-1) == -1);
}

/* ---- lettura, scrittura, posizione ---------------------------------------- */

static void test_read_write_seek(void)
{
    int fd, r;
    char buf[16];

    albero();

    fd = vfs_open("/a", O_RDONLY);
    r = vfs_read(fd, buf, 4);
    buf[r > 0 ? r : 0] = '\0';
    check("read di 4 byte da /a da' 4", r == 4);
    check("il contenuto letto e' \"ciao\"", same(buf, "ciao"));

    /* La posizione ha raggiunto size: zero significa "niente altro", e su un
       file regolare chi legge lo interpreta come fine. */
    check("la read successiva da' 0", vfs_read(fd, buf, 4) == 0);

    albero();
    fd = vfs_open("/a", O_RDONLY);
    check("read di 2 byte da' 2", vfs_read(fd, buf, 2) == 2);
    r = vfs_read(fd, buf, 2);
    buf[r > 0 ? r : 0] = '\0';
    check("la read successiva da' i 2 rimanenti",
          r == 2 && same(buf, "ao"));

    check("read con n = 0 da' 0", vfs_read(fd, buf, 0) == 0);

    check("read su un fd non aperto da' -1", vfs_read(7, buf, 4) == -1);
    check("read su un fd negativo da' -1", vfs_read(-1, buf, 4) == -1);

    /* /e e' un file le cui ops hanno read a zero: nullo significa "operazione
       non supportata", la convenzione di M8. */
    albero();
    fd = vfs_open("/e", O_RDONLY);
    check("read su un inode senza read nelle ops da' -1",
          vfs_read(fd, buf, 4) == -1);

    /* Leggere una directory da' -1: il suo contenuto si guarda con readdir,
       perche' sono strutture del filesystem e in M11 saranno voci minix. */
    albero();
    fd = vfs_open("/d", O_RDONLY);
    check("read su una directory da' -1", vfs_read(fd, buf, 4) == -1);

    /* I flag si memorizzano perche' read e write li consultino. */
    albero();
    fd = vfs_open("/c", O_WRONLY);
    check("read su un fd aperto O_WRONLY da' -1", vfs_read(fd, buf, 4) == -1);

    albero();
    fd = vfs_open("/c", O_RDONLY);
    check("write su un fd aperto O_RDONLY da' -1",
          vfs_write(fd, "x", 1) == -1);

    albero();
    fd = vfs_open("/c", O_RDWR);
    check("write su /c da' il numero di byte", vfs_write(fd, "ab", 2) == 2);
    check("i byte sono arrivati al dispositivo",
          c_scritto_len == 2 && c_scritto[0] == 'a' && c_scritto[1] == 'b');

    albero();
    fd = vfs_open("/a", O_RDWR);
    check("write su un inode senza write nelle ops da' -1",
          vfs_write(fd, "x", 1) == -1);

    /* Il controllo che il dispositivo finto esiste per fare: chiediamo 8 byte,
       lui ne da' 3, e la posizione deve avanzare di 3 e non di 8. Avanzare di n
       salterebbe cinque byte che nessuno ha visto. */
    albero();
    fd = vfs_open("/c", O_RDONLY);
    check("una read parziale da' quanti byte ci sono davvero",
          vfs_read(fd, buf, 8) == C_MAX);
    check("la posizione avanza di quanto e' stato letto, non di n",
          vfs_lseek(fd, 0, SEEK_CUR) == C_MAX);

    /* lseek nelle sue tre forme. */
    albero();
    fd = vfs_open("/a", O_RDONLY);
    vfs_read(fd, buf, 4);
    check("lseek(0, SEEK_SET) riporta a zero",
          vfs_lseek(fd, 0, SEEK_SET) == 0);
    r = vfs_read(fd, buf, 4);
    buf[r > 0 ? r : 0] = '\0';
    check("dopo lseek si rilegge dall'inizio", r == 4 && same(buf, "ciao"));

    check("lseek(2, SEEK_SET) da' 2", vfs_lseek(fd, 2, SEEK_SET) == 2);
    r = vfs_read(fd, buf, 4);
    buf[r > 0 ? r : 0] = '\0';
    check("la read dopo lseek parte da li'", r == 2 && same(buf, "ao"));

    vfs_lseek(fd, 0, SEEK_SET);
    vfs_lseek(fd, 1, SEEK_CUR);
    check("lseek con SEEK_CUR e' relativo alla posizione",
          vfs_lseek(fd, 1, SEEK_CUR) == 2);

    /* All'indietro, ed e' TUTTA la ragione per cui off e' firmato. Rifiutare
       ogni off negativo renderebbe SEEK_CUR monodirezionale, e passerebbe
       comunque il controllo qui sotto sulla posizione negativa — che verifica
       un caso diverso: il RISULTATO fuori intervallo, non il passo. */
    vfs_lseek(fd, 3, SEEK_SET);
    check("lseek con SEEK_CUR sa tornare indietro",
          vfs_lseek(fd, -1, SEEK_CUR) == 2);
    r = vfs_read(fd, buf, 4);
    buf[r > 0 ? r : 0] = '\0';
    check("e la read riparte dalla posizione arretrata",
          r == 2 && same(buf, "ao"));

    check("lseek(0, SEEK_END) da' size", vfs_lseek(fd, 0, SEEK_END) == 4);

    /* Anche SEEK_END con off negativo: "gli ultimi due byte". */
    check("lseek(-2, SEEK_END) da' size - 2",
          vfs_lseek(fd, -2, SEEK_END) == 2);

    check("un whence sconosciuto da' -1", vfs_lseek(fd, 0, 99) == -1);

    /* Una posizione negativa non esiste: va rifiutata SENZA muovere niente,
       perche' meta' di uno spostamento e' peggio di nessuno spostamento. */
    vfs_lseek(fd, 2, SEEK_SET);
    check("una posizione negativa e' rifiutata",
          vfs_lseek(fd, -10, SEEK_CUR) == -1);
    check("dopo un lseek rifiutato la posizione non e' cambiata",
          vfs_lseek(fd, 0, SEEK_CUR) == 2);

    /* Oltre la fine e' permesso, come su Unix: qui non si puo' scrivere, quindi
       l'effetto e' solo che la read successiva da' 0. */
    check("lseek oltre la fine e' permesso",
          vfs_lseek(fd, 100, SEEK_SET) == 100);
    check("la read oltre la fine da' 0", vfs_read(fd, buf, 4) == 0);

    check("lseek su un fd non aperto da' -1", vfs_lseek(7, 0, SEEK_SET) == -1);
}

/* ---- indipendenza dei livelli --------------------------------------------- */

static void test_livelli(void)
{
    int fd1, fd2;
    char buf[8];

    albero();

    /* Due open sullo stesso path: STESSO inode, posizioni indipendenti. E' il
       controllo che prende la posizione tenuta nell'inode invece che nel file
       aperto — il guasto che si manifesterebbe mesi dopo, con due processi che
       leggono lo stesso file. */
    fd1 = vfs_open("/a", O_RDONLY);
    fd2 = vfs_open("/a", O_RDONLY);

    vfs_read(fd1, buf, 2);

    check("due open hanno posizioni indipendenti",
          vfs_lseek(fd1, 0, SEEK_CUR) == 2 &&
          vfs_lseek(fd2, 0, SEEK_CUR) == 0);

    check("la lettura da un descrittore non muove l'altro",
          vfs_read(fd2, buf, 2) == 2 && buf[0] == 'c');

    vfs_close(fd1);
    check("chiudere un descrittore non disturba l'altro",
          vfs_read(fd2, buf, 1) == 1 && buf[0] == 'a');

    /* I descrittori sono PER TASK: lo stesso numero in due task diversi non e'
       lo stesso file. E' cio' che rompe l'isolamento se si restituisce l'indice
       della tabella globale come fd. */
    albero();
    task_finto = 0;
    fd1 = vfs_open("/a", O_RDONLY);

    task_finto = 1;
    check("un fd di un altro task non e' valido qui",
          vfs_read(fd1, buf, 1) == -1);

    fd2 = vfs_open("/d/b", O_RDONLY);
    check("lo stesso numero di fd in due task e' disponibile", fd2 == fd1);
    check("e punta a un file diverso",
          vfs_read(fd2, buf, 1) == 1 && buf[0] == 'x');

    task_finto = 0;
    check("il descrittore del primo task e' ancora il suo",
          vfs_read(fd1, buf, 1) == 1 && buf[0] == 'c');
}

/* ---- directory ------------------------------------------------------------ */

static void test_readdir(void)
{
    int fd;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;

    albero();

    fd = vfs_open("/", O_RDONLY);

    check("readdir(0) da' la prima voce",
          vfs_readdir(fd, 0, nome, &ino) == 1 && same(nome, "a"));
    check("readdir(1) da' la seconda",
          vfs_readdir(fd, 1, nome, &ino) == 1 && same(nome, "d"));
    check("readdir riporta anche il numero di inode",
          vfs_readdir(fd, 0, nome, &ino) == 1 && ino == ino_a.ino);

    /* Zero significa "le voci sono finite", e -1 "la domanda non aveva senso":
       collassandoli, un ciclo che si ferma su entrambi sembra funzionare finche'
       non gli passi un file. */
    check("oltre l'ultima voce da' 0",
          vfs_readdir(fd, 4, nome, &ino) == 0);

    albero();
    fd = vfs_open("/a", O_RDONLY);
    check("readdir su un file da' -1",
          vfs_readdir(fd, 0, nome, &ino) == -1);

    check("readdir su un fd non aperto da' -1",
          vfs_readdir(7, 0, nome, &ino) == -1);
}

/* ---- M11c: il mount ----------------------------------------------------------

   Diciassette controlli, e il piu' importante e' l'ottavo: dopo il mount, "/d/b"
   deve FALLIRE. Un mount che aggiunge senza coprire non e' un mount — e' quello
   che faceva minixfs_graft, ed e' la ragione per cui questa milestone esiste. */
static void test_mount(void)
{
    struct inode *p;
    char nome[VFS_NAME_MAX + 1];
    uint32_t n;
    int fd, r;

    albero();

    /* I rifiuti, prima. Sono la meta' che nessun controllo positivo puo' vedere,
       ed e' la lezione dei tre bug di M9b: un valore di ritorno sbagliato
       sull'insuccesso non ha sintomi finche' qualcuno non ci cammina sopra. */
    check("montare su un path inesistente fallisce",
          vfs_mount("/nonesiste", &ino_mroot) == -1);

    check("montare su un file fallisce",
          vfs_mount("/a", &ino_mroot) == -1);

    check("montare una radice nulla fallisce",
          vfs_mount("/d", 0) == -1);

    check("montare una radice che non e' una directory fallisce",
          vfs_mount("/d", &ino_a) == -1);

    /* E dopo quattro rifiuti l'albero dev'essere INTATTO. Se uno dei quattro
       avesse scritto in tabella prima di controllare, "/d/b" sarebbe gia'
       coperto adesso: e' il bug di M11b — allocare e poi accorgersi di non
       poter riuscire — nella sua forma piu' piccola. */
    check("dopo quattro rifiuti /d/b si risolve ancora",
          vfs_resolve("/d/b", &p) == 0 && p == &ino_db);

    check("il mount riesce", vfs_mount("/d", &ino_mroot) == 0);

    /* L'identita' del PUNTATORE, non del numero: gli inode sono unici dentro un
       filesystem, non fra filesystem, quindi confrontare ino non proverebbe
       niente. E' la nota di M11a su "dev" e "hello.txt" entrambi inode 2. */
    check("/d da' esattamente l'inode montato",
          vfs_resolve("/d", &p) == 0 && p == &ino_mroot);

    /* IL controllo. Il mount COPRE: b esiste sotto e non si vede piu'. */
    check("/d/b non si risolve piu': il mount copre",
          vfs_resolve("/d/b", &p) == -1);

    check("/d/m si risolve: il contenuto e' quello montato",
          vfs_resolve("/d/m", &p) == 0 && p == &ino_m);

    /* Il resto dell'albero non si accorge di niente. Il primo dei due prende una
       sostituzione fatta confrontando ino invece del puntatore. */
    check("/a non e' cambiato",
          vfs_resolve("/a", &p) == 0 && p == &ino_a);

    check("/ non e' cambiato",
          vfs_resolve("/", &p) == 0 && p == &ino_root);

    /* readdir passa dal fd, quindi dall'inode risolto: deve elencare il montato.
       Se la sostituzione avvenisse solo dentro il ciclo di vfs_resolve e non sul
       valore consegnato, questo controllo la prenderebbe. */
    fd = vfs_open("/d", O_RDONLY);
    r = (fd >= 0 && vfs_readdir(fd, 0, nome, &n) == 1 && same(nome, "m"));

    if (fd >= 0)
        vfs_close(fd);

    check("readdir su /d elenca le voci del filesystem montato", r);

    /* L'impilamento, che e' anche il solo controllo del ciclo esterno di
       risoluzione. Il secondo mount ha come punto la RADICE DEL PRIMO — perche'
       vfs_resolve("/d") ora da' quella — quindi risolvere /d deve seguire la
       catena due volte. Con una sostituzione sola si fermerebbe a ino_mroot. */
    check("un secondo mount sullo stesso punto riesce",
          vfs_mount("/d", &ino_mroot2) == 0);

    check("e /d segue la catena fino all'ultimo montato",
          vfs_resolve("/d", &p) == 0 && p == &ino_mroot2);

    /* vfs_init azzera la tabella. Senza, ogni gruppo di controlli erediterebbe i
       mount del precedente — e nel kernel il ramo di ripiego di kmain fa un
       secondo vfs_init lasciando in piedi mount verso un filesystem smontato. */
    albero();

    check("dopo vfs_init la tabella e' vuota: /d/b torna",
          vfs_resolve("/d/b", &p) == 0 && p == &ino_db);

    /* La tabella si riempie, e il rifiuto e' esplicito invece che silenzioso.
       Quattro radici DIVERSE: ne esce la catena lineare

           d -> mroot -> mroot2 -> mroot3 -> mroot4

       che e' anche la piu' lunga costruibile con MAX_MOUNTS slot. Se il tetto
       del ciclo di risoluzione fosse di un giro troppo corto, si fermerebbe a
       mroot3 e il controllo qui sotto se ne accorgerebbe. */
    check("quattro mount riempiono la tabella",
          vfs_mount("/d", &ino_mroot)  == 0 &&
          vfs_mount("/d", &ino_mroot2) == 0 &&
          vfs_mount("/d", &ino_mroot3) == 0 &&
          vfs_mount("/d", &ino_mroot4) == 0);

    check("e il quinto viene rifiutato",
          vfs_mount("/d", &ino_mroot) == -1);
}

int main(void)
{
    test_resolve();
    test_open_close();
    test_read_write_seek();
    test_livelli();
    test_readdir();
    test_mount();

    if (failures == 0) {
        printf("tutti i test del VFS passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
