/* I test di procfs, e sono il controllo che M11c aveva lasciato scoperto: un
 * filesystem scritto DOPO la tabella di mount, che non condivide una riga con
 * quelli che c'erano gia'.
 *
 * Girano interamente sull'host perche' procfs non tocca nessun hardware: la sua
 * unica sorgente di verita' e' la tabella dei task, che e' un array in .bss. Non
 * serve nemmeno il disco finto che test_minixfs.c deve costruire — e' il quarto
 * modulo del progetto provato cosi', dopo kprintf, il VFS e minixfs.
 *
 * Cosa NON si prova qui: che /proc sia MONTATO. Quello e' un fatto del kernel, e
 * lo prendono i self-check dentro la VM.
 */

#include <stdio.h>
#include <string.h>

#include "procfs.h"
#include "task.h"
#include "vfs.h"
#include "memory.h"

/* task.c si linka VERO, e schedule() chiama task_switch, che sta in switch.S:
   assembly che sull'host non si assembla. Lo stub basta perche' nessun test qui
   commuta — procfs LEGGE la tabella dei task, non la usa. */
void task_switch(uint32_t *old_esp, uint32_t new_esp)
{
    (void)old_esp;
    (void)new_esp;
}

/* kprintf.c si linka per snprintf, e i suoi due sink veri vogliono l'hardware.
   Stessa soluzione di test_kprintf.c, che li stuba nello stesso modo. */
void vga_putc(char c) { (void)c; }
void serial_putc(char c) { (void)c; }

static int failures;

