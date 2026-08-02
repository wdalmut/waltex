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
static int file_write(struct blockdev *b, uint32_t lba, const void *buf,
                      uint32_t count)
{
    (void)b; (void)lba; (void)buf; (void)count;
    return -1;
}

static FILE *img;
static struct blockdev disco;

static int apri_immagine(const char *path)
{
    img = fopen(path, "rb");

    if (img == NULL) {
        fprintf(stderr, "test_minixfs: non trovo %s\n", path);
        fprintf(stderr, "              (si rigenera con tools/mkminix.sh)\n");
        return -1;
    }

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

    /* 112 byte = sette voci da sedici. Misurato con od, e coerente con quello
       che ls mostra sull'immagine montata: . .. hello.txt etc grande.txt
       enorme.txt vuoto.txt */
    check("la radice misura 112 byte, cioe' sette voci", root->size == 112);

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

    /* L'ordine e' quello sul disco, misurato con od. */
    static const char *attese[] = {
        ".", "..", "hello.txt", "etc", "grande.txt", "enorme.txt", "vuoto.txt"
    };

    if (root == NULL)
        return;

    for (n = 0; n < 7; n++) {
        memset(nome, 0, sizeof(nome));
        ino = 0xDEADBEEF;

        r = root->ops->readdir(root, n, nome, &ino);

        if (r != 1) {
            check("readdir della radice da' sette voci", 0);
            return;
        }

        if (strcmp(nome, attese[n]) != 0) {
            printf("FAIL -- voce %d: attesa \"%s\", trovata \"%s\"\n",
                   n, attese[n], nome);
            failures++;
            return;
        }
    }

    check("readdir della radice elenca le sette voci nell'ordine giusto", 1);

    /* Lo zero significa "le voci sono finite", ed e' distinto dal -1: un ciclo
       che si fermasse su entrambi sembrerebbe funzionare fino al giorno in cui
       readdir comincia a fallire davvero. */
    check("oltre l'ultima voce readdir da' 0",
          root->ops->readdir(root, 7, nome, &ino) == 0);

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

static void test_graft(void)
{
    struct inode *root = minixfs_root();
    struct inode *trovato;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;
    int i, r, visto;

    /* Un inode finto: minixfs non deve sapere da dove viene. Nel kernel sara'
       devfs_root(), qui e' questa struct — ed e' lo stesso espediente di
       vfs_init(root). */
    static struct inode finto;

    if (root == NULL)
        return;

    finto.ino  = 999;
    finto.type = INODE_DIR;

    check("innestare prima del mount o con nome troppo lungo e' rifiutato",
          minixfs_graft("un_nome_lunghissimo_davvero", &finto) < 0);

    check("l'innesto riesce", minixfs_graft("dev", &finto) == 0);

    /* Uno slot, non una tabella: il secondo innesto si RIFIUTA invece di
       sostituire, perche' una sostituzione silenziosa sarebbe una directory che
       cambia sotto i piedi. */
    check("un secondo innesto e' rifiutato", minixfs_graft("altro", &finto) < 0);

    trovato = cerca(root, "dev");
    check("lookup della radice trova l'innesto", trovato == &finto);

    /* E deve comparire anche in readdir, altrimenti si ottiene un /dev che cat
       apre e ls non mostra. Le due funzioni descrivono lo stesso insieme. */
    visto = 0;
    for (i = 0; i < 16; i++) {
        memset(nome, 0, sizeof(nome));
        r = root->ops->readdir(root, i, nome, &ino);

        if (r != 1)
            break;

        if (strcmp(nome, "dev") == 0)
            visto = 1;
    }

    check("e readdir della radice lo elenca", visto);

    /* L'innesto non deve nascondere il disco: i nomi che c'erano ci sono
       ancora. */
    check("l'innesto non nasconde i file veri",
          cerca(root, "hello.txt") != NULL);

    /* E non deve toccare il disco: montare non scrive niente sul filesystem
       montante, ed e' il motivo per cui dentro l'immagine non esiste nessuna
       directory "dev". */
    check("l'innesto non e' sul disco: dopo un rimount non c'e' piu'",
          minixfs_init(&disco) == 0 && cerca(minixfs_root(), "dev") == NULL);
}

int main(int argc, char **argv)
{
    if (apri_immagine(argc > 1 ? argv[1] : MINIX_IMG) < 0)
        return 1;

    test_mount();
    test_radice();
    test_lookup();
    test_read_piccolo();
    test_read_zone_dirette();
    test_read_indiretto();
    test_readdir();
    test_graft();

    fclose(img);

    if (failures == 0)
        printf("tutti i test di minixfs passano\n");
    else
        printf("%d test di minixfs FALLITI\n", failures);

    return failures != 0;
}
