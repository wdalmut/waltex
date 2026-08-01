# M9a — VFS, il nucleo: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** contengono
> interfaccia, test, concetti e tranelli, ma il codice lo scrive Walter
> (vedi `CLAUDE.md`). I task **[CLAUDE]** sono infrastruttura.
>
> **Companion:** `2026-07-31-waltex-m9a-funzioni.md` — una scheda per funzione.

**Obiettivo:** un descrittore di file. `vfs_open("/dev/kbd", O_RDONLY)` restituisce
un numero piccolo, e da quel numero si legge senza sapere cosa ci sia dall'altra
parte.

**Architettura:** due tabelle e un risolutore. La tabella dei descrittori è per
task e contiene solo indici; la tabella dei file aperti è globale e tiene la
posizione di lettura; il risolutore cammina un path componente per componente
chiedendo a ogni directory di trovare la successiva.

Il filesystem concreto **non c'è**, ed è deliberato: `vfs_init` riceve l'inode
della radice come argomento. In M9b glielo passerà `devfs`; nei test glielo passa
un albero finto. È lo stesso espediente del sink di eco di `lineedit` — l'unica
ragione per cui questa milestone si prova interamente sull'host.

Circa 200 righe di kernel, e la milestone con più test host del progetto.

## Perché M9 è divisa in due

M9 nello spec è una milestone sola, e da vicino sono diciotto funzioni fra VFS,
filesystem concreto e comandi della shell. Divisa in due ha un punto verde in
mezzo, e soprattutto separa ciò che si prova sull'host da ciò che esiste solo
davanti all'hardware:

- **M9a** (questo piano) — tabelle, risoluzione dei path, le cinque chiamate.
  Logica pura sopra un albero iniettato: **niente QEMU**;
- **M9b** — `devfs`, l'innesto in `kmain`, `ls` e `cat` nella shell. Dentro la VM.

È la stessa divisione di M6a/M6b, e per la stessa ragione: quando qualcosa si
rompe, la superficie di sospetto è metà.

## Due amendamenti allo spec, dichiarati

**Lo spec prevede tre livelli; M9a ne costruisce due.** Il terzo — la cache di
inode con allocazione e refcount — **non serve ancora**, perché in M9b gli inode
sono tre tipi statici dentro `devfs.c` e nessuno viene mai liberato. Serve in
**M11**, quando gli inode arrivano dal disco e sul disco ce ne sono più di quanti
stiano in RAM: lì l'allocazione è forzata, come l'allocatore di memoria in M12.

Conseguenza: `struct inode` e `struct file` **non hanno il campo `refs`**.
Arriverà in **M16**, dove `fork` e `dup` fanno condividere un file aperto fra due
processi — che è il primo momento in cui contare i riferimenti serve a qualcosa.

**Il debito di concorrenza dello spec si sposta di conseguenza.** Non ci sono
refcount da proteggere in M9a. Resta condivisa una cosa: **l'allocazione di uno
slot nella tabella dei file aperti**. Due task che chiamassero `vfs_open` insieme
potrebbero scegliere lo stesso slot.

In M9 quel caso non capita, perché l'unico task che usa il VFS è la shell. Va
protetto comunque, con `irq_save`/`irq_restore` intorno alla ricerca dello slot:
sono tre righe, ed è esattamente il punto che in M16 conterà davvero. Da dire
onestamente: **nessun test in M9a può verificare quella protezione**, perché
serve un secondo task che apra file e non c'è.

## Vincoli globali

Quelli dei blocchi precedenti, più:

- **Solo path assoluti.** Devono cominciare con `/`. Non esiste una directory
  corrente: `chdir` e i path relativi arrivano con i processi, in M14.
- **Niente creazione, niente cancellazione.** `O_CREAT` non è supportato in M9a,
  perché il filesystem di M9b è di sola lettura per costruzione (le directory
  sono generate, non memorizzate). `mkdir` e `unlink` arrivano in M11 con il
  disco.
