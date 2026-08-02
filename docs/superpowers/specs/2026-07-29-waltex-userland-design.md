# waltex — design del secondo blocco: da kernel a sistema Unix

Data: 2026-07-29
Prerequisito: `2026-07-26-waltex-kernel-design.md`, milestone M1-M6b chiuse.

## Obiettivo

Il primo blocco ha prodotto un kernel: boot, segmentazione, interruzioni, timer,
tastiera, multitasking preemptive. È un kernel nel senso stretto — sa gestire la
macchina — ma dall'esterno non somiglia a niente. Non ha un'interfaccia, non ha
file, non ha un modo di essere usato.

Il secondo blocco aggiunge ciò che rende Linux *Linux* invece di un supervisore
per microcontrollori:

1. **una gestione dei device uniforme**, dove la tastiera non è un caso speciale
   ma un'istanza di una categoria;
2. **un filesystem, e sopra di esso la proprietà che «tutto è un file»** — un
   solo insieme di verbi (`open`, `read`, `write`, `close`) per parlare a un file
   su disco, a una directory e a un pezzo di hardware;
3. **una shell all'accensione**, e alla fine come vero processo utente in ring 3,
   caricato da disco, che parla al kernel solo attraverso syscall.

Il criterio di successo è lo stesso di prima: non «funziona», ma **si capisce
perché funziona, e lo si può dimostrare**.

Il criterio di fine del blocco è preciso: il kernel avvia `init` come PID 1,
`init` esegue `/bin/sh` letto da un filesystem minix su disco, la shell gira in
ring 3, e `fork`/`exec`/`wait` funzionano.

## Scelte fondanti

### 1. Destinazione: Unix completo, non solo la forma

**Scelta:** si arriva a `/bin/sh` come processo utente in ring 3.

**Alternativa scartata:** fermarsi alla forma Unix con tutto in ring 0 — device
layer, VFS, `/dev`, una shell come task del kernel. Sarebbero cinque milestone
invece di dieci e si otterrebbe l'esperienza completa di «tutto è un file».

**Perché:** la separazione dei privilegi non è un dettaglio implementativo, è
*il* concetto che rende un kernel un kernel. Finché la shell gira in ring 0,
«tutto è un file» è una convenzione fra parti dello stesso programma; in ring 3
diventa un confine imposto dall'hardware, e la frase acquista il suo significato
vero: un processo non ha *altro* modo di toccare il mondo.

Le prime cinque milestone sono comunque identiche nelle due strade, quindi la
decisione di proseguire resta rimandabile e niente viene buttato.

### 2. Ordinamento: la forma prima, l'isolamento dopo

**Scelta:** shell → device → VFS → disco → filesystem → memoria → paging →
ring 3 → ELF → fork/exec.

**Alternativa scartata:** l'ordine storico di Linux — memoria → paging → ring 3
→ syscall → device → VFS → disco → filesystem → ELF → shell.

**Perché tre ragioni, e nessuna è la comodità:**

- **La shell in M7 diventa lo strumento di debug delle altre nove.** Oggi per
  ispezionare qualcosa si aggiunge una `kprintf` e si ricompila. Con un prompt si
  ha `peek`, `ps`, `devs`, e in M13 `pgdir` per camminare le tabelle delle pagine
  *mentre* si scrive il paging. Le ore spese in M7 si ripagano in M13.
- **Il VFS viene scritto e usato in ring 0 prima che esista un confine di
  privilegi.** In M14 il chiamante passa dall'altra parte del confine **senza che
  l'interfaccia cambi**, e quella è la dimostrazione che il confine era disegnato
  nel punto giusto. Se il VFS nascesse dopo le syscall, non ci sarebbe modo di
  saperlo.
- **Ogni milestone si vede**, che è la regola che ha fatto funzionare le prime
  sei. L'ordine storico mette quattro milestone completamente invisibili in fila,
  e il paging è il problema di M2 — «se funziona non si vede niente» —
  moltiplicato per quattro, con la tripla fault come unico messaggio d'errore.

