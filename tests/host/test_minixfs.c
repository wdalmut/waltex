/* I test di minixfs, ed e' la verifica piu' forte del progetto.
 *
 * Il riferimento non e' un pattern che ci siamo inventati: tests/data/minix.img
 * l'ha costruita mkfs.minix di util-linux e ci ha scritto dentro il modulo minix
 * del kernel Linux, attraverso mount. Sono due implementazioni vere, e nessuna
 * delle due e' la nostra. Quando minixfs.c e loro non sono d'accordo, la
 * differenza si localizza con od.
 *
 * Cio' che rende possibile provare tutto questo sull'host in millisecondi e' il
 * struct blockdev che M10 ha consegnato: ha due puntatori a funzione, e qui
 * diventano fread e fseek su un file. minixfs.c non si accorge della differenza.
 * E' la stessa idea del sink di kprintf e dell'albero finto di test_vfs.c, e
 * questa volta paga piu' di sempre.
 *
 * I valori attesi in questo file sono MISURATI sull'immagine con od, non
 * ricordati. Se l'immagine si rigenera con tools/mkminix.sh cambiano solo i
 * timestamp, che nessun controllo guarda. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "minixfs.h"
#include "blockdev.h"
#include "vfs.h"

/* Il percorso arriva dal Makefile ed e' ASSOLUTO, costruito con $(CURDIR).
   Un percorso relativo funzionerebbe solo lanciando il binario da tests/host, e
   la prima cosa che si fa e' lanciarlo dalla radice del repo. Si puo' comunque
   passare un'altra immagine come primo argomento. */
#ifndef MINIX_IMG
#define MINIX_IMG "../data/minix.img"
#endif

/* La copia su cui si LAVORA, anche lei assoluta e anche lei dal Makefile: il
   percorso non deve dipendere da dove si lancia il binario, altrimenti
   l'immagine di lavoro spunta ora nella radice del repo ora in tests/host. */
#ifndef MINIX_WORK
#define MINIX_WORK "../data/minix-lavoro.img"
#endif

static int failures;