- **Nessuna allocazione dinamica**: `MAX_OPEN_FILES 32`, `TASK_FDS 8`,
  `VFS_PATH_MAX 64`, `VFS_NAME_MAX 14`.
- **`errno` non esiste ancora**: si ritorna un intero non negativo in caso di
  successo e `-1` in caso di errore. I codici veri arrivano in M14, e la firma
  non cambierà — cambierà solo *quale* negativo.
- **I valori delle costanti sono quelli POSIX**, verificati dagli header
  dell'host e non ricordati: `SEEK_SET 0`, `SEEK_CUR 1`, `SEEK_END 2`,
  `O_RDONLY 0`, `O_WRONLY 1`, `O_RDWR 2`. Costa zero e sta nella direzione del
  vincolo di M14.
- **`read` che ritorna 0 continua a significare «adesso non c'è niente»**, non
  fine del file — è la convenzione di M8 e non cambia. Chi legge un file
  *regolare* sa che è finito confrontando la posizione con `size`; un dispositivo
  a caratteri non finisce mai, e sta al chiamante decidere quando smettere.

## Struttura dei file al termine di M9a

| File | Responsabilità | Chi |
|---|---|---|
| `include/vfs.h` | `struct inode`, `struct inode_ops`, `struct file`, le otto funzioni | CLAUDE |
| `kernel/vfs.c` | le due tabelle, il risolutore, le cinque chiamate | **WALTER** |
| `tests/host/test_vfs.c` | 71 controlli su un albero finto | CLAUDE |
| `tests/host/Makefile` | la regola | CLAUDE |

Il kernel **non cambia**: nessuno chiama ancora `vfs_init`, quindi `make test`
resta verde su tutto il resto per l'intera durata di M9a. È la prima milestone
del progetto interamente fuori da QEMU.

## L'interfaccia

```c
/* ---- include/vfs.h ---- */

#define VFS_PATH_MAX    64
#define VFS_NAME_MAX    14          /* minix v1, e arriva in M11 */
#define MAX_OPEN_FILES  32
#define TASK_FDS         8

/* I valori sono quelli POSIX, verificati e non inventati. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

enum inode_type { INODE_NONE = 0, INODE_FILE, INODE_DIR, INODE_CHARDEV };

struct inode;

/* Le quattro operazioni che un filesystem concreto deve saper fare. Il VFS non
   sa quale filesystem sta parlando: chiama attraverso questi puntatori, ed e'
   tutto il polimorfismo che serve.

   read e write ricevono un OFFSET esplicito e non lo tengono: la posizione vive
   nella tabella dei file aperti, non nell'inode. Due descrittori sullo stesso
   file hanno due posizioni e un solo inode, e questa firma e' cio' che lo rende
   possibile. */
struct inode_ops {
    int (*read   )(struct inode *ino, uint32_t off, void *buf, uint32_t n);
    int (*write  )(struct inode *ino, uint32_t off, const void *buf, uint32_t n);
    int (*lookup )(struct inode *dir, const char *name, struct inode **out);
    int (*readdir)(struct inode *dir, int idx, char *name, uint32_t *ino_out);
};

struct inode {
    uint32_t         ino;        /* identita' dentro il suo filesystem */
    enum inode_type  type;
    uint32_t         size;       /* significativo per INODE_FILE */
    uint16_t         major, minor;   /* validi se INODE_CHARDEV */
    const struct inode_ops *ops;
    void            *priv;       /* il filesystem concreto ci mette quel che vuole */
};

struct file {
    struct inode *inode;         /* 0 = slot libero */
    uint32_t      off;
    int           flags;
};

/* Il punto d'iniezione, e la ragione per cui M9a si prova senza QEMU: la radice
   arriva da fuori. In M9b la passa devfs, nei test un albero finto. */
void vfs_init(struct inode *root);

int vfs_open (const char *path, int flags);
int vfs_read (int fd, void *buf, uint32_t n);
int vfs_write(int fd, const void *buf, uint32_t n);
int vfs_close(int fd);
int vfs_lseek(int fd, int32_t off, int whence);
int vfs_readdir(int fd, int idx, char *name, uint32_t *ino_out);

/* Esposta per la TESTABILITA': provare la funzione piu' densa di M9a attraverso
   vfs_open confonderebbe due lavori — trovare il file e trovare posto nelle
   tabelle — e il primo FAIL non direbbe quale dei due. Stesso motivo di
   task_slot in M6a. In M9b servira' anche a "ls", ma solo se mostra il tipo di
   ogni voce. */
int vfs_resolve(const char *path, struct inode **out);
```