**Prezzo accettato:** la shell si scrive «due volte». In realtà è lo stesso file
ricompilato contro stub di syscall invece di chiamate dirette, il che è di nuovo
la prova che l'interfaccia era giusta.

### 3. Filesystem: minix v1

**Scelta:** minix versione 1, magic `0x138F`, nomi da 14 caratteri.

**Alternative scartate:**

- **FAT12/16.** Reale e universale, ma insegna FAT, non la struttura Unix:
  catene di cluster, entry a 12 bit impacchettate a coppie, nomi 8.3, nessun
  inode. E «tutto è un file» ci sta sopra male, perché non ci sono né permessi né
  numeri di inode. Richiederebbe anche di installare `mtools`, assenti.
- **Un formato proprio, `waltexfs`.** Il `mkfs` scritto in Python è metà della
  lezione, ma nessuno strumento standard lo legge: quando il parser mente, l'unico
  riferimento è il proprio `mkfs`, e potrebbe essere lui a sbagliare.

**Perché minix v1:** è la figura canonica del filesystem Unix — superblocco,
bitmap degli inode, bitmap delle zone, inode con puntatori diretti — quella del
Tanenbaum e quella che implementa `fs/` in Linux 0.01, che è già materiale di
lettura del progetto.

Ma la ragione decisiva è **la verificabilità**, ed è stata misurata prima di
scegliere:

```text
mkfs.minix   presente (util-linux 2.39.3)
minix.ko     presente nel kernel host → l'immagine si può montare
```

Il superblocco è leggibile a occhio dall'host:

```text
$ mkfs.minix -1 fs.img && od -A d -t u2 -N 18 -j 1024 fs.img
480  1440  1  1  19  0  7168  4104  5007
 ↑     ↑   ↑  ↑   ↑  ↑     ↑____↑     ↑
inode zone im zm  1ª  log  max_size  magic 0x138F
                 zona dati
```

Un'implementazione di riferimento fidata genera l'immagine, il parser di waltex
la legge, e il disaccordo si localizza con `od`. È esattamente il metodo che il
progetto usa da M4: il PIT verificato contro il CMOS, la GDT riletta con `sgdt`,
il framebuffer riletto dopo la scrittura. **Su un filesystem, il riferimento
indipendente è `mkfs.minix` più `od`.**

**Prezzo accettato:** nomi da 14 caratteri e un paio di stranezze di layout.

### 4. POSIX come vincolo di design, glibc come fuori scope

**Scelta:** il confine delle syscall è quello di Linux i386 — numeri veri,
`errno` come ritorno negativo, `int $0x80`, argomenti in `ebx ecx edx esi edi
ebp`, layout di `struct stat` conforme. Ma non si porta glibc e non si eseguono
i coreutils GNU.

**Perché non glibc — misurato, non stimato.** Un `puts("ciao")` **linkato
staticamente** contro glibc su i386, cioè il caso più favorevole possibile, fa
11 syscall distinte:

```text
write            1     ← il lavoro
brk              5  ┐
set_thread_area  1  │
set_tid_address  1  │
set_robust_list  1  │
rseq             1  ├─ la cerimonia
getrandom        1  │
ugetrlimit       1  │
mprotect         1  │
readlinkat       1  │
statx            1  ┘
```

Dieci syscall di cerimonia per una di lavoro, prima che `main` cominci. E il
problema non è il numero, è *quali*:

- **`set_thread_area`** — glibc mette `errno` in TLS, e su i386 il TLS è `%gs`
  che punta a un descrittore: serve una LDT per thread. Senza, qualunque funzione
  che tocca `errno` fa fault.
- **`getrandom`** — canary dello stack ed entropia di malloc: serve una sorgente
  di entropia nel kernel.
- **`rseq`, `set_robust_list`, `set_tid_address`** — infrastruttura futex, cioè
  metà del threading.
- E **prima** di tutto questo, il vettore ausiliario sullo stack iniziale:
  `AT_PAGESZ`, `AT_PHDR`, `AT_PHNUM`, `AT_ENTRY`, `AT_RANDOM` con sedici byte
  casuali. glibc moderna non parte senza.

