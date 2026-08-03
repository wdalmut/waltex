# M11d — procfs: `/proc` sopra la tabella dei task

> **Per chi esegue:** i passi hanno checkbox (`- [ ]`). I file di Walter
> (`kernel/procfs.c`, `kernel/memory.c`) sono descritti per **contratto,
> struttura e trappole**, non con l'implementazione: è la regola di `CLAUDE.md`.
> I file di Claude (`include/*.h`, `tests/**`, `tools/**`, `kernel/main.c`,
> `kernel/selftest.c`) hanno il codice per intero.

**Obiettivo:** un terzo filesystem — `/proc` — che non ha né un disco né un
driver sotto, ma **genera** il proprio contenuto dalla tabella dei task, così che
`cat /proc/1/status` dica cosa sta facendo un processo.

**Architettura:** `procfs.c` riempie le stesse caselle di `inode_ops` che
riempiono `devfs.c` e `minixfs.c`, e `kmain` lo aggancia con
`vfs_mount("/proc", procfs_root())`. Tre livelli di inode, tutti statici:
la radice, una directory per slot di task, una foglia `status` per slot.

**Tecnologie:** C freestanding i386, QEMU, i test host già in piedi.

## Il vero scopo, che non è `/proc`

**M11c ha una tabella di mount con un solo cliente, ed è lo stesso di prima.**
devfs era già agganciato in M11a con la graft; il meccanismo nuovo non è ancora
stato messo alla prova da un filesystem che non esisteva quando è stato scritto.

Questa milestone è quella prova, e la misura è **binaria**:

```bash
git diff --stat kernel/vfs.c kernel/minixfs.c     # deve essere VUOTO
```

Se montare un terzo filesystem costa anche una sola riga in `vfs.c` o in
`minixfs.c`, il taglio di M11c non era nel punto giusto. È il controllo più
importante del piano, ed è nel Task 3 Passo 8.

Il secondo scopo è che `procfs.c` è **il primo filesystem che Walter scrive dal
primo all'ultimo carattere**: `devfs.c` è suo ma è nato con l'aiuto dello spec di
M9b, `minixfs.c` l'ho scritto io su sua richiesta.

## Vincoli globali

Da `docs/superpowers/specs/2026-07-29-waltex-userland-design.md` e da
`CLAUDE.md`. Valgono per ogni task.

- **Niente libc.** Tipi da `include/types.h`. Non esiste `sprintf`, e questa
  milestone ne ha bisogno: vedi il Task 1.
- **Nessuna allocazione dinamica fino a M12.** Tutti gli inode di procfs sono
  array statici dimensionati su `MAX_TASKS` (8).
- **`assert()` sempre attivo**, nessun `NDEBUG`.
- **Un puntatore nullo in `inode_ops` significa «non supportata»**, non
  «errore». procfs lascia a zero `write` e `create`: `/proc` è di sola lettura, e
  `mkdir /proc/x` deve fallire da sé senza un caso a parte.
- **`read` ritorna quanti byte ha copiato davvero, e zero NON è fine del file**
  — su un file regolare però lo è, ed è così che `shell_cat` esce.