## `struct inode` e `struct file`: cosa sono, e perché sono due

Prima dei livelli, perché è la confusione che questa interfaccia invita.

> **`struct inode` è il file. `struct file` è l'atto di averlo aperto.**

| | `struct inode` | `struct file` |
|---|---|---|
| quante ce ne sono | **una per file**, sempre | **una per `open()`** |
| cosa sa | tipo, dimensione, come si legge (`ops`) | quale inode, e **dove sei arrivato** |
| chi la crea | il filesystem | `vfs_open` |
| vive quanto | il file | dalla `open` alla `close` |

Molti a uno:

```text
struct file A (off = 0) ──┐
                          ├──> struct inode di /a   (size 4, ops)
struct file B (off = 2) ──┘
```

Per questo `struct file` contiene un **puntatore** e non una copia: non possiede
l'inode, lo riferisce. E per questo il campo si chiama `inode` e non `ino` — `ino`
dentro `struct inode` è il **numero** dell'inode, un `uint32_t`, e dare lo stesso
nome al puntatore rende impossibile capire quale dei due si sta leggendo.

Conseguenza pratica su `vfs_init`: svuotare la tabella dei file aperti significa
azzerare il **puntatore** di ogni slot. Il numero `ino` della radice lo decide il
filesystem, e `vfs_init` non lo tocca mai — serve solo a `readdir`, che riporta il
numero di inode di ogni voce come fa `ls -i`.

## I tre livelli, e perché sono tre

È la decisione strutturale della milestone, e in M9a ne esistono due.

| livello | cosa identifica | dove vive | chi lo possiede |
|---|---|---|---|
| descrittore | un indice piccolo | `fds[task][i]` | **per task** |
| file aperto | una posizione di lettura | `files[]` | globale |
| inode | l'identità di un file | dentro il filesystem | globale |

Il caso che spiega perché non possono essere uno:

```text
fd 3 ──┐
       ├──> file aperto A (off = 0)  ──┐
fd 5 ──┘                               ├──> inode di /dev/kbd
                                       │
fd 4 ─────> file aperto B (off = 12) ──┘
```

`fd 3` e `fd 5` sono lo stesso file aperto — è ciò che `dup` produrrà in M16, e
condividono la posizione. `fd 4` è un'altra `open` sullo stesso file: **stesso
inode, posizione propria**. Collassando i livelli, due `open` indipendenti si
ruberebbero la posizione a vicenda — e sarebbe un guasto diagnosticabile mesi
dopo, quando due processi leggono lo stesso file.

In M9a niente fa `dup`, quindi ogni descrittore ha il suo file aperto. La
separazione c'è comunque, perché aggiungerla dopo vorrebbe dire cambiare la forma
di tutte e cinque le chiamate.

## Lo stato, e come si rappresenta «libero»

```text
static struct inode *root;                    /* iniettata da vfs_init      */
static struct file   files[MAX_OPEN_FILES];   /* inode == 0  →  slot libero */
static int           fds[MAX_TASKS][TASK_FDS];/* -1        →  fd libero     */
```

Due scelte da vedere prima di scrivere.