`/bin/cat` su un file misura 19 syscall distinte e 48 chiamate; `ls` è molto
peggio — `getdents64`, `ioctl(TCGETS)` per capire se è un terminale,
`/etc/passwd` per i nomi utente, il locale.

Portare glibc è un progetto di **compatibilità ABI Linux**, non di scrittura di
un kernel: è il mestiere che ha richiesto un team a WSL1. Fuori scope.

**Ma la porta si chiude in M14, non alla fine, e tenerla aperta costa zero.**

| Scelta in M14 | Costo | Cosa tiene aperto |
|---|---|---|
| `errno` come ritorno negativo (`-ENOENT`) | zero | la convenzione che ogni libc si aspetta |
| **i numeri di syscall veri di Linux i386** | zero | gli stub di una libc compilata per Linux funzionano non modificati |
| args in `ebx ecx edx esi edi ebp`, `int $0x80` | zero | idem |
| `struct stat` col layout Linux i386 | zero | `fstat` è nei 15 stub di qualunque libc |
| firme POSIX nel VFS | zero | `open(path, flags, mode)`, non `vfs_apri()` |

Scegliere `syscall 4 = write` invece di `syscall 1 = write` non costa nulla oggi
e vale tutto dopo.

Numeri e valori **verificati dagli header dell'host**, non ricordati:

```text
 1 exit    2 fork    3 read    4 write   5 open    6 close   7 waitpid
11 execve 12 chdir  19 lseek  20 getpid 39 mkdir  41 dup    42 pipe
45 brk    54 ioctl 106 stat  108 fstat 141 getdents

ENOENT 2   EBADF 9    ENOMEM 12  EFAULT 14  ENOTDIR 20  EISDIR 21
EINVAL 22  ENFILE 23  EMFILE 24  ENOSPC 28  EROFS 30    ENOSYS 38
```

**La vetta realistica è newlib** (M17, opzionale). newlib vuole ~15 funzioni
stub — `_open _read _write _close _lseek _fstat _sbrk _exit _getpid _isatty
_kill _times _link _unlink _stat` — che sono **precisamente la forma del VFS di
M9**. Niente TLS, niente auxv, niente futex. Con newlib linkato staticamente si
compilano programmi C veri, con `printf` e `malloc`, e girano su waltex.

Oltre newlib: **musl** è più duro ma non assurdo — vuole `set_thread_area`, e lì
serve la LDT, che è la segmentazione di M2 riusata. E oltre musl, un `cat`
statico di coreutils è *plausibile*; `ls` no.

### 5. Nessuna allocazione dinamica fino a M12

**Scelta:** la disciplina degli array statici continua per cinque milestone
ancora. `MAX_DEVICES 16`, `MAX_INODES 64`, `MAX_OPEN_FILES 32`, `TASK_FDS 8`.
L'allocatore arriva in M12.

**Perché:** l'allocatore non è forzato da niente prima del paging, dove le
tabelle vanno allocate a runtime, una per processo, allineate a 4 KB. Metterlo
prima costerebbe due milestone invisibili prima di vedere un prompt, e la
disciplina statica ha la proprietà che il progetto ha scelto da M1: il
fallimento è deterministico e il bilancio della memoria si vede a tempo di link.

Nota: questo **corregge** l'indicazione data alla chiusura di M6b, dove la mappa
di memoria Multiboot era indicata come il prossimo passo naturale. Era giusta
rispetto alla domanda «qual è il prossimo capitolo», sbagliata rispetto
all'obiettivo scelto qui.

## Architettura

### Milestone