static void check(const char *name, int ok)
{
    printf("%s -- %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

static void task_di_prova(void) { for (;;) { } }

static struct inode *cerca(struct inode *dir, const char *nome)
{
    struct inode *out = NULL;

    if (dir == NULL || dir->ops == NULL || dir->ops->lookup == NULL)
        return NULL;

    if (dir->ops->lookup(dir, nome, &out) < 0)
        return NULL;

    return out;
}

/* Legge un inode per intero come fa shell_cat: a pezzi, avanzando l'offset,
   fermandosi sullo zero. Il pezzo e' SETTE byte, un numero che non divide niente
   apposta — cosi' ogni lettura cade in un punto diverso del testo generato, e una
   read che ignorasse l'offset si vedrebbe subito. */
static int leggi_tutto(struct inode *ino, char *dest, int max)
{
    int off = 0;
    int r;

    while (off < max - 1) {
        r = ino->ops->read(ino, (uint32_t)off, dest + off, 7);

        if (r < 0)
            return -1;

        if (r == 0)
            break;

        off += r;
    }

    dest[off] = '\0';
    return off;
}

/* ---- la radice ------------------------------------------------------------- */

static void test_radice(void)
{
    struct inode *root;

    check("procfs_procdir e' nulla prima di procfs_init",
          procfs_procdir() == NULL);

    procfs_init();
    root = procfs_procdir();

    check("procfs_procdir non e' nulla dopo procfs_init", root != NULL);

    if (root == NULL)
        return;

    check("la radice e' una directory", root->type == INODE_DIR);
    check("la radice e' l'inode 1", root->ino == PROC_INO_ROOT);

    /* size 0 e' CORRETTO, non una dimenticanza: la dimensione di un file
       generato non si conosce prima di generarlo. vfs_read non consulta size e
       shell_cat esce sullo zero — e' anche cio' che fa Linux, dove
       ls -l /proc/1/status mostra 0 byte. */
    check("la radice ha size 0", root->size == 0);

    /* Due chiamate danno lo STESSO puntatore, ed e' la proprieta' su cui poggia
       la tabella di mount di M11c: quella indicizza per puntatore. */
    check("due procfs_procdir() danno lo stesso puntatore",
          procfs_procdir() == root);
}

/* ---- le voci di /proc ------------------------------------------------------ */

static void test_voci(void)
{
    struct inode *root = procfs_procdir();
    struct inode *d0, *d1;
    int t;

    if (root == NULL)
        return;

    /* task_init registra il contesto corrente come task 0: da qui la tabella ha
       almeno un task pronto. */
    task_init();

    d0 = cerca(root, "0");

    check("/proc/0 esiste appena c'e' un task", d0 != NULL);
    check("ed e' una DIRECTORY, non il file", d0 != NULL && d0->type == INODE_DIR);
    check("con il numero di inode che gli spetta",
          d0 != NULL && d0->ino == PROC_INO_TASK(0));

    /* Nessun inode vale zero. Lo zero significa "nessun inode" — e' il valore
       con cui una voce di directory minix dice "cancellata" — quindi un
       filesystem che lo usasse per un file vero mentirebbe a chiunque lo
       controlli. */
    check("e non e' zero", d0 != NULL && d0->ino != 0);

    /* Uno slot libero NON compare: e' cio' che distingue "genero le voci dalla
       tabella" da "ho otto directory fisse". */
    check("uno slot libero non compare in /proc", cerca(root, "7") == NULL);

    t = task_create(task_di_prova);
    check("task_create riesce", t > 0);

    d1 = cerca(root, "1");
    check("e la sua directory compare SUBITO, senza rifare procfs_init",
          d1 != NULL && d1->type == INODE_DIR);

    /* IL controllo che il primo procfs non passava: due lookup diversi devono
       dare due OGGETTI diversi. Con un solo inode condiviso e riempito a ogni
       chiamata, la seconda lookup distrugge il risultato della prima — e due
       open su file diversi finiscono sullo stesso struct inode. E' la lezione di
       M11a: lookup restituisce un puntatore che deve sopravvivere alla
       chiamata. */
    check("/proc/0 e /proc/1 sono due oggetti distinti", d0 != d1);

    /* E il primo non e' stato modificato dalla ricerca del secondo. */
    check("cercare /proc/1 non ha alterato l'inode di /proc/0",
          d0 != NULL && d0->ino == PROC_INO_TASK(0));

    /* Lo stesso nome due volte da' lo stesso puntatore. */
    check("due lookup dello stesso nome danno lo stesso puntatore",
          cerca(root, "0") == d0);

    /* I nomi che non sono numeri, e i numeri malformati. Tre grafie diverse per
       lo stesso task sarebbero la stessa specie di errore del nome di
       dispositivo troncato in M8. */
    check("un nome non numerico non si risolve", cerca(root, "pippo") == NULL);
    check("un nome che COMINCIA con una cifra non si risolve",
          cerca(root, "0pippo") == NULL);
    check("un indice oltre MAX_TASKS non si risolve", cerca(root, "99") == NULL);
    check("un numero enorme non gira e non si risolve",
          cerca(root, "99999999999") == NULL);
    check("il nome vuoto non si risolve", cerca(root, "") == NULL);
}

/* ---- readdir --------------------------------------------------------------- */

static void test_readdir(void)
{
    struct inode *root = procfs_procdir();
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;
    int n, visti;

    if (root == NULL)
        return;

    /* readdir e lookup devono descrivere lo STESSO insieme: e' la lezione di
       M11a, dove un innesto presente solo in lookup dava un /dev che cat apriva
       e ls non mostrava. */
    visti = 0;

    for (n = 0; n < MAX_TASKS + 2; n++) {
        memset(nome, 0, sizeof(nome));

        if (root->ops->readdir(root, n, nome, &ino) != 1)
            break;

        if (cerca(root, nome) == NULL) {
            printf("FAIL -- readdir da' \"%s\", che lookup non trova\n", nome);
            failures++;
            return;
        }

        visti++;
    }

    check("readdir e lookup della radice sono d'accordo", visti > 0);

    /* Due task attivi: lo 0 di task_init e quello di task_create. */
    check("readdir elenca esattamente i task attivi", visti == 2);

    check("oltre l'ultima voce readdir da' 0",
          root->ops->readdir(root, visti, nome, &ino) == 0);

    /* -1 e non 0: "la domanda non aveva senso" e "le voci sono finite" sono due
       cose diverse, e un ciclo che si fermasse su entrambe sembrerebbe
       funzionare fino al giorno in cui readdir fallisce davvero. */
    check("readdir con un indice negativo da' -1",
          root->ops->readdir(root, -1, nome, &ino) == -1);

    /* Il numero riportato e' quello dell'inode della DIRECTORY del task, non
       l'indice: e' quello che ls stampa accanto al nome. */
    root->ops->readdir(root, 0, nome, &ino);
    check("readdir riporta il numero di inode, non l'indice",
          ino == PROC_INO_TASK(0));
}

/* ---- idx e' una POSIZIONE, non un indice di task ---------------------------- */

/* Il controllo che nessun altro fa, e il difetto che prende e' latente: al boot
   i task 0-3 sono contigui, quindi trattare idx come indice di task funziona per
   caso. Comparirebbe al primo exit di M16, lontano da qui, come una lista
   PLAUSIBILE E INCOMPLETA — il peggior genere di guasto.

   Si costruisce il buco a mano: task_slot da' accesso alla tabella, ed esiste
   per i test da M6a. */
static void test_buco(void)
{
    struct inode *root = procfs_procdir();
    struct task *t1;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;
    int r;

    if (root == NULL)
        return;

    t1 = task_slot(1);

    if (t1 == NULL) {
        check("con un buco nella tabella, readdir non si ferma", 0);
        return;
    }

    /* Si libera lo slot 1 e se ne occupa il 3: la tabella diventa 0, _, _, 3. */
    t1->state = TASK_FREE;
    task_slot(3)->state = TASK_READY;

    memset(nome, 0, sizeof(nome));
    r = root->ops->readdir(root, 0, nome, &ino);
    check("con un buco, la voce 0 e' ancora \"0\"",
          r == 1 && strcmp(nome, "0") == 0);

    /* IL controllo. idx == 1 deve dare "3", non 0: la posizione 1 fra i task
       attivi e' il task 3. */
    memset(nome, 0, sizeof(nome));
    r = root->ops->readdir(root, 1, nome, &ino);
    check("e la voce 1 e' \"3\", cioe' idx salta gli slot liberi",
          r == 1 && strcmp(nome, "3") == 0);

    check("e il suo numero di inode e' quello del task 3",
          ino == PROC_INO_TASK(3));

    check("dopo le due voci attive readdir da' 0",
          root->ops->readdir(root, 2, nome, &ino) == 0);

    /* E lookup e' d'accordo: /proc/1 non c'e' piu', /proc/3 si'. */
    check("lookup non trova piu' /proc/1", cerca(root, "1") == NULL);
    check("e trova /proc/3", cerca(root, "3") != NULL);

    /* Si rimette la tabella come stava, cosi' i gruppi seguenti non eredit'ano
       il buco. */
    t1->state = TASK_READY;
    task_slot(3)->state = TASK_FREE;
}

/* ---- /proc/N e /proc/N/status ---------------------------------------------- */

static void test_status(void)
{
    struct inode *root = procfs_procdir();
    struct inode *d0, *d1, *st, *st1;
    char nome[VFS_NAME_MAX + 1];
    char testo[256];
    uint32_t ino;
    int n;

    if (root == NULL)
        return;

    d0 = cerca(root, "0");
    d1 = cerca(root, "1");

    if (d0 == NULL || d1 == NULL) {
        check("/proc/0/status si trova", 0);
        return;
    }

    st = cerca(d0, "status");

    check("/proc/0/status si trova", st != NULL);

    if (st == NULL)
        return;

    check("ed e' un file, non una directory", st->type == INODE_FILE);
    check("con il numero di inode che gli spetta",
          st->ino == PROC_INO_STATUS(0));

    /* La directory di un task ha UNA voce sola, e nessun altro nome ci passa:
       accettando qualunque nome, cat /proc/0/pippo stamperebbe il contenuto
       giusto sotto il nome sbagliato. */
    check("dentro /proc/0 non c'e' nient'altro", cerca(d0, "altro") == NULL);

    memset(nome, 0, sizeof(nome));
    check("readdir di /proc/0 da' \"status\"",
          d0->ops->readdir(d0, 0, nome, &ino) == 1 &&
          strcmp(nome, "status") == 0 && ino == PROC_INO_STATUS(0));

    check("e una voce sola: idx 1 da' 0",
          d0->ops->readdir(d0, 1, nome, &ino) == 0);

    /* Gli status di due task sono due oggetti distinti, e l'inversa applicata
       dentro task_lookup e' quella giusta: PROC_TASK_DA_DIR e non
       PROC_TASK_DA_STATUS, che differiscono di MAX_TASKS e darebbero un indice
       plausibile e falso. */
    st1 = cerca(d1, "status");
    check("/proc/1/status e' un oggetto diverso da /proc/0/status",
          st1 != NULL && st1 != st);
    check("e ha il numero di inode del task 1",
          st1 != NULL && st1->ino == PROC_INO_STATUS(1));

    /* ---- il contenuto, che nasce nella read ---- */

    n = leggi_tutto(st, testo, sizeof(testo));

    check("si legge, e non e' vuoto", n > 0);
    check("il Pid c'e' e vale 0", strstr(testo, "Pid:    0") != NULL);
    check("lo State c'e'", strstr(testo, "State:  R (") != NULL);
    check("l'Esp c'e', in esadecimale col prefisso",
          strstr(testo, "Esp:    0x") != NULL);

    /* La lettura a pezzi da 7 byte ha attraversato il testo senza saltare
       niente: se read ignorasse l'offset, ogni pezzo ripartirebbe da capo e
       "Pid:" comparirebbe piu' volte. Cercare la SECONDA occorrenza e' il modo
       diretto di chiederlo. */
    check("read rispetta l'offset: \"Pid:\" compare una volta sola",
          strstr(testo, "Pid:") != NULL &&
          strstr(strstr(testo, "Pid:") + 1, "Pid:") == NULL);

    /* NIENTE TAB: '\t' vale 9, e vga_putc gestisce solo >= 32 piu' '\n' e '\b'.
       Un tab funzionerebbe sulla seriale e sparirebbe sul framebuffer, che e' il
       bug del backspace di M7 rifatto. */
    check("l'allineamento e' fatto con spazi, non con tab",
          strchr(testo, '\t') == NULL);

    /* Il task 1 non sta girando, quindi il suo stato e' "ready": e' l'unica
       informazione che genera_status aggiunge rispetto a leggere la struct. */
    n = leggi_tutto(st1, testo, sizeof(testo));
    check("/proc/1/status dice Pid: 1", strstr(testo, "Pid:    1") != NULL);
    check("e lo distingue da chi sta girando",
          strstr(testo, "R (ready)") != NULL);

    /* ---- e la read SENZA STATO ---- */

    /* Rileggere da offset 0 deve ridare la stessa cosa. Con un flag statico che
       alterna "prima riga" e "finito", questa seconda lettura darebbe 0 — e una
       lseek all'indietro sarebbe rotta. */
    memset(testo, 0, sizeof(testo));
    n = leggi_tutto(st, testo, sizeof(testo));
    check("rileggere da capo ridà lo stesso testo",
          n > 0 && strstr(testo, "Pid:    0") != NULL);

    /* Due letture INTRECCIATE su due file diversi. Con uno stato condiviso fra
       tutti gli status, la seconda si spegnerebbe da sola. */
    check("due read intrecciate non si disturbano",
          st->ops->read(st, 0, testo, 4) == 4 &&
          st1->ops->read(st1, 0, testo, 4) == 4 &&
          st->ops->read(st, 0, testo, 4) == 4);

    check("leggere oltre la fine da' 0",
          st->ops->read(st, 9999, testo, 16) == 0);

    /* n == 0: zero byte copiati, e NIENTE toccato. Il primo procfs qui
       ritornava 64 — snprintf con size 0 non termina il buffer, e la strlen che
       ne misurava il risultato camminava fuori. */
    memset(testo, 0x41, sizeof(testo));
    check("read con n == 0 da' 0 e non tocca il buffer",
          st->ops->read(st, 0, testo, 0) == 0 && testo[0] == 0x41);

    /* Sola lettura, e senza un caso speciale: i puntatori sono a zero, che e' la
       convenzione di M8. */
    check("status non si scrive", st->ops->write == NULL);
    check("dentro /proc/0 non si crea niente", d0->ops->create == NULL);
    check("e nemmeno dentro /proc", root->ops->create == NULL);
}

int main(void)
{
    test_radice();
    test_voci();
    test_readdir();
    test_buco();
    test_status();

    if (failures == 0) {
        printf("tutti i test di procfs passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