**`files[i].inode == 0` come «libero»** invece di un flag a parte: un inode nullo
non è un file aperto valido, quindi il campo che serve comunque fa già da
marcatore. È lo stesso ragionamento per cui il registro di M8 non ha un flag per
slot e il ring buffer di M5 non ha un contatore.

**La tabella dei descrittori sta in `vfs.c`, non in `struct task`.** In Unix vero
sta nel processo, e sarà il posto giusto quando ci saranno i processi. Adesso
tenerla qui ha due vantaggi concreti: `vfs.c` possiede tutto il proprio stato, e
per provarlo sull'host basta uno stub di `task_current()` invece di mezza
`task.c`. In M16 `fork` dovrà copiare una riga di quella matrice — che è più
facile, non meno, di copiare un campo dentro una struct.

Costo: `8 × 8 × 4` = 256 byte di `.bss`.

## I task

### Task 1 [CLAUDE]: header e test host

Scrivo `include/vfs.h` come sopra, `tests/host/test_vfs.c` con l'albero finto e
i controlli, e la regola nel `tests/host/Makefile`.

**L'albero finto** è il pezzo interessante del test, e vale la pena vederlo prima
di scrivere `vfs.c`, perché è la forma che `devfs` avrà in M9b:

```text
/                dir     lookup: "a" → file, "d" → dir
/a               file    "ciao" (4 byte, letti da un buffer statico)
/d               dir     lookup: "b" → file
/d/b             file    "xy" (2 byte)
/c               chardev finto: read da un contatore, write in un buffer
```

Ogni nodo è uno `struct inode` statico con le sue `inode_ops`. Il VFS non sa che
è finto — è precisamente la proprietà che si sta verificando.

I controlli, per gruppo:

*Risoluzione dei path* (15)

- `/` dà la radice
- `/a` dà il file
- `/d` dà la directory
- `/d/b` dà il file annidato
- un path che non comincia con `/` è rifiutato
- il path vuoto è rifiutato
- `/nonesiste` è rifiutato
- `/a/b` è rifiutato: `a` non è una directory
- `/d/nonesiste` è rifiutato
- `//a` risolve come `/a`: le barre doppie si saltano
- `/d/` dà la directory, non un errore
- `///` dà la radice
- un path più lungo di `VFS_PATH_MAX` è rifiutato
- un nome di componente più lungo di `VFS_NAME_MAX` è rifiutato

*Apertura e chiusura* (14)

- `vfs_open("/a", O_RDONLY)` dà un fd non negativo
- il primo fd è il più basso disponibile
- due `open` danno due fd distinti
- l'fd di una `open` dopo una `close` riusa il numero liberato
- `open` su un path inesistente dà -1
- `open` di una directory riesce — serve a `ls`
- `TASK_FDS` aperture riescono, la successiva dà -1
- `MAX_OPEN_FILES` aperture attraverso task diversi riescono, la successiva dà -1
- `close` di un fd valido dà 0
- `close` di un fd mai aperto dà -1
- `close` due volte sullo stesso fd: la seconda dà -1

*Lettura, scrittura, posizione* (29)

- `read` di 4 byte da `/a` dà 4 e il contenuto è `"ciao"`
- una `read` successiva dà 0: la posizione è arrivata a `size`
- `read` di 2 byte dà 2, e la successiva dà i 2 rimanenti
- la posizione avanza di quanto si è letto davvero
- `read` con `n` = 0 dà 0 e non muove la posizione
- `read` su un fd non aperto dà -1
- `read` su un inode senza `read` nelle sue ops dà -1
- `read` su un fd aperto `O_WRONLY` dà -1
- `write` su un fd aperto `O_RDONLY` dà -1
- `write` su `/c` dà il numero di byte
- `write` su un inode senza `write` dà -1
- `lseek(fd, 0, SEEK_SET)` riporta a zero, e la `read` successiva rilegge tutto
- `lseek(fd, 2, SEEK_SET)` dà 2 e la `read` parte da lì
- `lseek(fd, 1, SEEK_CUR)` è relativo alla posizione corrente
- `lseek(fd, 0, SEEK_END)` dà `size`
- `lseek` con un `whence` sconosciuto dà -1
- `lseek` a una posizione negativa dà -1 e non muove la posizione
- `lseek` oltre la fine è **permesso** e la `read` successiva dà 0