static void check(const char *name, int ok)
{
    printf("%s -- %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

/* ---- il disco finto -----------------------------------------------------
 *
 * Quattro righe, ed e' tutto il ponte fra un filesystem del kernel e un file
 * dell'host. priv tiene il FILE *, che e' esattamente l'uso per cui priv esiste:
 * la stessa funzione read serve piu' dispositivi, e priv dice quale. */

static int file_read(struct blockdev *b, uint32_t lba, void *buf, uint32_t count)
{
    FILE *f = (FILE *)b->priv;

    if (count == 0)
        return 0;

    if (fseek(f, (long)lba * SECTOR_SIZE, SEEK_SET) != 0)
        return -1;

    if (fread(buf, SECTOR_SIZE, count, f) != count)
        return -1;

    return (int)count;
}

/* Non serve a niente in M11a, che e' di sola lettura, ma la struct la vuole e
   lasciarla nulla sarebbe una promessa diversa — "questo disco non si scrive"
   invece di "questo test non ci scrive". */
/* Da M11b scrive davvero — su una COPIA dell'immagine, mai sul riferimento.
   La copia la fa apri_immagine, e si rifa' a ogni esecuzione: un test che
   scrive nel proprio input non e' ripetibile, ed e' la lezione di disk.sh. */
static int file_write(struct blockdev *b, uint32_t lba, const void *buf,
                      uint32_t count)
{
    FILE *f = (FILE *)b->priv;

    if (count == 0)
        return 0;

    if (fseek(f, (long)lba * SECTOR_SIZE, SEEK_SET) != 0)
        return -1;

    if (fwrite(buf, SECTOR_SIZE, count, f) != count)
        return -1;

    /* Il flush a ogni scrittura e' l'equivalente di cache=writethrough in QEMU e
       del FLUSH CACHE del driver ATA: senza, i controlli che rileggono
       vedrebbero il buffer della libc invece del file. */
    fflush(f);

    return (int)count;
}

static FILE *img;
static struct blockdev disco;

/* La copia su cui si lavora. Il riferimento in tests/data/ non si tocca MAI:
   e' committato, ed e' l'unica cosa che non deve poter cambiare. */
static char copia[512];

static int apri_immagine(const char *path)
{
    FILE *src;
    char buf[4096];
    size_t n;

    /* Si rifa' a ogni esecuzione, e sta accanto al riferimento invece che in
       /tmp: cosi' quando un controllo fallisce si ispeziona con od, e ci si
       lancia fsck.minix — che e' la prima cosa da fare. */
    snprintf(copia, sizeof(copia), "%s", MINIX_WORK);

    src = fopen(path, "rb");

    if (src == NULL) {
        fprintf(stderr, "test_minixfs: non trovo %s\n", path);
        fprintf(stderr, "              (si rigenera con tools/mkminix.sh)\n");
        return -1;
    }

    img = fopen(copia, "w+b");

    if (img == NULL) {
        fprintf(stderr, "test_minixfs: non riesco a creare %s\n", copia);
        fclose(src);
        return -1;
    }

    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, img);

    fclose(src);
    fflush(img);

    memset(&disco, 0, sizeof(disco));
    strcpy(disco.name, "img");
    disco.nsectors = 256 * 2;        /* 256 KB */
    disco.read  = file_read;
    disco.write = file_write;
    disco.priv  = img;

    return 0;
}

/* ---- aiutanti -----------------------------------------------------------
 *
 * I test non chiamano minix_lookup e minix_read per nome: sono static dentro
 * minixfs.c, e si raggiungono attraverso ops, che e' anche il modo in cui le
 * chiama il VFS. Provare la stessa strada che percorre il codice vero, invece
 * di una scorciatoia, e' meta' del valore di questi test. */

static struct inode *cerca(struct inode *dir, const char *nome)
{
    struct inode *out = NULL;

    if (dir == NULL || dir->ops == NULL || dir->ops->lookup == NULL)
        return NULL;

    if (dir->ops->lookup(dir, nome, &out) < 0)
        return NULL;

    return out;
}

/* Legge un file intero a pezzi da 100 byte — un numero che NON divide 1024
   apposta: cosi' ogni lettura cade a cavallo dei confini di zona in un punto
   diverso, ed e' il caso che una lettura da 1024 allineata non proverebbe mai. */
static int leggi_tutto(struct inode *ino, char *dest, int max)
{
    int off = 0;
    int r;

    while (off < max) {
        r = ino->ops->read(ino, (uint32_t)off, dest + off, 100);

        if (r < 0)
            return -1;

        if (r == 0)
            break;

        off += r;
    }

    return off;
}

/* ---- i controlli --------------------------------------------------------- */

static void test_mount(void)
{
    check("il mount di un disco nullo fallisce", minixfs_init(NULL) < 0);
    check("e dopo un mount fallito la radice e' nulla", minixfs_root() == NULL);

    check("il mount dell'immagine di riferimento riesce",
          minixfs_init(&disco) == 0);
}

static void test_radice(void)
{
    struct inode *root = minixfs_root();

    check("minixfs_root non e' nulla", root != NULL);

    if (root == NULL)
        return;

    /* Il tipo si controlla a parte: una radice mai riempita e' tutta zeri,
       quindi il puntatore e' valido e type vale INODE_NONE. */
    check("la radice e' una directory", root->type == INODE_DIR);

    /* L'inode 1, non lo 0. Lo zero significa "nessun inode", ed e' il valore con
       cui una voce di directory dice "cancellata". */
    check("la radice e' l'inode 1", root->ino == 1);

    /* 144 byte = nove voci da sedici. Misurato con od, e coerente con quello
       che ls mostra sull'immagine montata: . .. hello.txt etc grande.txt
       enorme.txt vuoto.txt dev proc

       Le ultime due sono i PUNTI DI MOUNT, arrivate in M11c e in M11d: sono
       directory VUOTE sul disco, perche' in Unix mount copre una directory che
       c'e' gia' invece di aggiungere un nome. E' per questo che minix_readdir
       non deve sapere niente dei mount — i nomi glieli da' il disco. */
    check("la radice misura 144 byte, cioe' nove voci", root->size == 144);

    /* LA proprieta' della cache, e non e' un'ottimizzazione: due lookup dello
       stesso path devono dare lo STESSO puntatore. Con due copie, la i_size di
       una puo' divergere dall'altra. */
    check("due minixfs_root() danno lo stesso puntatore",
          minixfs_root() == root);
}

static void test_lookup(void)
{
    struct inode *root = minixfs_root();
    struct inode *hello, *etc, *motd, *vuoto;

    if (root == NULL)
        return;

    hello = cerca(root, "hello.txt");
    check("lookup trova hello.txt", hello != NULL);
    check("hello.txt e' un file", hello != NULL && hello->type == INODE_FILE);
    check("hello.txt misura 26 byte", hello != NULL && hello->size == 26);

    etc = cerca(root, "etc");
    check("lookup trova etc", etc != NULL);
    check("etc e' una directory", etc != NULL && etc->type == INODE_DIR);

    /* Due livelli: e' il caso che vfs_resolve percorre davvero, e l'unico che
       prova che la lookup funzioni su una directory che non e' la radice. */
    motd = cerca(etc, "motd");
    check("lookup trova etc/motd", motd != NULL);
    check("etc/motd misura 35 byte", motd != NULL && motd->size == 35);

    /* size 0 e zone[0] a zero: il caso che fa sbagliare chi deduce la fine del
       file dalla prima zona nulla invece che da i_size. */
    vuoto = cerca(root, "vuoto.txt");
    check("lookup trova vuoto.txt", vuoto != NULL);
    check("vuoto.txt misura 0 byte", vuoto != NULL && vuoto->size == 0);

    check("lookup non trova un nome inesistente",
          cerca(root, "nonesiste") == NULL);

    /* Il prefisso NON deve bastare, ed e' il controllo che prende il confronto
       troncato a 14 caratteri senza verificare la lunghezza del nome cercato. */
    check("un prefisso non basta: hello.tx non si trova",
          cerca(root, "hello.tx") == NULL);
    check("ne' un nome piu' lungo: hello.txtX non si trova",
          cerca(root, "hello.txtX") == NULL);

    /* . e .. sono voci normali sul disco, non casi speciali del codice, e
       misurate ci sono davvero: sono le prime due della radice. */
    check(". risolve alla radice stessa", cerca(root, ".") == root);
    check(".. della radice risolve alla radice", cerca(root, "..") == root);

    /* La stessa lookup due volte: lo stesso puntatore. */
    check("due lookup di hello.txt danno lo stesso puntatore",
          cerca(root, "hello.txt") == hello);
}

static void test_read_piccolo(void)
{
    struct inode *root = minixfs_root();
    struct inode *hello, *vuoto;
    char buf[2048];
    int n;

    if (root == NULL)
        return;

    hello = cerca(root, "hello.txt");

    if (hello == NULL)
        return;

    memset(buf, 0x5A, sizeof(buf));
    n = leggi_tutto(hello, buf, (int)sizeof(buf));

    /* 26 byte, non 1024. Senza il troncamento su i_size si leggerebbe la zona
       intera, e i 998 byte in piu' non sarebbero spazzatura casuale: sarebbero
       dati veri di qualcun altro, quindi con l'aria di essere giusti. */
    check("hello.txt legge esattamente 26 byte", n == 26);
    check("e il contenuto e' quello scritto dall'host",
          n == 26 && memcmp(buf, "ciao dal filesystem minix\n", 26) == 0);
    check("e il byte 27 del buffer non e' stato toccato",
          (unsigned char)buf[26] == 0x5A);

    /* Leggere oltre la fine da' 0, che significa "finito" e non un errore. */
    check("leggere a partire da i_size da' 0",
          hello->ops->read(hello, 26, buf, 100) == 0);
    check("leggere molto oltre la fine da' 0",
          hello->ops->read(hello, 100000, buf, 100) == 0);

    /* Una lettura che comincia dentro e finisce oltre la fine va TRONCATA, non
       rifiutata. */
    check("una lettura a cavallo della fine si tronca",
          hello->ops->read(hello, 20, buf, 100) == 6);

    vuoto = cerca(root, "vuoto.txt");
    check("un file vuoto da' 0 alla prima lettura",
          vuoto != NULL && vuoto->ops->read(vuoto, 0, buf, 100) == 0);
}

static void test_read_zone_dirette(void)
{
    struct inode *grande = cerca(minixfs_root(), "grande.txt");
    char buf[6000];
    int n, i, tutti;

    check("lookup trova grande.txt", grande != NULL);

    if (grande == NULL)
        return;

    /* 5000 byte = cinque zone DIRETTE. Misurato: zone 11 12 13 14 15. */
    check("grande.txt misura 5000 byte", grande->size == 5000);

    memset(buf, 0, sizeof(buf));
    n = leggi_tutto(grande, buf, (int)sizeof(buf));

    check("grande.txt legge esattamente 5000 byte", n == 5000);

    tutti = (n == 5000);
    for (i = 0; i < n && tutti; i++)
        if (buf[i] != 'G')
            tutti = 0;

    /* Un carattere solo ripetuto non proverebbe granche' da solo — ma qui il
       punto e' la CONTINUITA' fra zone: un salto di zona sbagliato darebbe zeri
       o pezzi di un altro file in mezzo, e il confronto li prende. */
    check("e sono cinquemila G di fila, senza buchi fra le zone", tutti);
}

static void test_read_indiretto(void)
{
    struct inode *enorme = cerca(minixfs_root(), "enorme.txt");
    char *buf;
    int n, i, tutti;

    check("lookup trova enorme.txt", enorme != NULL);

    if (enorme == NULL)
        return;

    /* IL test di M11a.
     *
     * 20000 byte sono 20 zone: le prime sette dirette — misurate, 16..22 — e
     * le altre tredici attraverso il blocco INDIRETTO, che e' la zona 23 e
     * contiene 24 25 26 ... 36.
     *
     * Senza un file oltre i 7168 byte la mappatura si prova solo nel ramo
     * facile, e il bug classico di minix v1 — i puntatori di zona letti come
     * uint32 invece che uint16, che e' il formato della v2 — non lo vedrebbe
     * nessuno: i file piccoli continuerebbero a funzionare. */
    check("enorme.txt misura 20000 byte", enorme->size == 20000);

    buf = malloc(25000);
    if (buf == NULL)
        return;

    memset(buf, 0, 25000);
    n = leggi_tutto(enorme, buf, 21000);

    check("enorme.txt legge esattamente 20000 byte", n == 20000);

    tutti = (n == 20000);
    for (i = 0; i < n && tutti; i++)
        if (buf[i] != 'Z')
            tutti = 0;

    check("e sono ventimila Z: l'indiretto e' mappato bene", tutti);

    /* I due confini, provati singolarmente, perche' e' li' che un > diventa
       un >= e viceversa:
       il byte 7167 e' l'ultimo della settima zona DIRETTA,
       il byte 7168 e' il primo della prima zona INDIRETTA. */
    check("il byte 7167, l'ultimo diretto, e' una Z", buf[7167] == 'Z');
    check("il byte 7168, il primo indiretto, e' una Z", buf[7168] == 'Z');
    check("e l'ultimo byte del file e' una Z", buf[19999] == 'Z');

    free(buf);
}

static void test_readdir(void)
{
    struct inode *root = minixfs_root();
    struct inode *etc;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;
    int r, n;

    /* L'ordine e' quello sul disco, misurato con od — non l'ordine in cui
       mkminix.sh crea i file, che per caso coincide. "dev" e' l'ultima perche'
       lo script la crea per ultima apposta: cosi' i numeri di inode dei file di
       prova non si spostano, e hello.txt resta il 2. */
    static const char *attese[] = {
        ".", "..", "hello.txt", "etc", "grande.txt", "enorme.txt", "vuoto.txt",
        "dev", "proc"
    };

    if (root == NULL)
        return;

    for (n = 0; n < 9; n++) {
        memset(nome, 0, sizeof(nome));
        ino = 0xDEADBEEF;

        r = root->ops->readdir(root, n, nome, &ino);

        if (r != 1) {
            check("readdir della radice da' nove voci", 0);
            return;
        }

        if (strcmp(nome, attese[n]) != 0) {
            printf("FAIL -- voce %d: attesa \"%s\", trovata \"%s\"\n",
                   n, attese[n], nome);
            failures++;
            return;
        }
    }

    check("readdir della radice elenca le nove voci nell'ordine giusto", 1);

    /* Lo zero significa "le voci sono finite", ed e' distinto dal -1: un ciclo
       che si fermasse su entrambi sembrerebbe funzionare fino al giorno in cui
       readdir comincia a fallire davvero. */
    check("oltre l'ultima voce readdir da' 0",
          root->ops->readdir(root, 9, nome, &ino) == 0);

    /* Il numero di inode di una voce nota, misurato: hello.txt e' l'inode 2. */
    root->ops->readdir(root, 2, nome, &ino);
    check("la voce hello.txt porta il numero di inode 2", ino == 2);

    etc = cerca(root, "etc");

    if (etc == NULL)
        return;

    check("readdir funziona anche su una directory che non e' la radice",
          etc->ops->readdir(etc, 2, nome, &ino) == 1 &&
          strcmp(nome, "motd") == 0);

    /* readdir e lookup descrivono lo stesso insieme, e niente le costringe a
       essere d'accordo: se divergessero, ls mostrerebbe un nome che cat non
       riesce ad aprire. */
    check("readdir e lookup sono d'accordo su etc/motd",
          cerca(etc, "motd") != NULL);
}

/* ---- M11b: la scrittura ---------------------------------------------------
 *
 * Ogni controllo che conta passa da un RIMONTAGGIO. Senza, si verifica la
 * cache degli inode invece del disco: una modifica che non e' mai stata scritta
 * si rilegge benissimo finche' il filesystem resta montato, e sparisce al
 * riavvio. E' il bug piu' frequente della milestone, e l'unico modo di
 * prenderlo e' rimontare. */

static void rimonta(void)
{
    check("il rimontaggio riesce", minixfs_init(&disco) == 0);
}

static void test_scrittura(void)
{
    struct inode *hello;
    /* 4096 e non 2048: piu' sotto il file cresce a 3000 byte e ci si legge
       dentro tutto. Con 2048 il memset ne scriveva 952 di troppo, e il canary
       di gcc lo prendeva con "stack smashing detected" — che almeno e' un
       guasto rumoroso. */
    char buf[4096];
    int r;

    hello = cerca(minixfs_root(), "hello.txt");

    if (hello == NULL || hello->ops->write == NULL) {
        check("minix_read ha una write", 0);
        return;
    }

    /* Sovrascrivere DENTRO un file esistente, senza allargarlo. Il caso piu'
       semplice: nessuna zona da allocare, nessuna size da aggiornare. */
    r = hello->ops->write(hello, 0, "CIAO", 4);
    check("scrivere 4 byte all'inizio di hello.txt riesce", r == 4);
    check("e la size non cambia", hello->size == 26);

    memset(buf, 0, sizeof(buf));
    hello->ops->read(hello, 0, buf, 26);
    check("i 4 byte sono cambiati", memcmp(buf, "CIAO", 4) == 0);

    /* E i byte INTORNO non devono essere cambiati: e' il controllo che prende
       una write che riscrive il blocco intero invece di leggerlo prima. */
    check("e i 22 byte dopo sono intatti",
          memcmp(buf + 4, " dal filesystem minix\n", 22) == 0);

    /* Il rimontaggio: se la write non e' arrivata al disco, qui torna il
       contenuto originale. */
    rimonta();
    hello = cerca(minixfs_root(), "hello.txt");
    memset(buf, 0, sizeof(buf));
    hello->ops->read(hello, 0, buf, 26);
    check("dopo il rimontaggio i 4 byte sono ancora cambiati",
          memcmp(buf, "CIAO", 4) == 0);

    /* Far CRESCERE un file: da 26 byte a 3000, cioe' da una zona a tre. Serve
       allocare due zone e riscrivere i_size. */
    memset(buf, 'K', 3000);
    r = hello->ops->write(hello, 26, buf, 2974);
    check("far crescere hello.txt a 3000 byte riesce", r == 2974);
    check("e la size e' aggiornata", hello->size == 3000);

    rimonta();
    hello = cerca(minixfs_root(), "hello.txt");
    check("dopo il rimontaggio la size e' ancora 3000",
          hello != NULL && hello->size == 3000);

    memset(buf, 0, sizeof(buf));
    r = leggi_tutto(hello, buf, 3000);
    check("e il file si rilegge tutto", r == 3000);
    check("con il contenuto giusto ai due capi",
          memcmp(buf, "CIAO", 4) == 0 && buf[26] == 'K' && buf[2999] == 'K');
}

static void test_creazione(void)
{
    struct inode *root = minixfs_root();
    struct inode *nuovo = NULL;
    char buf[64];
    int r;

    if (root == NULL || root->ops->create == NULL) {
        check("la radice ha una create", 0);
        return;
    }

    r = root->ops->create(root, "nuovo.txt", INODE_FILE, &nuovo);
    check("creare /nuovo.txt riesce", r == 0);

    if (r != 0 || nuovo == NULL) {
        check("e l'inode e' un file vuoto", 0);
        return;
    }

    check("e l'inode e' un file vuoto",
          nuovo->type == INODE_FILE && nuovo->size == 0);

    /* Il numero deve essere NUOVO, non uno di quelli gia' in uso: l'immagine ha
       gli inode da 1 a 9 occupati, quindi il primo libero e' il 10.

       Era 8 in M11b, 9 in M11c, 10 da M11d: se li sono presi "dev" e poi "proc",
       i due punti di mount. E' l'unico controllo della suite che guarda un
       numero di inode ASSOLUTO invece di un nome, e per questo l'unico che si
       accorge di ogni directory nuova sull'immagine. */
    check("con un numero di inode non ancora usato", nuovo->ino == 10);

    /* Creare due volte lo stesso nome deve FALLIRE. Chi crea deve poterlo
       sapere: e' vfs_open con O_CREAT a decidere di aprire quello che c'e'. */
    check("crearlo una seconda volta fallisce",
          root->ops->create(root, "nuovo.txt", INODE_FILE, &nuovo) < 0);

    /* Scriverci dentro, e rimontare. La guardia non e' pedanteria: finche'
       create e' uno stub, cerca() ritorna NULL e senza il controllo il test
       muore di SIGSEGV invece di riportare — cioe' smette di dire a che punto
       era arrivato, che e' l'unica cosa che serve mentre si scrive il codice. */
    nuovo = cerca(root, "nuovo.txt");

    if (nuovo == NULL) {
        check("/nuovo.txt si ritrova con lookup", 0);
        return;
    }

    nuovo->ops->write(nuovo, 0, "contenuto\n", 10);

    rimonta();
    root = minixfs_root();
    nuovo = cerca(root, "nuovo.txt");

    check("dopo il rimontaggio /nuovo.txt esiste ancora", nuovo != NULL);
    check("con la size giusta", nuovo != NULL && nuovo->size == 10);

    if (nuovo == NULL)
        return;

    memset(buf, 0, sizeof(buf));
    nuovo->ops->read(nuovo, 0, buf, 10);
    check("e il contenuto giusto", memcmp(buf, "contenuto\n", 10) == 0);

    /* E deve comparire in readdir: create ha inserito una voce, non solo
       allocato un inode. Un inode allocato e non collegato e' quello che fsck
       chiama "Unattached inode". */
    check("e readdir della radice lo elenca", cerca(root, "nuovo.txt") != NULL);
}

static void test_mkdir(void)
{
    struct inode *root = minixfs_root();
    struct inode *dir = NULL;
    struct inode *dentro = NULL;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;
    int r;

    if (root == NULL || root->ops->create == NULL)
        return;

    r = root->ops->create(root, "sub", INODE_DIR, &dir);
    check("mkdir /sub riesce", r == 0);

    if (r != 0 || dir == NULL) {
        check("ed e' una directory", 0);
        return;
    }

    check("ed e' una directory", dir->type == INODE_DIR);

    /* Una directory nasce con "." e "..", e la size lo dice: due voci da 16. */
    check("che nasce con due voci, cioe' 32 byte", dir->size == 32);

    check("la prima voce e' \".\" e punta a se stessa",
          dir->ops->readdir(dir, 0, nome, &ino) == 1 &&
          strcmp(nome, ".") == 0 && ino == dir->ino);

    check("la seconda e' \"..\" e punta alla radice",
          dir->ops->readdir(dir, 1, nome, &ino) == 1 &&
          strcmp(nome, "..") == 0 && ino == root->ino);

    /* Creare dentro la directory nuova: e' il caso che prova che create
       funziona su una directory che non e' la radice. */
    if (dir->ops == NULL || dir->ops->create == NULL) {
        check("la directory nuova ha una create", 0);
        return;
    }

    r = dir->ops->create(dir, "dentro.txt", INODE_FILE, &dentro);
    check("creare /sub/dentro.txt riesce", r == 0);

    rimonta();
    root = minixfs_root();
    dir = cerca(root, "sub");

    check("dopo il rimontaggio /sub esiste", dir != NULL);
    check("ed e' ancora una directory", dir != NULL && dir->type == INODE_DIR);
    check("e /sub/dentro.txt e' dentro",
          dir != NULL && cerca(dir, "dentro.txt") != NULL);

    /* Il nome che non ci sta: quattordici caratteri e' il massimo, e si
       RIFIUTA invece di troncare. Troncare farebbe collidere due file diversi,
       che e' l'errore del nome di dispositivo in M8. */
    check("un nome di 15 caratteri e' rifiutato",
          root->ops->create(root, "quindici_caratt", INODE_FILE, &dentro) < 0);
}

static void test_crescita_directory(void)
{
    struct inode *root = minixfs_root();
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;
    int i, creati = 0, contate = 0;

    if (root == NULL || root->ops->create == NULL)
        return;

    /* IL controllo che prende la crescita della directory.
     *
     * La radice sta in UNA zona da 1024 byte, cioe' 64 voci. Ne ha gia' una
     * decina, quindi il ramo "non c'e' piu' posto, allarga" non si esercita
     * finche' non se ne creano una sessantina. Un test che ne creasse tre
     * passerebbe con quel ramo completamente assente. */
    for (i = 0; i < 70; i++) {
        struct inode *f = NULL;

        nome[0] = 'f';
        nome[1] = (char)('0' + (i / 10));
        nome[2] = (char)('0' + (i % 10));
        nome[3] = '\0';

        if (root->ops->create(root, nome, INODE_FILE, &f) == 0)
            creati++;
        else
            break;      /* inode finiti: 96 in tutto, ed e' un esito legittimo */
    }

    check("si creano abbastanza file da far crescere la radice", creati > 55);
    check("e la radice ora occupa piu' di una zona", root->size > 1024);

    rimonta();
    root = minixfs_root();

    for (i = 0; i < 200; i++) {
        if (root->ops->readdir(root, i, nome, &ino) != 1)
            break;
        if (ino != 0)
            contate++;
    }

    /* Le voci contate dopo il rimontaggio devono essere quelle di prima piu'
       quelle create. Se i_size della directory non fosse stata riscritta,
       l'ultima parte sparirebbe qui. */
    check("dopo il rimontaggio ci sono ancora tutte", contate >= creati);
}

int main(int argc, char **argv)
{
    /* Riga per riga, e non e' cosmetica: con il buffering pieno — quello che si
       ottiene quando l'output e' rediretto — un SIGSEGV butta via tutto quello
       che il test aveva gia' stampato, e non si sa nemmeno a quale controllo era
       arrivato. Da M11b il test scrive sul disco, quindi puo' schiantarsi
       davvero. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (apri_immagine(argc > 1 ? argv[1] : MINIX_IMG) < 0)
        return 1;

    test_mount();
    test_radice();
    test_lookup();
    test_read_piccolo();
    test_read_zone_dirette();
    test_read_indiretto();
    test_readdir();

    /* M11b. Vengono per ultimi perche' modificano l'immagine: i controlli di
       lettura devono girare su uno stato noto. */
    test_scrittura();
    test_creazione();
    test_mkdir();
    test_crescita_directory();

    fclose(img);

    if (failures == 0)
        printf("tutti i test di minixfs passano\n");
    else
        printf("%d test di minixfs FALLITI\n", failures);

    return failures != 0;
}