```text
M7   shell            editor di riga + tabella comandi          ← un prompt, subito
M8   device layer     struct device, registro, i driver si iscrivono
M9   VFS + devfs      path, inode, tabella fd, open/read/write  ← «tutto è un file»
M10  ATA PIO          driver disco in polling + strato a blocchi
M11  minix v1         superblocco, bitmap, inode — lettura, poi scrittura
M12  memoria          mmap Multiboot, allocatore di pagine, kmalloc
M13  paging           page directory, spazi di indirizzamento per processo
M14  TSS + ring 3     int 0x80, ABI Linux i386, validazione puntatori utente
M15  ELF + exec       loader, build user-space, crt0, stub delle syscall
M16  fork/wait        init come PID 1, /bin/sh in ring 3          ← la vetta
M17  newlib           opzionale: printf e malloc veri
```

Ogni milestone si chiude come le prime sei: kernel che boota, test verdi, un
commit, e il commit lo propone Claude ma lo autorizza Walter.

### File nuovi

```text
kernel/shell.c        editor di riga, tabella dei comandi        [Walter]
kernel/device.c       registro dei device                        [Walter]
kernel/vfs.c          path, inode, tabelle fd e file aperti      [Walter]
kernel/devfs.c        /dev sopra il registro dei device          [Walter]
kernel/minixfs.c      superblocco, bitmap, inode, zone           [Walter]
kernel/pmm.c          allocatore di pagine fisiche su bitmap     [Walter]
kernel/paging.c       page directory, mappature, CR3             [Walter]
kernel/syscall.c      dispatch di int 0x80, validazione puntatori[Walter]
kernel/ata.c          driver ATA PIO                             [Claude]
kernel/elf.c          parsing dei program header                 [Claude]
kernel/tss.c          il TSS e la sua entry nella GDT            [Claude]

include/device.h include/vfs.h include/errno.h include/syscall.h
include/fs.h include/minixfs.h include/pmm.h include/paging.h
include/shell.h include/elf.h include/ata.h                      [Claude]

user/crt0.S           ingresso di un processo utente             [Claude]
user/syscall.S        gli stub delle syscall                     [Claude]
user/sh.c             la shell portata in ring 3                 [Walter]
user/Makefile user/user.ld                                       [Claude]
tools/mkimage.sh      costruisce l'immagine minix del disco      [Claude]
tests/**                                                         [Claude]
```

La divisione è la stessa regola di `CLAUDE.md`, estesa: Walter scrive i moduli
concettualmente rilevanti, Claude il tedio (`ata.c` è una sequenza fissa di
`outb` trascritta da un datasheet, `elf.c` è parsing di struct) e
l'infrastruttura.

### Interfacce pubbliche

Tre header portano il peso del blocco.

**`include/device.h`** (M8). Solo device a caratteri; i blocchi arrivano in M10
con un'interfaccia propria, perché la loro granularità è il settore, non il byte.

```c
#define MAX_DEVICES  16
#define DEV_NAME_MAX 16

struct device {
    char     name[DEV_NAME_MAX];    /* "console", "kbd", "ttyS0" */
    uint16_t major, minor;
    int (*read )(struct device *, void *, uint32_t);
    int (*write)(struct device *, const void *, uint32_t);
    void *priv;
};

int device_register(const struct device *d);   /* -ENOSPC se pieno */
struct device *device_find(const char *name);
struct device *device_by_id(uint16_t major, uint16_t minor);
int device_count(void);
struct device *device_at(int i);               /* per l'enumerazione e i test */
```

Il punto: `vga.c`, `serial.c` e `keyboard.c` **non cambiano la loro logica**.
Aggiungono una `*_register()` chiamata da `kmain` in ordine visibile, come ogni
`*_init()` del progetto. Il device layer è un registro, non un rifacimento.

**`include/vfs.h`** (M9), il cuore del blocco. Firme POSIX da subito, anche se
per ora girano in ring 0.