*Indipendenza dei livelli* (7)

- due `open` sullo stesso path danno due posizioni indipendenti: avanzando una,
  l'altra non si muove
- ...e puntano allo **stesso** inode
- una `close` non disturba l'altro descrittore
- gli fd sono **per task**: aperto con `task_current()` che dà 0, l'fd non è
  valido quando `task_current()` dà 1
- lo stesso numero di fd in due task diversi punta a file aperti diversi

*Directory* (6)

- `vfs_readdir` sulla radice dà i nomi in ordine di indice
- oltre l'ultima voce dà 0
- `readdir` su un file, non su una directory, dà -1
- `read` su una directory dà -1
- `readdir` su un fd non aperto dà -1

Settantuno controlli. Serve uno stub di `task_current()` che il test possa
pilotare, ed è l'unica dipendenza esterna di `vfs.c`.

**Verifica:** `make test` resta verde e invariato — 227 host, 58 self-check — e
`make -C tests/host test_vfs` non linka.

---

### Task 2 [WALTER]: `vfs_resolve`

Da sola, perché è la funzione più densa della milestone e ha quindici
controlli suoi. Con lei verde, il resto è impianto.

Il dettaglio sta nella scheda 2 del companion. Qui la forma:

```text
resolve(path):
    se path non comincia con '/'  →  errore
    se path e' piu' lungo di VFS_PATH_MAX  →  errore

    corrente = root
    p = path + 1

    finche' *p != '\0':
        salta le barre                       ← e' cio' che fa funzionare "//a"
        se *p == '\0'  →  esci               ← e' cio' che fa funzionare "/d/"

        copia il componente fino a '/' o '\0' in un buffer locale
        se il componente e' piu' lungo di VFS_NAME_MAX  →  errore

        se corrente non e' una directory  →  errore
        se corrente->ops->lookup e' nullo  →  errore
        se lookup(corrente, nome, &prossimo) fallisce  →  errore

        corrente = prossimo

    *out = corrente
    ritorna 0
```

**Il buffer del componente è locale e limitato**, e non è un dettaglio: un
componente più lungo di `VFS_NAME_MAX` va **rifiutato**, non troncato — troncare
farebbe risolvere due nomi diversi allo stesso file, che è lo stesso errore del
nome del dispositivo in M8.

**Verifica:** i 15 controlli sulla risoluzione passano.

---

### Task 3 [WALTER]: le tabelle — `vfs_open` e `vfs_close`

Le due tabelle e l'allocazione degli slot, più i tre aiutanti `static` che le
altre cinque funzioni riusano: `fd_to_file`, `fd_alloc`, `file_alloc`. Schede 3,
4 e 5 del companion.

`fd_to_file` merita di esistere invece di essere ripetuta: fa **due** controlli —
che l'fd sia nell'intervallo, e che la sua casella non sia libera — e averla una
volta sola è ciò che evita di dimenticarne uno in una delle cinque chiamate. È
precisamente così che si scrivono i bug di sicurezza nei kernel.

Tre cose da tenere insieme:

- **l'fd più basso libero**, non il primo mai usato: è ciò che rende
  riutilizzabile un numero dopo una `close`, e ciò che in M15 farà sì che i primi
  tre descrittori di un processo siano 0, 1 e 2;
- **due tabelle, due esaurimenti diversi**: finiti i descrittori del task, o
  finiti gli slot globali. Sono due `-1` con due cause, e il test li distingue;