- **Errori come ritorno negativo.** `-1`, come tutto il VFS attuale.
- **`kprintf` scrive su VGA e COM1.** I test leggono la seriale.
- **Dopo ogni modifica al kernel si esegue `make test`**, non `make`.
- **Una milestone alla volta.** Solo `status`. Niente `/proc/meminfo` (non c'è
  ancora un allocatore), niente `/proc/N/maps` (non c'è ancora il paging),
  niente `/proc/self`.

## La forma di `/proc`

```text
/proc                    directory: le voci sono gli INDICI dei task attivi
/proc/0                  directory: una voce sola, "status"
/proc/0/status           foglia: testo GENERATO dalla tabella dei task
```

Nessuno dei tre memorizza le proprie voci: le genera quando gliele si chiede. È
la stessa proprietà di devfs, e qui è ancora più netta — in devfs le voci sono
il registro dei dispositivi, che almeno esiste; qui il *contenuto* non esiste da
nessuna parte finché qualcuno non lo chiede.

Contenuto di `/proc/0/status`, tre righe:

```text
Pid:    0
State:  R (running)
Esp:    0x00107f5c
```

`R (running)` per il task corrente, `R (ready)` per gli altri. La lettera è
quella di Linux — `R` copre sia «sta girando» sia «è pronto a girare» — e la
parola fra parentesi è la distinzione che `ps` già mostra.

**Le colonne si allineano con SPAZI, non con `\t`.** Linux usa il tab, noi non
possiamo: `'\t'` vale 9, e `vga_putc` gestisce solo `>= 32` più `'\n'` e `'\b'`.
Un tab sparirebbe sul framebuffer e funzionerebbe sulla seriale — che è
esattamente il bug del backspace di M7, riscoperto da capo.

## Le quattro decisioni, e perché

### 1. L'indice del task sta in `ino`, non in `priv`, e MAI un `struct task *`

`status_read` è **una funzione sola che serve otto file**, distinti solo da quale
task descrivono. È la situazione che il commento di M9b nelle `inode_ops`
descrive:

> `dir` e `ino` nelle `inode_ops` sembrano inutili e non lo sono. In `devfs` ogni
> directory ha la sua funzione, quindi `dir` è ignorato; `minix_lookup` sarà una
> sola funzione per migliaia di directory e `dir->ino` sarà l'unica cosa che le
> distingue.

Qui è la terza volta, e stavolta con otto invece di migliaia. Quindi `ino` porta
l'informazione e `priv` resta a zero.

**E soprattutto: mai un `struct task *`.** In M16 i task escono e i loro slot
vengono riusati; un inode che tiene un puntatore riporterebbe in silenzio su un
processo diverso. È la conclusione **opposta** a quella della tabella di mount di
M11c, ed è giusto che sia opposta: uno slot della cache di inode non viene mai
riciclato, uno slot della tabella dei task sì.

### 2. Il buffer di generazione è LOCALE, non statico

`read` prende un **offset**, quindi il testo va generato per intero e poi se ne
consegna una fetta. Il buffer dove generarlo:

- **statico** costa 128 byte una volta sola, ma è condiviso fra task
  prelazionati cento volte al secondo. Fra il «genero» e il «copio» ci sta un
  tick, e due `cat /proc/*/status` in parallelo si mescolerebbero;
- **locale** costa 128 byte sullo stack di chi chiama — che ha 4096 byte — e non
  è condiviso con nessuno.

Locale, e non serve nessuna sezione critica. Linux risolve lo stesso problema con
`seq_file`, che vale la pena guardare **dopo** averci sbattuto contro.

### 3. `size` vale 0, ed è corretto

La dimensione non si conosce prima di aver generato il testo. Zero funziona, ed è
**verificato**: `vfs_read` non consulta `size` ([vfs.c:451-476](../../../kernel/vfs.c#L451-L476))
e `shell_cat` esce quando `read` ritorna 0. È anche quello che fa Linux — `ls -l
/proc/1/status` mostra 0 byte.

### 4. Il punto di mount esiste sul disco

`mkdir proc` in `tools/mkminix.sh`, come `dev` in M11c, e per la stessa ragione:
in Unix `mount` copre una directory che c'è già. Va creata **dopo** `dev`, così
`dev` resta l'inode 8 e non si spostano i valori appena assestati.

## Struttura dei file

| file | chi | cosa |
|---|---|---|
| `include/memory.h` | Claude | dichiarazione di `utoa` |
| `kernel/memory.c` | **Walter** | `utoa` |
| `include/procfs.h` | Claude | il contratto: `procfs_init`, `procfs_root` |
| `kernel/procfs.c` | **Walter** | tutto il filesystem — **file NUOVO** |
| `kernel/main.c` | Claude | `procfs_init` + `vfs_mount("/proc", …)` |
| `kernel/selftest.c` | Claude | i controlli che esistono solo nella VM |
| `tools/mkminix.sh` | Claude | `mkdir proc` |
| `tests/data/minix.img` | Claude | rigenerata e ricommittata |
| `tests/host/test_memory.c` | Claude | i controlli di `utoa` |
| `tests/host/test_procfs.c` | Claude | **file NUOVO**, il grosso della verifica |
| `tests/host/Makefile` | Claude | il nuovo binario |
| `tests/host/test_minixfs.c` | Claude | i conteggi della radice, ancora |
| `tests/shell.sh` | Claude | `ls /proc` e `cat /proc/0/status` |
| `CLAUDE.md` | Claude | `procfs.c` nella lista dei file di Walter, M11d chiusa |

`Makefile` **non** va toccato: `CSRC := $(wildcard kernel/*.c)` prende
`procfs.c` da sé.

## Il conto dei test, atteso

| | prima | dopo |
|---|---|---|
| host | 429 | ~455 (+6 `utoa`, +20 procfs) |
| self-check | 109 | 114 |
| marker | 9 | 9 |
| script nella VM | 6 | 6 |

Numeri **da misurare a fine milestone**:

```bash
make -C tests/host -s run | grep -cE "ok +--"
```

---

## Task 1 — `utoa`, perché i nomi delle directory sono numeri

`readdir` di `/proc` deve produrre `"0"`, `"1"`, … dagli indici dei task, e
`status_read` deve mettere dei numeri dentro un testo. In freestanding non c'è
`sprintf`, e `kprintf` scrive sui sink — non in un buffer.

**File:**
- Modifica: `include/memory.h`
- Modifica: `kernel/memory.c`
- Test: `tests/host/test_memory.c`

**Interfacce:**
- Consuma: niente.
- Produce: `int utoa(uint32_t v, unsigned base, char *buf, int max)`. Il Task 2
  la usa in `proc_readdir` e in `status_read`.

- [ ] **Passo 1: la dichiarazione**

In `include/memory.h`, dopo `strchr`:

```c
/* Scrive v in base "base" dentro buf, terminata da '\0'. Ritorna quanti
   caratteri ha scritto SENZA contare il terminatore, oppure -1 se non ci sta o
   se la base non e' supportata.

   max conta il buffer INTERO, terminatore compreso: utoa(7, 10, buf, 2) riesce
   e scrive "7\0", utoa(70, 10, buf, 2) fallisce.

   Basi ammesse: 10 e 16. In base 16 le cifre sono minuscole e NON c'e' nessun
   prefisso "0x" — se lo si vuole lo mette il chiamante, che e' l'unico a sapere
   se sta scrivendo un indirizzo o un numero.

   Solo SENZA SEGNO, e il nome lo dice. Non e' una limitazione da togliere piu'
   avanti: chi ha un numero con segno sa di averlo, e passare un -1 come
   uint32_t e ottenere 4294967295 e' il comportamento giusto, non un bug.

   Su -1 il buffer NON viene toccato — la convenzione di lookup e di
   shell_parse_hex: su ogni uscita d'errore il chiamante tiene quello che aveva.

   Duplica la logica di put_uint dentro kprintf.c, e la duplicazione e'
   deliberata: put_uint scrive su un sink, questa in un buffer, e unificarle
   vorrebbe dire far formattare kprintf dentro un buffer intermedio — un cambio
   piu' grande di quello che questa milestone vuole. Il giorno che si facesse,
   si sistemerebbe anche il debito di M1 per cui put_uint tratta la base 10 come
   con segno e non sa stampare oltre 2^31. */
int utoa(uint32_t v, unsigned base, char *buf, int max);
```

- [ ] **Passo 2: scrivere i test (falliscono)**

In `tests/host/test_memory.c`, un gruppo nuovo, e in `main()` la chiamata.

```c
/* ---- M11d: utoa --------------------------------------------------------------

   Sei controlli, e i tre che contano sono lo zero, il troppo-corto e
   l'inversione delle cifre. Le cifre si generano dall'ULTIMA — si divide per la
   base — quindi escono al contrario, ed e' il bug che si fa scrivendo questa
   funzione: "123" diventa "321", che e' leggibile, ordinato e sbagliato. */
static void test_utoa(void)
{
    char buf[16];

    memset(buf, 'Z', sizeof(buf));
    check("utoa(0) scrive \"0\", non la stringa vuota",
          utoa(0, 10, buf, sizeof(buf)) == 1 && strcmp(buf, "0") == 0);

    check("utoa(7) in decimale",
          utoa(7, 10, buf, sizeof(buf)) == 1 && strcmp(buf, "7") == 0);

    /* L'inversione: con le cifre al contrario questo darebbe "321". */
    check("utoa(123) non ha le cifre al contrario",
          utoa(123, 10, buf, sizeof(buf)) == 3 && strcmp(buf, "123") == 0);

    check("utoa in base 16, minuscolo e senza prefisso",
          utoa(0x1a2b, 16, buf, sizeof(buf)) == 4 &&
          strcmp(buf, "1a2b") == 0);

    /* Il numero piu' grande che esista in 32 bit: dieci cifre piu' il
       terminatore. Con un buffer da 11 ci sta esattamente. */
    check("utoa(4294967295) ci sta in undici byte",
          utoa(4294967295u, 10, buf, 11) == 10 &&
          strcmp(buf, "4294967295") == 0);

    /* Il rifiuto, e il buffer non si tocca. max conta il terminatore, quindi
       due byte bastano per una cifra sola e non per due. */
    memset(buf, 'Z', sizeof(buf));
    check("utoa rifiuta un buffer troppo corto senza scriverci",
          utoa(70, 10, buf, 2) == -1 && buf[0] == 'Z');
}
```

E in `main()`, prima del riepilogo:

```c
    test_utoa();
```

- [ ] **Passo 3: verificare che falliscano**

```bash
make -C tests/host test_memory
```

Atteso: `undefined reference to 'utoa'`. Un errore di link **è** un test che
fallisce, ed è lo stesso che in M7 ha scoperto lo `strcmp` mancante.

- [ ] **Passo 4: implementare in `kernel/memory.c` — file di Walter**

La forma, che è la sola cosa non ovvia:

```text
se base non e' 10 ne' 16   -> -1
se max < 2                 -> -1        (una cifra piu' il terminatore)

genera le cifre in un buffer TEMPORANEO, dall'ultima alla prima:
    ripeti:  cifra = v % base;  v /= base
    finche' v != 0
    (il "ripeti ... finche'" e non il "finche' ... ripeti" e' cio' che fa
     uscire "0" invece della stringa vuota per v == 0)

se le cifre non ci stanno in max - 1   -> -1, senza toccare buf

copia il temporaneo in buf AL CONTRARIO, e chiudi con '\0'
```

Il buffer temporaneo vuole **10 byte**: `4294967295` è il massimo in base 10, e
in base 16 sono 8 cifre. Dieci basta per entrambe.

- [ ] **Passo 5: verificare che passino**

```bash
make -C tests/host -s run
```

Atteso: `test_memory` verde con sei controlli in più, tutto il resto invariato.

- [ ] **Passo 6: il kernel compila**

```bash
make test
```

Atteso: verde e invariato. `utoa` esiste e nessuno la chiama ancora.

- [ ] **Passo 7: commit**

```bash
git add include/memory.h kernel/memory.c tests/host/test_memory.c
git commit -m "M11d: utoa, formattazione di interi in un buffer"
```

---

## Task 2 — `procfs.c`

Tutto sull'host. Il filesystem esiste e nessuno lo monta: è deliberato, la
meccanica si prova per intero prima che qualcosa dipenda da lei — la stessa
disciplina del Task 1 di M11c.

**File:**
- Crea: `include/procfs.h`
- Crea: `kernel/procfs.c`
- Crea: `tests/host/test_procfs.c`
- Modifica: `tests/host/Makefile`

**Interfacce:**
- Consuma: `utoa(uint32_t, unsigned, char *, int)` dal Task 1;
  `task_slot(int)`, `task_current()`, `MAX_TASKS`, `enum task_state` da
  `include/task.h`; `struct inode`, `struct inode_ops` da `include/vfs.h`.
- Produce: `void procfs_init(void)` e `struct inode *procfs_root(void)`. Il
  Task 3 li consuma da `kernel/main.c`.

- [ ] **Passo 1: `include/procfs.h`**

```c
#ifndef WALTEX_PROCFS_H
#define WALTEX_PROCFS_H

#include "types.h"
#include "vfs.h"

/* Il filesystem di M11d, e il primo che non ha NIENTE sotto: non un disco come
   minix, non un registro come devfs. Il contenuto di /proc/0/status non esiste
   da nessuna parte finche' qualcuno non lo chiede — lo GENERA la read.

     /proc              directory: le voci sono gli indici dei task attivi
     /proc/<N>          directory: una voce sola, "status"
     /proc/<N>/status   foglia: tre righe costruite dalla tabella dei task

   Di sola lettura: write e create restano a zero nelle inode_ops, quindi
   "mkdir /proc/x" fallisce da se' — la convenzione di M8, per la quarta volta.

   L'INDICE DEL TASK STA IN ino, E NON C'E' NESSUN struct task * DA NESSUNA
   PARTE. In M16 i task escono e i loro slot vengono riusati: un inode che
   tenesse un puntatore riporterebbe in silenzio su un processo diverso. E' la
   conclusione opposta a quella della tabella di mount in vfs.c, ed e' giusto che
   sia opposta — uno slot della cache di inode non viene mai riciclato, uno slot
   della tabella dei task si'. */

/* Numerazione degli inode. Esplicita e in un posto solo, perche' e' un PATTO fra
   lookup, readdir e read: la prima li assegna, le altre due ci risalgono. Lo
   stesso genere di patto degli array paralleli di M9b, ma piu' stretto — qui e'
   una formula sola in un file solo, invece di due strutture che devono restare
   allineate. */
#define PROC_INO_ROOT           1u
#define PROC_INO_TASK(n)        (2u + (uint32_t)(n))
#define PROC_INO_STATUS(n)      (2u + MAX_TASKS + (uint32_t)(n))

/* E le inverse. Chi le usa deve GIA' sapere che tipo di inode ha in mano:
   applicare quella sbagliata da' un indice plausibile e falso. */
#define PROC_TASK_DA_DIR(ino)     ((int)((ino) - 2u))
#define PROC_TASK_DA_STATUS(ino)  ((int)((ino) - 2u - MAX_TASKS))

/* Riempie gli inode statici. NON legge la tabella dei task: quella si consulta
   a ogni lookup e a ogni readdir, perche' i task nascono e — da M16 — muoiono.
   Chiamarla una volta e memorizzare chi c'era darebbe un /proc congelato al
   boot.

   Niente vincolo d'ordine rispetto a task_init, per la stessa ragione. */
void procfs_init(void);

/* L'inode di /proc, da passare a vfs_mount.

   Ritorna 0 se procfs_init non e' ancora stata chiamata: con una radice fatta di
   zeri il VFS avrebbe type INODE_NONE e ops nullo, quindi il mount fallirebbe
   senza dire perche'. Stessa scelta di devfs_root() e minixfs_root(). */
struct inode *procfs_root(void);

#endif
```

- [ ] **Passo 2: `tests/host/Makefile`**

Aggiungere `test_procfs` a `BINS` e la regola:

```make
# task.c si linka VERO, non stubbato: procfs legge la tabella dei task
# attraverso task_slot e task_current, ed e' logica pura — test_task lo linka
# gia' cosi' da M6a. memory.c serve per utoa e per le funzioni di stringa.
#
# E' l'ultimo filesystem del blocco che si prova interamente sull'host, e la
# ragione e' la stessa di minixfs: procfs non tocca nessun hardware. Qui e'
# ancora piu' netto — non c'e' nemmeno un disco finto da costruire, perche' la
# sua unica sorgente di verita' e' un array in .bss.
test_procfs: test_procfs.c ../../kernel/procfs.c ../../kernel/task.c \
             ../../kernel/memory.c
	$(CC) $(CFLAGS) -o $@ $^
```

- [ ] **Passo 3: scrivere `tests/host/test_procfs.c` (fallisce)**

```c
/* I test di procfs, e sono il controllo che M11c ha lasciato scoperto: un
 * filesystem scritto DOPO la tabella di mount, che non condivide una riga con
 * quelli che c'erano.
 *
 * Girano interamente sull'host perche' procfs non tocca nessun hardware: la sua
 * unica sorgente di verita' e' la tabella dei task, che e' un array in .bss.
 * Non serve nemmeno il disco finto che test_minixfs.c deve costruire.
 *
 * Nota su cosa NON si prova qui: che /proc sia montato. Quello e' un fatto del
 * kernel, e lo prendono i self-check dentro la VM. */

#include <stdio.h>
#include <string.h>

#include "procfs.h"
#include "task.h"
#include "vfs.h"
#include "memory.h"

static int failures;

static void check(const char *name, int ok)
{
    printf("%s -- %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

/* I task finti. task.c e' quello vero, quindi si costruisce la tabella con le
   sue funzioni invece di scriverci dentro a mano: cosi' il test esercita la
   stessa struttura che esercitera' il kernel. */
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
   fermandosi sullo zero. Il pezzo e' 7 byte — un numero che non divide niente
   apposta, cosi' ogni lettura cade in un punto diverso del testo generato. */
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

/* ---- i controlli --------------------------------------------------------- */

static void test_radice(void)
{
    struct inode *root;

    check("procfs_root e' nulla prima di procfs_init", procfs_root() == NULL);

    procfs_init();
    root = procfs_root();

    check("procfs_root non e' nulla dopo procfs_init", root != NULL);

    if (root == NULL)
        return;

    check("la radice e' una directory", root->type == INODE_DIR);
    check("la radice e' l'inode 1", root->ino == PROC_INO_ROOT);

    /* size 0 e' CORRETTO, non una dimenticanza: la dimensione di un file
       generato non si conosce prima di generarlo, vfs_read non consulta size, e
       shell_cat esce sullo zero. E' anche cio' che fa Linux — ls -l su
       /proc/1/status mostra 0. */
    check("la radice ha size 0", root->size == 0);

    /* Due chiamate danno lo STESSO puntatore: e' la proprieta' che rende
       possibile la tabella di mount, che indicizza per puntatore. */
    check("due procfs_root() danno lo stesso puntatore",
          procfs_root() == root);
}

static void test_voci(void)
{
    struct inode *root = procfs_root();
    struct inode *d0, *d1, *nulla;
    int t;

    if (root == NULL)
        return;

    /* task_init registra il contesto corrente come task 0. Da qui in poi la
       tabella ha almeno un task pronto. */
    task_init();

    check("/proc/0 esiste appena c'e' un task", cerca(root, "0") != NULL);

    d0 = cerca(root, "0");

    check("ed e' una directory", d0 != NULL && d0->type == INODE_DIR);
    check("con il numero di inode che gli spetta",
          d0 != NULL && d0->ino == PROC_INO_TASK(0));

    /* Uno slot libero NON deve comparire. E' il controllo che distingue "genero
       le voci dalla tabella" da "ho otto directory fisse". */
    nulla = cerca(root, "7");
    check("uno slot libero non compare in /proc", nulla == NULL);

    t = task_create(task_di_prova);
    check("task_create riesce", t > 0);

    d1 = cerca(root, "1");
    check("e la sua directory compare SUBITO, senza rifare procfs_init",
          d1 != NULL && d1->type == INODE_DIR);

    /* I nomi che non sono numeri, e i numeri fuori intervallo. */
    check("un nome non numerico non si risolve", cerca(root, "pippo") == NULL);
    check("un indice oltre MAX_TASKS non si risolve", cerca(root, "99") == NULL);
    check("il nome vuoto non si risolve", cerca(root, "") == NULL);
}

static void test_readdir(void)
{
    struct inode *root = procfs_root();
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;
    int r, n, visti;

    if (root == NULL)
        return;

    /* readdir e lookup devono descrivere lo STESSO insieme: e' la lezione di
       M11a, dove un innesto che compariva solo in lookup dava un /dev che cat
       apriva e ls non mostrava. */
    visti = 0;

    for (n = 0; n < MAX_TASKS + 2; n++) {
        memset(nome, 0, sizeof(nome));
        r = root->ops->readdir(root, n, nome, &ino);

        if (r != 1)
            break;

        if (cerca(root, nome) == NULL) {
            printf("FAIL -- readdir da' \"%s\", che lookup non trova\n", nome);
            failures++;
            return;
        }

        visti++;
    }

    check("readdir e lookup della radice sono d'accordo", visti > 0);

    /* Due task attivi: il task 0 di task_init e quello di task_create. */
    check("readdir elenca esattamente i task attivi", visti == 2);

    /* Lo zero significa "le voci sono finite", ed e' distinto dal -1. */
    check("oltre l'ultima voce readdir da' 0",
          root->ops->readdir(root, visti, nome, &ino) == 0);

    check("readdir con un indice negativo da' -1",
          root->ops->readdir(root, -1, nome, &ino) == -1);
}

static void test_status(void)
{
    struct inode *root = procfs_root();
    struct inode *d0, *st;
    char testo[256];
    int n;

    if (root == NULL)
        return;

    d0 = cerca(root, "0");

    if (d0 == NULL) {
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

    /* La directory di un task ha UNA voce sola. */
    check("dentro /proc/0 non c'e' nient'altro", cerca(d0, "altro") == NULL);

    n = leggi_tutto(st, testo, sizeof(testo));

    check("si legge, e non e' vuoto", n > 0);
    check("contiene il Pid", strstr(testo, "Pid:") != NULL);
    check("e il Pid e' 0", strstr(testo, "Pid:    0") != NULL);
    check("contiene lo State", strstr(testo, "State:") != NULL);
    check("contiene l'Esp in esadecimale", strstr(testo, "Esp:    0x") != NULL);

    /* NIENTE TAB: '\t' vale 9, e vga_putc gestisce solo >= 32 piu' '\n' e
       '\b'. Un tab funzionerebbe sulla seriale e sparirebbe sul framebuffer —
       il bug del backspace di M7, rifatto. */
    check("l'allineamento e' fatto con spazi, non con tab",
          strchr(testo, '\t') == NULL);

    /* La lettura a pezzi da 7 byte ha attraversato il testo senza saltare
       niente: se read ignorasse l'offset, ogni pezzo ripartirebbe da capo e
       "Pid:" comparirebbe piu' volte. Cercare la SECONDA occorrenza e' il modo
       diretto di chiederlo. */
    check("read rispetta l'offset: \"Pid:\" compare una volta sola",
          strstr(testo, "Pid:") != NULL &&
          strstr(strstr(testo, "Pid:") + 1, "Pid:") == NULL);

    /* Oltre la fine si ottiene 0, non spazzatura. */
    check("leggere oltre la fine da' 0",
          st->ops->read(st, (uint32_t)n + 100, testo, 16) == 0);

    /* Sola lettura, e senza un caso speciale: i puntatori sono a zero. */
    check("status non si scrive", st->ops->write == NULL);
    check("dentro /proc non si crea niente", d0->ops->create == NULL);
}

int main(void)
{
    test_radice();
    test_voci();
    test_readdir();
    test_status();

    if (failures == 0) {
        printf("tutti i test di procfs passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
```

> **Nota su `strstr`/`strrchr`:** questo file gira in ambiente **hosted** e
> include `<string.h>`, quindi le prende dalla libc di sistema — come
> `test_minixfs.c` fa già con `strcmp` e `memcmp`. Non finiscono nel kernel.

- [ ] **Passo 4: verificare che fallisca**

```bash
make -C tests/host test_procfs
```

Atteso: `procfs.h: No such file or directory` finché il Passo 1 non è fatto,
poi `undefined reference to 'procfs_init'`.

- [ ] **Passo 5: implementare `kernel/procfs.c` — file di Walter**

Sette funzioni, e nessuna supera le venticinque righe. Il dettaglio funzione per
funzione — cosa ritorna in ogni caso, come ci si sbaglia, quale test lo prende —
sta in `2026-08-02-waltex-m11d-funzioni.md`.

Lo scheletro:

```text
static struct inode ino_root;
static struct inode ino_task[MAX_TASKS];
static struct inode ino_status[MAX_TASKS];
static int pronto;                      /* procfs_init e' stata chiamata */

static int task_attivo(int i);          /* slot valido e state != TASK_FREE */
static int genera_status(int i, char *buf, int max);   /* il testo, e la sua lunghezza */

static int root_lookup (dir, name, out);
static int root_readdir(dir, idx, name, ino_out);
static int task_lookup (dir, name, out);      /* accetta solo "status" */
static int task_readdir(dir, idx, name, ino_out);
static int status_read (ino, off, buf, n);

static const struct inode_ops ops_root   = { .lookup = root_lookup,  .readdir = root_readdir  };
static const struct inode_ops ops_taskdir= { .lookup = task_lookup,  .readdir = task_readdir  };
static const struct inode_ops ops_status = { .read   = status_read };

void procfs_init(void);
struct inode *procfs_root(void);
```

**Inizializzatori designati** nelle tre tabelle, non la forma posizionale: da
M11b `inode_ops` ha cinque campi, e la forma posizionale farebbe protestare
`-Wextra` su ogni tabella incompleta. I campi assenti restano a zero
**dichiarandolo**.

Le tre cose da non sbagliare, in ordine di costo:

1. **`root_readdir` deve saltare gli slot liberi, e `idx` è una POSIZIONE fra
   quelli attivi, non un indice di task.** Con i task 0 e 3 attivi, `idx == 1`
   deve dare `"3"`. Scambiare i due significa che `ls /proc` mostra buchi e che
   l'enumerazione si ferma al primo slot libero.
2. **`status_read` deve rispettare `off`.** Genera il testo intero, poi copia da
   `off` per al massimo `n` byte, e ritorna quanti ne ha copiati — zero se `off`
   è oltre la fine. Ignorandolo, ogni chiamata riparte da capo e `cat` stampa
   il primo pezzo all'infinito.
3. **Il buffer di `genera_status` è LOCALE a `status_read`.** Statico sarebbe
   condiviso fra task prelazionati cento volte al secondo, e due `cat` in
   parallelo si mescolerebbero.

- [ ] **Passo 6: verificare che passi**

```bash
make -C tests/host -s run
```

Atteso: `test_procfs` verde con venti controlli, tutti gli altri binari
invariati.

- [ ] **Passo 7: il kernel compila**

```bash
make test
```

Atteso: verde e invariato. `procfs.c` viene compilato e linkato — `CSRC` lo
prende da sé — e nessuno lo chiama.

Vale la pena guardare quanto è cresciuto il `.bss`: 17 `struct inode` sono meno
di mezzo kilobyte, e il bilancio si legge a tempo di link.

```bash
size build/waltex.elf
```

- [ ] **Passo 8: commit**

```bash
git add include/procfs.h kernel/procfs.c tests/host/test_procfs.c \
        tests/host/Makefile
git commit -m "M11d: procfs, /proc generato dalla tabella dei task"
```

---

## Task 3 — montare `/proc`

**File:**
- Modifica: `tools/mkminix.sh`
- Modifica: `tests/data/minix.img` (rigenerata, binaria)
- Modifica: `kernel/main.c`, `kernel/selftest.c`
- Modifica: `tests/host/test_minixfs.c`, `tests/shell.sh`

**Interfacce:**
- Consuma: `procfs_init()`, `procfs_root()` dal Task 2; `vfs_mount(const char *,
  struct inode *)` da M11c.
- Produce: niente.

- [ ] **Passo 1: `/proc` sull'immagine**

In `tools/mkminix.sh`, dentro il blocco `sudo sh -c`, **dopo** la riga di `dev`:

```sh
    mkdir -p '$MNT/proc'
```

E nel commento sopra il blocco, sotto la voce `dev/`:

```sh
#   proc/        directory VUOTA, e il secondo punto di mount. Vale parola per
#                parola quello che vale per dev/, e insieme provano che il
#                meccanismo di M11c regge per piu' di un cliente. Creata DOPO
#                dev/ cosi' dev resta l'inode 8 e i controlli host su quel
#                numero non si spostano.
```

- [ ] **Passo 2: rigenerare l'immagine e verificarla**

```bash
./tools/mkminix.sh tests/data/minix.img
fsck.minix -f tests/data/minix.img
od -An -c -j$((7 * 1024)) -N 160 tests/data/minix.img
```

Atteso: `fsck` esce **0**, e il dump della radice mostra nove voci con `proc`
in fondo e l'inode **9**. Il conto di coerenza di M11a deve tornare **identico**
— `2+1+1 = 4`, `96*32 = 3` blocchi, `4+3 = 7 = s_firstdatazone`: una directory
in più non cambia il numero di inode dichiarati.

- [ ] **Passo 3: aggiornare i conteggi in `tests/host/test_minixfs.c`**

Quattro valori, tutti misurati al passo precedente:

```c
    check("la radice misura 144 byte, cioe' nove voci", root->size == 144);
```

```c
    static const char *attese[] = {
        ".", "..", "hello.txt", "etc", "grande.txt", "enorme.txt", "vuoto.txt",
        "dev", "proc"
    };
```

Il ciclo diventa `for (n = 0; n < 9; n++)`, i due messaggi dicono `nove voci`,
e `readdir(root, 9, ...)` è quello che deve dare 0.

E il controllo del primo inode libero, che in M11c era già caduto una volta per
la stessa ragione:

```c
    /* Era 8 in M11b, 9 in M11c, 10 adesso: se li sono presi "dev" e poi "proc".
       E' l'unico controllo della suite che guarda un numero di inode ASSOLUTO
       invece di un nome, e per questo l'unico che si accorge di ogni directory
       nuova sull'immagine. */
    check("con un numero di inode non ancora usato", nuovo->ino == 10);
```

- [ ] **Passo 4: `kmain` monta anche `/proc` — `kernel/main.c`**

Aggiungere l'include di `procfs.h` e, dentro il ramo del mount riuscito:

```c
   if (minixfs_init(ata_drive(1)) == 0) {
      vfs_init(minixfs_root());

      /* DUE mount, e il secondo e' il punto di M11d. procfs non esisteva quando
         la tabella di mount e' stata scritta, non condivide una riga con devfs,
         e agganciarlo non ha richiesto di toccare ne' vfs.c ne' minixfs.c —
         che e' esattamente cio' che M11c prometteva. */
      procfs_init();

      if (vfs_mount("/dev", devfs_devdir()) == 0 &&
          vfs_mount("/proc", procfs_root()) == 0) {
         kprintf("waltex: radice minix su hdb, /dev e /proc montate\n");
      } else {
         kprintf("waltex: radice minix su hdb, un mount e' fallito\n");
      }
   } else {
      vfs_init(devfs_root());
      kprintf("waltex: nessun filesystem su hdb, radice su devfs\n");
   }
```

**`procfs_init()` va prima di `vfs_mount`**, perché `procfs_root()` ritorna 0
finché non è stata chiamata e il mount rifiuterebbe una radice nulla. Nessun
altro vincolo d'ordine: procfs **non** legge la tabella dei task
all'inizializzazione.

- [ ] **Passo 5: il marker in `tests/smoke.sh`**

`"waltex: radice minix su hdb, /dev montata"` diventa:

```sh
"waltex: radice minix su hdb, /dev e /proc montate"
```

- [ ] **Passo 6: i self-check — `kernel/selftest.c`**

Aggiungere `#include "procfs.h"` e, dopo `check_minix`, una funzione nuova
chiamata da `selftest_run()`:

```c
/* --- M11d: procfs ------------------------------------------------------------

   I venti controlli host provano il filesystem; questi cinque provano le due
   cose che esistono solo qui — che sia MONTATO, e che i task che descrive siano
   quelli veri del kernel invece di una tabella costruita da un test. */

static void check_procfs(void)
{
    struct inode *ino, *dir;
    char buf[128];
    int fd, r;

    /* L'identita' del puntatore, come per /dev: "proc" esiste sull'immagine
       come directory vuota, quindi risolverlo riuscirebbe anche a mount
       fallito. */
    report("/proc e' esattamente l'inode di procfs",
           vfs_resolve("/proc", &dir) == 0 && dir == procfs_root());

    /* Il task 0 e' kmain, che sta girando adesso: c'e' di sicuro. */
    report("/proc/0 si risolve ed e' una directory",
           vfs_resolve("/proc/0", &ino) == 0 && ino->type == INODE_DIR);

    report("/proc/0/status si risolve ed e' un file",
           vfs_resolve("/proc/0/status", &ino) == 0 &&
           ino->type == INODE_FILE);

    fd = vfs_open("/proc/0/status", O_RDONLY);

    if (fd < 0) {
        report("vfs_open(\"/proc/0/status\") riesce", 0);
        return;
    }

    r = vfs_read(fd, buf, sizeof(buf) - 1);
    buf[r > 0 ? r : 0] = '\0';

    report("si legge attraverso il VFS, e non e' vuoto", r > 0);

    /* Il contenuto e' GENERATO adesso da questa CPU: la prima riga dice il pid
       del task che sta eseguendo questo self-check. */
    report("e comincia con \"Pid:\"",
           r > 4 && buf[0] == 'P' && buf[1] == 'i' && buf[2] == 'd' &&
           buf[3] == ':');

    vfs_close(fd);
}
```

E la chiamata in `selftest_run()`, **dopo** `check_minix_write()`.

- [ ] **Passo 7: `tests/shell.sh`**

Dopo i controlli di `ls /dev`, i comandi e i controlli nuovi.

I comandi, accanto agli altri `sendkeys.py`:

```sh
# M11d: il terzo filesystem, e il secondo mount. La stessa ls e lo stesso cat
# di M9b, senza una riga di modifica, su un filesystem che genera il proprio
# contenuto invece di leggerlo.
python3 tests/sendkeys.py "$MON" l s spc slash p r o c ret
python3 tests/sendkeys.py "$MON" c a t spc slash p r o c slash 0 slash s t a t u s ret
```

E i controlli, accanto agli altri:

```sh
# La radice del disco ora ha due punti di mount: entrambi vengono dall'immagine
# e vengono COPERTI, che e' la differenza fra montare e creare.
if fra_prompt "ls /" | grep -qE "(^| )proc$"; then
    echo "ok   -- ls / elenca proc"
else
    echo "FAIL -- ls / non ha elencato proc"
    FALLITI=1
fi

# "0" e' il task kmain, che sta girando: se ls /proc fosse vuota, procfs
# starebbe elencando slot fissi invece di leggere la tabella dei task.
if fra_prompt "ls /proc" | grep -qE "(^| )0$"; then
    echo "ok   -- ls /proc elenca il task 0"
else
    echo "FAIL -- ls /proc non ha elencato il task 0"
    FALLITI=1
fi

# E il contenuto, che non esiste da nessuna parte finche' cat non lo chiede.
if fra_prompt "cat /proc/0/status" | grep -q "Pid:"; then
    echo "ok   -- cat /proc/0/status genera il testo"
else
    echo "FAIL -- cat /proc/0/status non ha prodotto Pid:"
    FALLITI=1
fi
```

`slash` è già usato da `tests/shell.sh` per `ls /`, quindi è verificato. **Il
nome del tasto per la cifra `0` non lo è**: `ret`, `spc` e `backspace` sono
verificati contro il monitor di QEMU, `enter` e `space` vengono rifiutati in
silenzio, e le cifre non sono mai state provate. Si controlla prima di scrivere
il resto del test:

```bash
python3 tests/monitor.py "$MON" 'sendkey 0'
```

Un nome rifiutato non dà errore, dà un test che fallisce senza dire perché — è
la ragione per cui in M7 la lista dei nomi accettati è finita in `CLAUDE.md`.

- [ ] **Passo 8: IL CONTROLLO DELLA MILESTONE**

```bash
git diff --stat kernel/vfs.c kernel/minixfs.c
```

**Deve essere vuoto.** Se non lo è, si guarda *cosa* ha richiesto la modifica
prima di farla: è la prova che il taglio di M11c non era completo, e vale più di
qualunque test verde.

- [ ] **Passo 9: `make test`**

```bash
make test
```

Atteso: tutto verde. In particolare `tests/minixwrite.sh` invariato, con
`fsck.minix -f` che esce 0 su un'immagine che ora ha due directory in più.

E la prova che montare non scrive:

```bash
mkdir -p /tmp/m && sudo mount -o loop,ro build/minix.img /tmp/m
ls -la /tmp/m/proc; sudo umount /tmp/m; rmdir /tmp/m
```

Atteso: `/tmp/m/proc` **vuota**, come `/tmp/m/dev`.

- [ ] **Passo 10: la prova a mano**

Nella VM:

```text
waltex> ls /proc
waltex> cat /proc/0/status
waltex> cat /proc/1/status
```

*(**Correzione, misurata provandolo:** qui avevo scritto di lanciare `spin` e
aspettarsi due directory in più. È sbagliato. `demo_tasks_init()` crea i due task
di prova **al boot**, silenziosi; `spin` non li crea, accende solo la loro
stampa. Quindi `ls /proc` mostra già quattro voci — 0 kmain, 1 shell, 2 e 3 i
task di prova — e dopo `spin` sono le stesse quattro, sommerse da `A` e `B`.*

*La liveness la prova invece la coppia di controlli automatici, che è più forte:
i self-check girano **prima** di `task_init` e pretendono `/proc` **vuota**,
`tests/shell.sh` gira al prompt e la pretende **piena**. La stessa domanda in due
istanti, e un `procfs_init` che si fosse memorizzato la tabella cadrebbe sul
primo.)*

Ciò che vale la pena guardare con gli occhi è il campo `State`: `/proc/1/status`
letto dalla shell dice `R (running)`, perché è la shell stessa a leggerlo, mentre
gli altri task dicono `R (ready)`. **Non lo prende nessun test** — sull'host il
task corrente è sempre lo 0 e la distinzione si vede solo con più task veri.

- [ ] **Passo 11: commit**

```bash
git add tools/mkminix.sh tests/data/minix.img kernel/main.c kernel/selftest.c \
        tests/smoke.sh tests/shell.sh tests/host/test_minixfs.c
git commit -m "M11d: /proc montato, e il secondo cliente della tabella di mount"
```

---

## Task 4 — la documentazione

**File:**
- Modifica: `CLAUDE.md`

**Interfacce:** nessuna.

- [ ] **Passo 1: `procfs.c` nella lista dei file che Claude non scrive**

Nel blocco «secondo blocco» della prima lista, accanto a `devfs.c` e
`minixfs.c`:

```text
kernel/shell.c      kernel/device.c     kernel/vfs.c      kernel/devfs.c
kernel/minixfs.c    kernel/procfs.c     kernel/pmm.c      kernel/paging.c
kernel/syscall.c
```

- [ ] **Passo 2: la sezione di M11d**

Dopo il blocco di M11c, prima di «Stato dei test»:

```markdown
M11d chiusa: **procfs**, e il vero scopo non era `/proc`.

**Era la prova che mancava a M11c.** Fino a qui la tabella di mount aveva un
cliente solo, ed era lo stesso di prima: devfs, che era già agganciato con la
graft. `procfs` è il primo filesystem scritto DOPO il meccanismo, e la misura è
binaria — `git diff --stat kernel/vfs.c kernel/minixfs.c` dev'essere vuoto.
Lo è.

- **il contenuto NON ESISTE finché qualcuno non lo chiede.** minixfs serve byte
  che stanno sul disco, devfs byte che arrivano da un driver, procfs li
  **genera**. E `read` prende un offset, quindi si genera il testo intero e se ne
  consegna una fetta. Linux ha inventato `seq_file` per questo problema, e vale
  la pena guardarlo dopo averci sbattuto contro;
- **il buffer di generazione è LOCALE, non statico.** Statico costerebbe 128 byte
  una volta sola, ma fra il «genero» e il «copio» ci sta un tick del timer: due
  `cat /proc/*/status` in parallelo si mescolerebbero. Sullo stack non è
  condiviso con nessuno, e non serve nessuna sezione critica;
- **`size` vale 0, ed è corretto.** La dimensione non si conosce prima di
  generare. `vfs_read` non consulta `size` — verificato — e `shell_cat` esce
  quando `read` ritorna 0. È anche quello che fa Linux: `ls -l /proc/1/status`
  mostra 0 byte;
- **l'indice del task sta in `ino`, e non c'è nessun `struct task *`.** È la
  terza volta che `dir->ino` serve a distinguere directory servite dalla stessa
  funzione — la prima nota sta nelle `inode_ops` da M9b. E il divieto del
  puntatore è la conclusione **opposta** a quella della tabella di mount, ed è
  giusto che sia opposta: uno slot della cache di inode non viene mai riciclato,
  uno slot della tabella dei task sì, e in M16 comincia a succedere;
- **niente `\t`.** Linux allinea `/proc/N/status` con i tab; `'\t'` vale 9, e
  `vga_putc` gestisce solo `>= 32` più `'\n'` e `'\b'`. Un tab funzionerebbe
  sulla seriale e sparirebbe sul framebuffer — il bug del backspace di M7,
  rifatto per la seconda volta;
- **`idx` in `readdir` è una POSIZIONE, non un indice di task.** Con i task 0 e 3
  attivi, `idx == 1` dà `"3"`. Confonderli fa fermare `ls /proc` al primo slot
  libero.

**`utoa` è nata qui**, in `memory.c`: i nomi delle directory di `/proc` sono
numeri e in freestanding non c'è `sprintf`. Duplica la logica di `put_uint` in
`kprintf.c`, che scrive su un sink invece che in un buffer — unificarle vorrebbe
dire far formattare `kprintf` dentro un buffer intermedio, e sistemerebbe anche
il debito di M1 per cui `put_uint` tratta la base 10 come con segno.

Il controllo che nessun test automatico fa: nella VM, `spin` e poi `ls /proc`.
Devono comparire due directory in più, senza che nessuno abbia rifatto
`procfs_init`. È la differenza fra generare le voci e memorizzarle.
```

- [ ] **Passo 3: la roadmap e lo stato**

```text
M11d procfs           /proc sopra la tabella dei task, secondo mount   CHIUSA
```

E la riga di stato resta su M12 come prossima.

- [ ] **Passo 4: i numeri dei test, misurati**

```bash
make -C tests/host -s run | grep -cE "ok +--"
```

e la stessa cosa sul log seriale per i self-check. Se non tornano con gli attesi
(~455 e 114), **vince la misura**.

- [ ] **Passo 5: commit**

```bash
git add CLAUDE.md
git commit -m "docs: M11d, procfs e la prova che mancava a M11c"
```

---

## Cosa resta fuori, dichiarato

- **`/proc/meminfo`**, che vuole M12: senza un allocatore non c'è niente da
  riportare.
- **`/proc/N/maps`**, che vuole M13. Sarà per il paging quello che `peek` è per
  la memoria — lo strumento con cui si cammina una tabella mentre la si scrive —
  ed è la ragione più forte per avere procfs *adesso* invece che dopo.
- **`/proc/self`**, che vuole i pid di M16. Oggi «il processo corrente» è
  `task_current()`, e un link simbolico non esiste nel VFS.
- **`/proc/N/cmdline` e `/proc/N/fd/`**, che vogliono M15 e la tabella dei
  descrittori per processo esposta.
- **Scrivere in `/proc`.** In Linux `/proc/sys` si scrive; qui `write` e `create`
  restano nulli, che è la convenzione di M8 e costa zero righe.

## Autoverifica di questo piano

**Copertura.** Le tre cose che la milestone produce — `utoa`, il filesystem, il
mount — hanno un task ciascuna, più la documentazione. Il controllo che dà senso
a tutto (`git diff --stat` vuoto su `vfs.c` e `minixfs.c`) è un passo esplicito,
il Task 3 Passo 8, invece di una speranza.

**Segnaposto.** I test host sono scritti per intero, i self-check pure, e le tre
trappole di `procfs.c` sono nominate con il sintomo che producono invece che con
un «gestire i casi limite».

**Coerenza dei nomi.** `procfs_init`, `procfs_root`, `PROC_INO_ROOT`,
`PROC_INO_TASK(n)`, `PROC_INO_STATUS(n)`, `PROC_TASK_DA_DIR`,
`PROC_TASK_DA_STATUS`, `utoa(v, base, buf, max)` sono gli stessi in ogni task.
`task_slot(int)` e `task_current(void)` sono quelli veri di `include/task.h`,
verificati.

**Un rischio noto.** Il Task 3 tocca `tests/data/minix.img` per la seconda volta
in due milestone, e ogni volta i quattro conteggi della radice si spostano. Se
capitasse una terza, varrebbe la pena farli derivare da una costante invece che
scriverli a mano — ma con due directory nuove in tutto il progetto, tre righe di
`check` sono più leggibili di un `#define`.