```c
#define VFS_PATH_MAX    64
#define VFS_NAME_MAX    14      /* minix v1 */
#define MAX_INODES      64
#define MAX_OPEN_FILES  32
#define TASK_FDS         8

enum inode_type { INODE_NONE, INODE_FILE, INODE_DIR, INODE_CHARDEV };

struct inode;

struct inode_ops {
    int (*read   )(struct inode *, uint32_t off, void *, uint32_t);
    int (*write  )(struct inode *, uint32_t off, const void *, uint32_t);
    int (*lookup )(struct inode *, const char *name, struct inode **);
    int (*readdir)(struct inode *, int idx, char *name, uint32_t *ino);
};

struct inode {
    uint32_t ino;
    enum inode_type type;
    uint32_t size;
    uint16_t major, minor;              /* validi se INODE_CHARDEV */
    int refs;
    const struct inode_ops *ops;
    void *priv;                         /* il fs concreto ci mette quel che vuole */
};

struct file { struct inode *ino; uint32_t off; int flags; int refs; };

int vfs_open (const char *path, int flags);
int vfs_read (int fd, void *buf, uint32_t n);
int vfs_write(int fd, const void *buf, uint32_t n);
int vfs_close(int fd);
int vfs_lseek(int fd, int32_t off, int whence);
```

**Tre livelli, uno scopo ciascuno**, ed è la decisione strutturale del blocco:

| Livello | Cosa identifica | Chi lo possiede |
|---|---|---|
| tabella fd | un indice piccolo | **per task** |
| tabella dei file aperti | una posizione di lettura + refcount | globale |
| cache di inode | l'identità di un file | globale |

È la separazione che rende `dup` e `fork` banali in M16: `dup` copia un indice
nella tabella fd, `fork` copia la tabella fd e incrementa i refcount dei file
aperti. Collassando i tre livelli in uno, due processi dopo un `fork`
condividerebbero la posizione di lettura per sbaglio — e sarebbe un bug
diagnosticabile solo mesi dopo.

`devfs` e `minixfs` implementano `inode_ops` e il VFS non sa quale dei due sta
parlando. È il polimorfismo che fa funzionare la frase «tutto è un file»: `cat`
non contiene un caso speciale per `/dev/kbd`.

**`include/syscall.h`** e **`include/errno.h`** (M14): i numeri e i valori
misurati sopra, non inventati.

### Il debito di concorrenza di questo blocco

Va detto qui perché è il punto in cui questo blocco si farà male, e finisce in
`CLAUDE.md` accanto alla regola del ring buffer.

La tabella fd è per task, ma **la tabella dei file aperti e la cache di inode
sono condivise fra task che vengono prelazionati cento volte al secondo**.
`refs++` è read-modify-write: è lo stesso `count++` che nel ring buffer di M5 si
è evitato con la struttura — un solo scrittore per indice — e qui la struttura
non salva, perché tutti i task aprono file per definizione.

Servono `irq_save`/`irq_restore` intorno agli aggiornamenti dei refcount e alla
ricerca di uno slot libero, corti come in `vga_putc` e per la stessa ragione: una
sezione critica lunga fa perdere tick al timer, e il self-check di M4 sulla
frequenza lo noterebbe.

## Verifica

La proprietà interessante di questo blocco è che **la quota testabile sull'host
sale**, non scende. VFS, minixfs e l'allocatore sono logica pura sopra un array
di byte: si compilano con il gcc dell'host e si provano in millisecondi.

| | Sull'host | Dentro la VM |
|---|---|---|
| **M7** shell | editor di riga, backspace, splitting dei comandi | `tests/shell.sh`: inietta tasti con `sendkeys.py`, cerca l'output di un comando |
| **M8** device | registro con device finti | enumerazione: `console`, `kbd`, `ttyS0` presenti con i major/minor attesi |
| **M9** VFS | **decine di test**: risoluzione dei path, allocazione fd, offset dopo `lseek`, codici d'errore — tutto su ramfs | `cat /dev/kbd` |
| **M10** ATA | — | pattern scritto e riletto, **più** la rilettura di un settore scritto dall'host |
| **M11** minixfs | **`minixfs.c` compilato sull'host cammina un'immagine `mkfs.minix` vera**, confrontata con `mount` + `ls` | stessa immagine, stessi risultati |
| **M12** memoria | bitmap dell'allocatore: alloca, libera, riallocazione dello stesso frame | RAM riportata vs il `-m` di QEMU |
| **M13** paging | — | mappa → scrivi → smappa → rimappa altrove → rileggi; e un #PF deliberato con il CR2 giusto |
| **M14** ring 3 | — | **test negativi**: un task utente che tenta `cli`, `outb`, o di scrivere in memoria kernel **deve** prendere #GP |
| **M15-16** | parsing dei program header su un `.o` vero | programma utente che stampa ed esce; `fork` che ritorna due volte con valori diversi; la shell che esegue `/bin/ls` |