- **`irq_save`/`irq_restore` intorno alla ricerca dello slot globale**. In M9
  nessun test lo verifica, perché serve un secondo task che apra file; sono tre
  righe e in M16 sono obbligatorie.

**Verifica:** i 14 controlli su apertura e chiusura passano, più i 7
sull'indipendenza dei livelli.

---

### Task 4 [WALTER]: `read`, `write`, `lseek`, `readdir`

Le quattro che usano le tabelle. Schede 6, 7, 8 e 9.

La cosa che le lega: **nessuna di loro sa cosa sia un file**. Prendono l'fd,
trovano il file aperto, prendono l'inode, e chiamano attraverso `ops`. Se il
puntatore nell'ops è nullo, l'operazione non è supportata e si ritorna `-1` — la
convenzione di M8, ereditata.

E la sola che tiene stato: la posizione. `read` la fa avanzare di **quanto ha
letto davvero**, non di `n` — un dispositivo che dà meno di quanto chiesto non
deve far scivolare la posizione oltre.

**Verifica:** i 29 controlli su lettura/scrittura/posizione e i 6 sulle directory
passano. In tutto **71 su 71**.

---

### Task 5 [CLAUDE]: chiusura di M9a

Aggiorno `CLAUDE.md` con lo stato, i numeri e le convenzioni nuove, e
**propongo** il commit — `M9a: VFS, descrittori di file e risoluzione dei path` —
eseguendolo solo se confermi.

Nessuna riga di `README.md`: M9a non si vede da fuori. Si vedrà in M9b, con
`ls /dev`.

## Come si sbaglia

**Tenere la posizione nell'inode invece che nel file aperto.** Due `open` sullo
stesso file si ruberebbero la posizione. È il motivo per cui `read` e `write`
delle `inode_ops` ricevono un offset esplicito, e il test lo prende aprendo lo
stesso path due volte.

**Far avanzare la posizione di `n` invece di quanto letto.** Su un file va uguale
finché la lettura è piena; su un dispositivo che dà meno, la posizione scivola
oltre e i byte successivi si perdono.

**Restituire l'indice della tabella globale come fd.** Sembra funzionare — è un
numero piccolo — e rompe l'isolamento: due task vedrebbero gli stessi fd, e in
M15 un processo utente riceverebbe un indice in una tabella del kernel.

**Cercare il primo fd libero partendo dall'ultimo usato.** I numeri non
verrebbero riusati, e `TASK_FDS` aperture-e-chiusure alternate esaurirebbero la
tabella.

**Dimenticare che `close` deve liberare due cose:** la casella dell'fd *e* lo
slot globale. Liberare solo la prima esaurisce la tabella dei file aperti dopo
32 `open`, e il sintomo arriva molto dopo la causa.

**Troncare un componente del path troppo lungo** invece di rifiutarlo.

**Non controllare `ops->lookup` prima di chiamarlo.** Un file non ha `lookup`, e
`/a/b` ci arriva.

**Accettare un path relativo.** Senza directory corrente non c'è niente rispetto
a cui risolverlo, e `dev/kbd` senza barra iniziale deve essere un errore, non un
tentativo.

## Lettura di accompagnamento

**xv6**, `file.c` e `fs.c`: la sua `struct file` con `off` e la sua tabella
globale `ftable` sono la stessa cosa che stiamo costruendo, in duecento righe
commentate meglio di quanto riesca a me.

**Linux 0.01**, `fs/namei.c`: `namei()` è `vfs_resolve`, e `fs/open.c` è
`vfs_open`. Interessante che lì la risoluzione e il permesso siano intrecciati,
mentre noi non abbiamo permessi da far valere — è una delle cose che lo spec
mette esplicitamente fuori scope.

**Il Tanenbaum**, capitolo sui filesystem, per la figura dei tre livelli: è
disegnata meglio di qualunque descrizione a parole, e M11 la userà per intero.
