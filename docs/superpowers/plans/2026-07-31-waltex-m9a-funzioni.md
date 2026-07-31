# M9a — Cosa deve fare ogni funzione

Companion di `2026-07-31-waltex-m9a-vfs.md`. Sei voci per scheda, come in M8:
**compito**, **ritorna** (ogni caso), **non deve**, **chi la chiama** (adesso e in
quale milestone futura), **come si sbaglia**, **test**.

## Indice

1. [`vfs_init`](#1-vfs_init)
2. [`vfs_resolve`](#2-vfs_resolve) — la più densa
3. [gli aiutanti interni](#3-gli-aiutanti-interni)
4. [`vfs_open`](#4-vfs_open)
5. [`vfs_close`](#5-vfs_close)
6. [`vfs_read`](#6-vfs_read)
7. [`vfs_write`](#7-vfs_write)
8. [`vfs_lseek`](#8-vfs_lseek)
9. [`vfs_readdir`](#9-vfs_readdir)

---

## Lo stato

```text
static struct inode *root;                     /* iniettata da vfs_init     */
static struct file   files[MAX_OPEN_FILES];    /* ino == 0  →  slot libero  */
static int           fds[MAX_TASKS][TASK_FDS]; /* -1        →  fd libero    */
```

`files[i].ino == 0` fa da marcatore di slot libero perché un inode nullo non è un
file aperto valido: il campo che serve comunque fa già quel lavoro. Stesso
ragionamento del registro di M8 senza flag per slot, e del ring buffer di M5
senza contatore.

---

## 1. `vfs_init`

```c
void vfs_init(struct inode *root);
```

**Compito.** Ricevere la radice, svuotare le due tabelle: ogni `files[i].ino` a
zero, ogni `fds[t][i]` a −1.

**Ritorna.** Niente.

**Non deve.** Costruire la radice — la riceve. È tutta la ragione per cui M9a si
prova senza QEMU: il filesystem concreto è un argomento, non una dipendenza.

**Attenzione al valore di «libero» nella tabella dei descrittori:** è **−1**, non
zero. Zero è un fd valido, e sarà `stdin` in M15. Un ciclo che azzerasse la
matrice invece di riempirla di −1 renderebbe ogni descrittore «già aperto sul
file aperto 0», e il guasto si vedrebbe alla prima `read` su un fd mai aperto.

È il rovescio esatto della situazione di `files[]`, dove zero **è** libero — due
convenzioni opposte a due righe di distanza, e la sola ragione è che in un caso
zero è un indice valido e nell'altro un puntatore non valido.

**Chi la chiama.** In M9b: `kmain`, dopo `devfs_init()` che costruisce la radice.
Nei test: ogni gruppo di controlli, per ripartire pulito.

**Come si sbaglia.** Azzerando `fds` invece di metterlo a −1. E chiamandola
prima di aver costruito la radice, che in M9b vorrebbe dire passarle un puntatore
nullo.

**Test.** Tutti, indirettamente: ogni gruppo comincia chiamandola.

---

## 2. `vfs_resolve`

```c
int vfs_resolve(const char *path, struct inode **out);
```

La funzione più densa di M9a: quattordici controlli guardano lei.

**Compito.** Camminare il path componente per componente, chiedendo a ogni
directory di trovare la successiva, e restituire l'inode finale.

**Ritorna.**

| caso | valore |
|---|---|
| risolto | `0`, e `*out` è l'inode |
| il path non comincia con `/` | `-1` |
| path vuoto | `-1` |
| più lungo di `VFS_PATH_MAX` | `-1` |
| un componente più lungo di `VFS_NAME_MAX` | `-1` |
| un componente non esiste | `-1` |
| un componente intermedio non è una directory | `-1` |
| la directory non ha `lookup` nelle sue ops | `-1` |

In tutti i casi di errore **`*out` non va toccato**: la stessa regola di
`shell_parse_hex`, e per la stessa ragione — chi chiama deve poter tenere il
valore che aveva.

**Non deve.** Accettare un path relativo. Non esiste una directory corrente
rispetto a cui risolverlo: `dev/kbd` senza barra iniziale è un errore, non un
tentativo. La `cwd` arriva in M14 con i processi.

**Non deve** troncare un componente troppo lungo: due nomi diversi risolverebbero
allo stesso file, che è l'errore del nome di dispositivo in M8.

**I tre casi delle barre**, che sono quelli che si dimenticano:

```text
"/"        →  la radice, senza nessun lookup
"//a"      →  come "/a": le barre consecutive si saltano
"/d/"      →  la directory d, non un errore
```

Vengono tutti e tre da una sola scelta: **saltare le barre in cima al ciclo**, e
uscire se dopo averle saltate si è arrivati al terminatore. Scritto così non
serve nessun caso speciale.

**Chi la chiama.** `vfs_open`, e in M9b il comando `ls` — che ha bisogno
dell'inode e non di un descrittore. È esposta nell'header per quel motivo e
perché essendo la più densa conviene provarla direttamente invece che attraverso
`vfs_open`.

**Come si sbaglia.**

- **il buffer del componente non limitato.** Va copiato in un array locale da
  `VFS_NAME_MAX + 1`, controllando la lunghezza *mentre* si copia. Copiare prima
  e controllare dopo è già troppo tardi;
- **non controllare che l'inode corrente sia una directory** prima di chiedergli
  un `lookup`. Su `/a/b` con `a` file, `a` non ha `lookup` e ci si arriva;
- **non controllare che `ops->lookup` sia non nullo.** Anche una directory
  potrebbe non averlo, e la convenzione di M8 dice che nullo significa «non
  supportata»;
- **trattare `"/"` come «un componente vuoto»** e chiedere alla radice un
  `lookup` di `""`.

**Test.** I 14 della sezione «risoluzione dei path».

---

## 3. Gli aiutanti interni

Non stanno nell'header: sono `static` in `vfs.c` e servono a non ripetere lo
stesso codice in cinque funzioni.

```c
static struct file *fd_to_file(int fd);   /* 0 se l'fd non e' aperto */
static int  fd_alloc(int file_index);     /* l'fd piu' basso libero, o -1 */
static int  file_alloc(void);             /* uno slot globale libero, o -1 */
```

**`fd_to_file`** fa **due** controlli, e sono due perché ci sono due tabelle: che
`fd` sia nell'intervallo `0 … TASK_FDS-1`, e che `fds[task][fd]` non sia −1.
Salta il primo e un `fd` di −5 legge fuori dalla matrice; salta il secondo e un
descrittore mai aperto restituisce il file aperto zero.

Averla una volta sola è ciò che evita di dimenticare uno dei due controlli in una
delle cinque chiamate — che è precisamente come si scrivono i bug di sicurezza
nei kernel.

**`fd_alloc`** cerca **il più basso** libero, non il primo mai usato. Due
ragioni: dopo una `close` il numero torna disponibile, e in M15 i primi tre
descrittori di un processo saranno 0, 1 e 2 perché nessun altro li ha presi.

**`file_alloc`** cerca uno slot con `ino == 0`, e qui va la sezione critica:

```text
flags = irq_save()
    cerca lo slot e marcalo occupato
irq_restore(flags)
```

Due task che cercassero insieme troverebbero lo stesso slot. In M9 non capita —
usa il VFS solo la shell — e nessun test può verificarlo, perché servirebbe un
secondo task che apre file. Sono tre righe e in M16 sono obbligatorie.

**Marcare lo slot dentro la sezione critica**, non dopo: trovarlo e uscire, per
poi riempirlo, lascia la finestra aperta esattamente dove si voleva chiuderla.

---

## 4. `vfs_open`

```c
int vfs_open(const char *path, int flags);
```

**Compito.** Risolvere il path, prendere uno slot globale, prendere un fd, e
legare i due.

**Ritorna.**

| caso | valore |
|---|---|
| aperto | l'fd, `>= 0` |
| il path non si risolve | `-1` |
| descrittori del task esauriti | `-1` |
| slot globali esauriti | `-1` |

**L'ordine delle operazioni conta**, e non per eleganza: se prendi lo slot globale
e poi scopri che i descrittori del task sono finiti, **devi liberare lo slot**.
Uno slot occupato da nessuno non si libera mai più, e dopo 32 `open` fallite la
tabella è morta senza che niente lo dica.

Il modo di non sbagliare è risolvere **prima** tutto ciò che può fallire senza
effetti, e allocare per ultimo — oppure ricordarsi di disfare. La prima è più
difficile da dimenticare.

**Non deve.** Applicare i permessi: non ci sono. `flags` si **memorizza** perché
`read` e `write` lo consultino, non perché apra o neghi l'accesso al file.

**Non deve** supportare `O_CREAT`: il filesystem di M9b è di sola lettura per
costruzione, perché le sue directory sono generate e non memorizzate. La
creazione arriva in M11 con il disco.

**Aprire una directory deve riuscire.** Serve a `ls`: `vfs_readdir` prende un fd,
e quell'fd viene da qui.

**Chi la chiama.** In M9b `ls` e `cat`. In **M14** la syscall `open`, numero 5. In
**M15** `execve`, per leggere il file da eseguire.

**Come si sbaglia.**

- **non liberare lo slot globale** quando l'fd non si trova;
- **restituire l'indice della tabella globale** come fd. Sembra funzionare, è un
  numero piccolo, e rompe l'isolamento fra task: in M15 un processo utente
  riceverebbe un indice in una tabella del kernel;
- **azzerare `off`?** Sì, va azzerato: lo slot potrebbe venire da un file chiuso
  prima e contenere la sua posizione.

**Test.** Gli 11 su apertura e chiusura, più i 5 sull'indipendenza dei livelli.

---

## 5. `vfs_close`

```c
int vfs_close(int fd);
```

**Compito.** Liberare **due** cose: la casella dell'fd e lo slot globale.

**Ritorna.** `0` se chiuso, `-1` se l'fd non era aperto.

**Le due cose sono il punto.** Liberare solo la casella dell'fd fa sembrare tutto
corretto: le `open` continuano a riuscire finché i descrittori bastano, e la
tabella globale si esaurisce silenziosamente dopo 32 aperture totali. Il sintomo
— «`open` fallisce e non capisco perché» — arriva molto dopo la causa, ed è il
guasto più costoso da diagnosticare di questa milestone.

**Non deve.** Chiamare niente nelle `inode_ops`: non c'è nulla da svuotare,
perché non c'è nessuna cache di scrittura. In M11 con il disco ci sarà.

**Chiudere due volte deve dare `-1`**, non 0: la seconda `close` su un fd già
chiuso è un bug del chiamante e va detto.

**Chi la chiama.** `ls` e `cat` in M9b. In **M14** la syscall `close`, numero 6.
In **M16** `exit`, che chiude tutti i descrittori del processo che termina.

**Come si sbaglia.** Liberando una tabella sola. E liberando lo slot globale
*prima* di aver verificato che l'fd fosse valido — allora `close(-1)` libererebbe
lo slot di qualcun altro.

**Test.** Tre controlli su `close`, più quello che fa 32 aperture: senza il
rilascio dello slot globale, quel controllo cade.

---

## 6. `vfs_read`

```c
int vfs_read(int fd, void *buf, uint32_t n);
```

**Compito.** Trovare il file aperto, chiamare `ops->read` con la **posizione
corrente**, e far avanzare la posizione di quanto è stato letto.

**Ritorna.**

| caso | valore |
|---|---|
| letto | quanti byte, da `0` a `n` |
| fd non aperto | `-1` |
| l'inode non ha `read` nelle ops | `-1` |
| l'fd è aperto in sola scrittura | `-1` |
| è una directory | `-1` |
| `ops->read` ha fallito | `-1` |

**Zero non è un errore e non è fine del file:** è la convenzione di M8 e non
cambia. Su un file regolare significa che la posizione ha raggiunto `size`; su un
dispositivo a caratteri significa «adesso non c'è niente». Chi legge distingue i
due casi guardando il tipo dell'inode, non il valore di ritorno.

**La posizione avanza di quanto è stato letto DAVVERO, non di `n`.** Su un file
pieno è la stessa cosa; su un dispositivo che dà tre byte quando gliene chiedi
otto, avanzare di otto salterebbe cinque byte che nessuno ha visto.

**Non deve.** Interpretare il contenuto. Non deve azzerare il resto del buffer se
ha letto meno di `n` — chi chiama sa quanti byte sono arrivati perché è il valore
di ritorno.

**Leggere una directory deve dare `-1`.** Il contenuto di una directory si guarda
con `readdir`: sono strutture del filesystem, e in M11 saranno voci minix da 16
byte che nessuno vuole vedere come testo.

**Chi la chiama.** `cat` in M9b. In **M14** la syscall `read`, numero 3. In
**M15** la libc di un processo utente, dietro `fread`.

**Come si sbaglia.**

- **far avanzare la posizione di `n`**;
- **far avanzare la posizione anche quando `ops->read` ha dato `-1`**;
- **non controllare `flags`**: leggere da un fd aperto `O_WRONLY` deve fallire;
- indicizzare il `void *` senza cast, come in M8.

**Test.** Otto controlli fra quelli su lettura/scrittura/posizione.

---

## 7. `vfs_write`

```c
int vfs_write(int fd, const void *buf, uint32_t n);
```

**Compito.** Come `read`, nell'altra direzione.

**Ritorna.** Quanti byte scritti; `-1` se l'fd non è aperto, se l'inode non ha
`write`, se l'fd è aperto `O_RDONLY`, o se `ops->write` ha fallito.

**Non deve** avanzare la posizione di `n` quando ne ha scritti meno — stesso
ragionamento di `read`.

**Scrivere su una directory deve dare `-1`.**

**Chi la chiama.** In M9b il comando che scrive su `/dev/console`. In **M14** la
syscall `write`, numero 4 — che è la prima syscall che un processo utente userà,
perché senza di lei non può dire niente.

**Come si sbaglia.** Come `read`. In più: **dimenticare il `const`** nella firma
dell'ops e ritrovarsi a poter modificare il buffer del chiamante.

**Test.** Tre controlli, sul dispositivo finto dell'albero di prova.

---

## 8. `vfs_lseek`

```c
int vfs_lseek(int fd, int32_t off, int whence);
```

**Compito.** Spostare la posizione, in uno dei tre modi.

**Ritorna.** La posizione nuova, oppure `-1`.

| `whence` | valore | la posizione nuova è |
|---|---|---|
| `SEEK_SET` | 0 | `off` |
| `SEEK_CUR` | 1 | posizione corrente + `off` |
| `SEEK_END` | 2 | `size` + `off` |

I tre valori sono quelli POSIX, verificati dagli header dell'host.

**`off` è firmato**, e per questo: `lseek(fd, -1, SEEK_CUR)` deve poter tornare
indietro di uno. Una posizione **negativa** però non esiste, quindi va rifiutata
con `-1` **senza muovere niente**: metà di uno spostamento è peggio di nessuno
spostamento.

**Oltre la fine è permesso**, e non è una stranezza: su Unix è così che si creano
i file sparsi. Qui non si può scrivere, quindi l'effetto è solo che la `read`
successiva dà 0.

**Un `whence` sconosciuto dà `-1`.**

**Non deve.** Chiamare niente nelle ops: la posizione è nel file aperto e non
riguarda il filesystem. È la funzione che dimostra da sola perché la posizione
non sta nell'inode.

**Chi la chiama.** `cat` in M9b, per rileggere dall'inizio. In **M14** la syscall
`lseek`, numero 19.

**Come si sbaglia.**

- **calcolare in `uint32_t`**: `off` negativo diventa un numero enorme e il
  controllo sul negativo non scatta mai. Il conto va fatto con un tipo firmato e
  poi convertito;
- **modificare la posizione e poi accorgersi che è negativa**;
- **`SEEK_END` su un dispositivo a caratteri**, dove `size` è zero: dà zero, che è
  corretto e inutile. Non è un errore.

**Test.** Sette controlli.

---

## 9. `vfs_readdir`

```c
int vfs_readdir(int fd, int idx, char *name, uint32_t *ino_out);
```

**Compito.** Chiedere alla directory la voce numero `idx`, e copiarne il nome.

**Ritorna.** `1` se la voce esiste, `0` se `idx` è oltre l'ultima, `-1` se l'fd
non è aperto, se non è una directory, o se non ha `readdir` nelle ops.

**Tre valori e non due**, perché ci sono tre esiti distinti: c'è una voce, le voci
sono finite, oppure la domanda non aveva senso. Collassando gli ultimi due, chi
enumera non distingue «directory finita» da «questo fd non è una directory» — e
un ciclo che si ferma su entrambi sembra funzionare finché non gli passi un file.

**L'indice esplicito** invece di una posizione interna è una scelta: `readdir`
diventa senza stato, quindi ripetibile e provabile con un ciclo semplice. Il
`readdir` POSIX usa la posizione del descrittore, ed è più elegante — ma qui
costerebbe di più in stato condiviso di quanto renda.

**`name` è del chiamante**, che deve dargli `VFS_NAME_MAX + 1` byte. Il nome va
copiato limitato e terminato: è il filesystem concreto a fornire il nome, e in M11
arriverà da un disco che può contenere qualunque cosa. **Un nome che arriva dal
disco non è una stringa C** finché non lo hai reso tale — è la lezione di
`device_register`, un livello più in basso.

**Chi la chiama.** `ls` in M9b. In **M14** la syscall `getdents`, numero 141.

**Come si sbaglia.**

- **restituire 0 sia per «finite» sia per «non è una directory»**;
- **non terminare `name`**;
- **fidarsi della lunghezza del nome** che arriva dalle ops.

**Test.** Cinque controlli.

---

## Riepilogo

| funzione | righe circa | primo chiamante vero |
|---|---|---|
| `vfs_init` | 8 | `kmain` in M9b |
| `vfs_resolve` | 40 | `vfs_open`, e `ls` in M9b |
| `fd_to_file`, `fd_alloc`, `file_alloc` | 25 | tutte le altre |
| `vfs_open` | 30 | `ls` e `cat` in M9b, syscall 5 in M14 |
| `vfs_close` | 12 | idem, syscall 6 in M14 |
| `vfs_read` | 20 | `cat` in M9b, syscall 3 in M14 |
| `vfs_write` | 20 | M9b, syscall 4 in M14 |
| `vfs_lseek` | 25 | `cat` in M9b, syscall 19 in M14 |
| `vfs_readdir` | 15 | `ls` in M9b, syscall 141 in M14 |

Circa 195 righe. **Cinque delle nove diventeranno syscall in M14 senza cambiare
firma** — cambierà solo chi le chiama e da quale anello di privilegio. È il
motivo per cui vale la pena che i valori di ritorno e i codici siano quelli POSIX
già adesso: la porta si chiude qui, non in M14.