Tre righe meritano attenzione.

**M11 è il test più forte del progetto.** Un'implementazione di riferimento
fidata genera l'immagine, il parser di Walter la legge, e il disaccordo si
localizza immediatamente con `od`. Nessun'altra milestone ha un oracolo così
buono.

**M13 si verifica solo rileggendo, come M2.** Le tabelle delle pagine vanno
camminate e verificate *prima* di accendere `CR0.PG`, cioè mentre un errore è
ancora leggibile invece di essere una tripla fault muta. È lo stesso principio per
cui in M6a lo stack falsificato da `task_create` si verifica senza saltarci dentro.

**M14 è l'unica milestone i cui test veri sono negativi.** Il valore del ring 3
non è che il codice utente funzioni: è che *non riesca* a fare certe cose. Un test
che verifica soltanto «il programma utente stampa» passerebbe anche con il
programma in ring 0.

## Innesti sul codice esistente

I dettagli non ovvi, trovati rileggendo il codice e non deducibili dallo spec.

- **Il consumatore della tastiera va spostato, non aggiunto.**
  `keyboard_getchar` ammette un solo consumatore, per la regola del ring buffer.
  Oggi è il ciclo di idle in `kernel/main.c`. In M7 il consumatore diventa la
  shell e quel ciclo deve *smettere* di leggere, non leggere in parallelo.
- **`task_a`/`task_b` vanno messi a tacere, ma `tests/tasks.sh` dipende dal loro
  rumore.** Stampano `A`/`B` in continuazione e renderebbero la shell
  illeggibile. Soluzione: restano, silenziosi, e la shell guadagna un comando
  `spin` che li avvia; `tests/tasks.sh` invia `spin\n` con `sendkeys.py` e poi
  misura transizioni e lunghezza delle corse come adesso. Il test ne esce
  migliore, perché smette di dipendere da un effetto collaterale del kernel.
  Cade anche il filtro `tr -d 'AB'` in `tests/keyboard.sh`.
- **Il Makefile non va toccato per i moduli nuovi**: `CSRC := $(wildcard
  kernel/*.c)` li prende da sé. Va toccato in **M10** (`-drive
  file=build/disk.img,format=raw,if=ide`), in **M11** (la regola che costruisce
  l'immagine con `tools/mkimage.sh`) e in **M15** (la build separata di `user/`).
- **`struct task` cresce** in M13-M16: page directory, pid, parent, tabella fd,
  cwd. Con `MAX_TASKS 8` e stack da 4 KB il `.bss` è già 51 KB, e ogni campo
  nuovo va moltiplicato per otto: resta visibile a tempo di link, che è
  precisamente il punto della disciplina statica.
- **Il TSS entra nella GDT.** `kernel/gdt.c` ha oggi tre descrittori; in M14
  diventano cinque — più codice e dati ring 3 — più il TSS.

## Gestione degli errori

- **Codici negativi, valori Linux.** Ogni funzione che può fallire ritorna
  `-ENOENT` e simili. Non `-1` generico: la ragione del fallimento è
  informazione, e buttarla costa tempo di debug.
- **`assert()` resta sempre attivo** e chiama `panic()`, come da M1.
- **Un puntatore che arriva da ring 3 non è un puntatore.** In M14 ogni
  argomento di syscall che è un indirizzo va validato prima dell'uso: dentro lo
  spazio utente, mappato, e con la lunghezza che non scavalca il confine.
  Saltare questo controllo significa che un processo utente può far scrivere al
  kernel dove vuole, ed è il bug di sicurezza classico dei kernel didattici.
- **`panic` impara a dire quale task.** `task_current()` esiste già; da M13 il
  dump include pid e page directory.

## Dove ci si farà male

In ordine di quanto costano da diagnosticare.

1. **L'istante in cui si accende `CR0.PG`.** Se la mappa identità non copre il
   kernel *e le tabelle stesse*, il prossimo fetch di istruzione fa fault:
   tripla fault, zero messaggi. Mitigazione obbligatoria: camminare le proprie
   tabelle e verificarle prima di scrivere `CR0`.
2. **Il TSS.** Serve per una cosa sola: da dove la CPU prende `esp0` quando un
   interrupt arriva mentre è in ring 3. Sbagliato, e *ogni* interrupt in user
   mode è un double fault. Ed è di nuovo bit-packing in un descrittore GDT, la
   categoria di bug più costosa secondo `CLAUDE.md`.
3. **`execve`**: `argc`, `argv`, `envp` disposti sullo stack utente nell'ordine
   giusto. Sbagliato, e il programma parte e legge spazzatura — cioè fallisce
   lontano dalla causa.
4. **I refcount condivisi del VFS**, per la ragione detta sopra.
5. **`fork`** copia l'intero spazio di indirizzamento, senza copy-on-write.
   Deliberato: più lento, molto più semplice, e il concetto di COW si capisce
   meglio dopo aver visto cosa costa non averlo.

## Fuori scope

Segnali e la loro consegna a user mode, pipe, copy-on-write, `termios` vero, più
filesystem montati insieme, symlink, hard link, *applicazione* dei permessi (i
bit si leggono e si memorizzano, non si fanno valere), SMP, rete, virgola mobile,
glibc, coreutils GNU.

*(**Emendato in M11c: «più filesystem montati insieme» non è più fuori scope.**
La tabella di mount c'è, in `vfs.c`, con `MAX_MOUNTS 4` e la chiave sul puntatore
a inode. Non è stato un ampliamento di ambizione: la scorciatoia prevista —
`minixfs_graft`, uno slot dentro il filesystem — obbligava a modificare il
filesystem che possedeva il punto di innesto per montarci sopra qualunque cosa,
e costava più di quanto risparmiasse.*

*Restano fuori `umount`, che vuole i refcount di M16 per sapere se ci sono file
aperti sotto il punto, e `..` che attraversa il confine all'indietro — `/dev/..`
oggi fallisce — che serve solo quando arriva `chdir` in M14.)*

Un'assenza si sentirà, e va nominata: **non c'è blocking I/O.** Lo scheduler è
round-robin sui task pronti e non esistono `task_block`/`task_wake`, quindi la
shell farà spin su `keyboard_getchar`. Con la prelazione è tollerabile — gli
altri task girano — ma è la ragione per cui le pipe non ci sono, perché `read()`
su una pipe vuota *deve* bloccare, e un `read()` che gira a vuoto non è un
`read()`.

Punto di decisione in **M9**: circa quaranta righe di sleep/wakeup, e le pipe
diventano possibili. La decisione si prende lì, con il VFS davanti agli occhi,
non adesso.

## Lettura di accompagnamento

Per questo blocco cambia il materiale. **Linux 0.01** diventa più pertinente di
prima, non meno: `fs/` implementa esattamente il minix v1 di M11, `fs/exec.c` è
l'`execve` di M15, `kernel/fork.c` è M16. Resta materiale di lettura per
milestone, non una struttura da seguire.

**xv6** del MIT è il riferimento migliore per il VFS a tre livelli di M9 e per
il confine delle syscall di M14: fa le stesse scelte che facciamo qui, scritte da
persone che le hanno insegnate per vent'anni.

Il **Tanenbaum** per il capitolo sui filesystem, che descrive minix v1 perché
minix è suo.

E il **manuale Intel volume 3A**, capitoli 4 (paging) e 7 (task management, per
il TSS), quando OSDev è ambiguo — che in M13 e M14 succede spesso.
