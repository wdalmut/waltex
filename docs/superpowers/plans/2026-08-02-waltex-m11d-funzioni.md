# M11d, funzione per funzione

Companion di `2026-08-02-waltex-m11d-procfs.md`. Per ogni funzione: cosa deve
fare, **cosa ritorna in ogni caso**, cosa non deve fare, come ci si sbaglia, e
quale test lo prende.

«Come ci si sbaglia» non è un elenco di possibilità: sono i modi in cui questa
cosa specifica va storta, in ordine di quanto costano da diagnosticare.

---

## Il quadro in una figura

```text
cat /proc/1/status
  │
  ├─ vfs_resolve("/proc/1/status")
  │     minix_lookup(root, "proc")  →  inode di /proc su DISCO (dir vuota)
  │     risolvi_mount(quello)       →  ino_root di procfs      ← il mount
  │
  │     root_lookup(ino_root, "1")  →  "1" e' un numero? il task 1 e' attivo?
  │                                    → &ino_task[1]
  │
  │     task_lookup(&ino_task[1], "status")  →  &ino_status[1]
  │
  └─ vfs_read(fd, buf, 64)
        status_read(&ino_status[1], off=0, buf, 64)
              i = PROC_TASK_DA_STATUS(ino->ino)      ← 1, dal NUMERO
              char testo[128];                       ← LOCALE
              len = genera_status(i, testo, 128)     ← il contenuto NASCE QUI
              copia testo[off .. off+n)  →  buf
```

Tre cose da leggere in questa figura:

- **nessuno tiene un `struct task *`.** L'indice viaggia dentro `ino`, e
  `status_read` lo ricava con una sottrazione. In M16, quando uno slot di task
  viene riciclato, un numero descrive comunque «lo slot 1 *adesso*», mentre un
  puntatore descriverebbe un processo che non c'è più;
- **`minix_lookup` viene chiamata comunque** e la sua risposta viene buttata via.
  Stesso prezzo di `/dev` in M11c, stessa ragione: è il risolutore a sostituire,
  non il filesystem;
- **il contenuto non esiste prima di `status_read`**, e smette di esistere quando
  ritorna. È l'unica differenza vera fra procfs e gli altri due filesystem.

---

## `kernel/memory.c` — file di Walter

### `utoa(uint32_t v, unsigned base, char *buf, int max) -> int`

**Cosa deve fare.** Scrivere `v` in base `base` dentro `buf`, terminata da
`'\0'`.

**Cosa ritorna.**

| caso | ritorna |
|---|---|
| ci sta | il numero di caratteri **senza** il terminatore |
| `v == 0` | `1`, e `buf` contiene `"0"` |
| `base` diversa da 10 e 16 | `-1`, `buf` intatto |
| `max < 2` | `-1`, `buf` intatto |
| le cifre non ci stanno in `max - 1` | `-1`, `buf` intatto |
| `buf == 0` | `-1` |

**Cosa NON deve fare.** Non scrive niente su `-1` — la convenzione di `lookup` e
di `shell_parse_hex`: su ogni uscita d'errore il chiamante tiene quello che
aveva. Non mette il prefisso `0x` in base 16: lo mette il chiamante, che è
l'unico a sapere se sta scrivendo un indirizzo o un numero. Non tratta il segno,
e il nome lo dichiara.

**Come ci si sbaglia**, in ordine di costo:

1. **Le cifre escono al contrario.** Si generano dividendo per la base, quindi
   dall'**ultima**: senza il rovesciamento finale `123` diventa `"321"`, che è
   leggibile, ordinato e sbagliato — lo stesso genere di guasto degli `insw`
   senza `cld` in M10. **Preso da** «`utoa(123)` non ha le cifre al contrario»,
   e da nessun altro: `utoa(7)` e `utoa(0)` passerebbero comunque.
2. **`while (v) { ... }` invece di `do { ... } while (v)`.** Con `v == 0` il
   ciclo non gira nemmeno una volta e si ottiene la stringa vuota invece di
   `"0"`. Nel kernel il sintomo è `ls /proc` che mostra una riga senza nome per
   il task 0 — cioè per `kmain`, che c'è sempre. **Preso da** «`utoa(0)` scrive
   `"0"`, non la stringa vuota».
3. **`max` contato senza il terminatore.** Se `max` è il buffer intero, una
   cifra vuole `max >= 2`. Contandolo male si scrive un byte oltre la fine, che
   su uno stack sembra funzionare finché non tocca qualcosa. **Preso da**
   «`utoa` rifiuta un buffer troppo corto senza scriverci» e da
   «`utoa(4294967295)` ci sta in undici byte», che è il caso esatto al limite.
4. **Il buffer temporaneo troppo corto.** `4294967295` sono dieci cifre; con
   otto byte si scavalca. Dieci basta per la base 10 e avanza per la 16, che al
   massimo ne fa otto.

---

## `kernel/procfs.c` — file di Walter

### `procfs_init(void)`

**Cosa deve fare.** Riempire i 17 inode statici: la radice, `MAX_TASKS`
directory, `MAX_TASKS` foglie `status`. Alzare il flag `pronto`.

**Cosa ritorna.** Niente. Non può fallire: non alloca, non legge, non tocca
hardware.

**Cosa NON deve fare — e questa è la parte importante.** **Non guarda la tabella
dei task.** Riempie *tutti* gli slot, attivi o no, perché è `lookup` e `readdir`
a decidere quali far vedere, **al momento della domanda**. Un `procfs_init` che
leggesse la tabella darebbe un `/proc` congelato all'istante del boot: `spin`
avvierebbe due task e `/proc` non se ne accorgerebbe.

Non ha nessun vincolo d'ordine rispetto a `task_init`, ed è la conseguenza
diretta della riga sopra — al contrario di `devfs_init`, che **deve** venire dopo
tutte le `*_init` dei driver perché legge il registro una volta sola.

**Come ci si sbaglia.**

1. **Riempire solo gli slot attivi.** Funziona al boot e si rompe a `spin`.
   **Preso da** «e la sua directory compare SUBITO, senza rifare `procfs_init`».
2. **Dimenticare `pronto`.** `procfs_root()` restituirebbe un puntatore valido a
   una struct di zeri: `type` sarebbe `INODE_NONE`, `ops` nullo, e `vfs_mount`
   rifiuterebbe con un `-1` che non dice perché. **Preso da** «`procfs_root` è
   nulla prima di `procfs_init`».
3. **Numerare gli inode a mano invece che con le macro.** Le tre macro di
   `procfs.h` sono un patto fra `lookup`, che assegna, e `read`, che risale.
   Scrivere `2 + i` in un posto e usare `PROC_TASK_DA_DIR` nell'altro funziona
   finché qualcuno non cambia la formula in un posto solo.

---

### `procfs_root(void) -> struct inode *`

**Cosa ritorna.** `&ino_root` se `procfs_init` è stata chiamata, **`0`**
altrimenti. Stessa scelta di `devfs_root()` e `minixfs_root()`, e per la stessa
ragione: restituire una radice di zeri fa fallire ogni risoluzione senza dire
perché, mentre uno zero si vede al primo controllo.

**Cosa NON deve fare.** Non chiama `procfs_init` da sé. «Nessuna
inizializzazione implicita o lazy» è un vincolo del progetto: ogni sottosistema
ha una `*_init()` esplicita chiamata da `kmain` in ordine visibile.

**Come ci si sbaglia.** Due chiamate che danno puntatori diversi. Non è
un'ipotesi remota: se qualcuno la facesse ritornare una copia invece di
`&ino_root`, **la tabella di mount smetterebbe di funzionare** — indicizza per
puntatore. **Preso da** «due `procfs_root()` danno lo stesso puntatore».

---

### `task_attivo(int i) -> int`

`static`, tre righe, e vale la pena averla separata perché la chiamano in
quattro.

**Cosa deve fare.** Dire se lo slot `i` esiste e ospita un task vivo.

**Cosa ritorna.** `1` se `0 <= i < MAX_TASKS`, `task_slot(i)` non è nullo e il
suo `state` non è `TASK_FREE`. `0` in ogni altro caso.

**Come ci si sbaglia.** Controllare `state != TASK_FREE` **prima** di aver
verificato che `task_slot(i)` non sia nullo: `task_slot` ritorna 0 per un indice
fuori intervallo, e dereferenziarlo è il primo indirizzo della memoria. È lo
stesso ordine di controlli che in M10 vuole BSY prima di tutto — finché il primo
non è vero, il secondo non significa niente.

---

### `genera_status(int i, char *buf, int max) -> int`

`static`. È il cuore della milestone: la funzione che fa esistere il contenuto.

**Cosa deve fare.** Scrivere le tre righe dentro `buf` e ritornare la lunghezza.

```text
Pid:    0
State:  R (running)
Esp:    0x00107f5c
```

**Cosa ritorna.** La lunghezza scritta, **senza** il terminatore. `-1` se
`i` non è un task attivo o se il testo non ci sta in `max`.

**Cosa NON deve fare.**

- **Niente `'\t'`.** Linux allinea con i tab; noi non possiamo, perché `'\t'`
  vale 9 e `vga_putc` gestisce solo `>= 32` più `'\n'` e `'\b'`. Un tab
  funzionerebbe sulla seriale e sparirebbe sul framebuffer, che è **il bug del
  backspace di M7 rifatto**. **Preso da** «l'allineamento è fatto con spazi, non
  con tab».
- **Non tiene un `struct task *` oltre la propria durata.** Dentro la funzione
  serve per leggere `state` ed `esp`, e va bene: la funzione ritorna subito. Ciò
  che non deve succedere è che il puntatore finisca in un inode.

**Come ci si sbaglia**, in ordine di costo:

1. **Non controllare che il testo ci stia.** Tre righe sono una cinquantina di
   byte e il buffer ne ha 128, quindi il controllo sembra inutile — finché
   qualcuno non aggiunge una quarta riga. Scrivere oltre un array locale
   corrompe lo stack del chiamante, e il guasto compare al `return` di una
   funzione che non c'entra.
2. **`R (running)` per tutti.** La distinzione fra il task corrente e gli altri
   la dà `task_current()`, ed è l'unica informazione che questa funzione
   aggiunge rispetto a leggere la struct. **Non lo prende nessun test host**,
   perché sull'host il task corrente è sempre lo 0 e con un task solo i due rami
   danno lo stesso risultato. Vale la pena guardarlo a mano nella VM dopo `spin`.
3. **`esp` senza il prefisso `0x`.** `utoa` in base 16 non lo mette apposta, e
   `1a2b` letto come decimale è un numero diverso. **Preso da** «contiene l'Esp
   in esadecimale», che cerca `Esp:    0x`.

Una nota sul valore di `esp`, la stessa che `shell_ps` ha già in fondo: per il
task **in esecuzione** il numero è vecchio, e va bene così. Il campo `esp` di
`struct task` viene scritto solo quando il task viene abbandonato da
`task_switch`; finché sta girando, il suo `esp` vero è nel registro della CPU.
Quindi la riga dice dove il task era l'ultima volta che ha ceduto il controllo.

---

### `root_lookup(struct inode *dir, const char *name, struct inode **out) -> int`

**Cosa deve fare.** Tradurre un nome — che è un numero in cifre decimali — nella
directory di quel task, **se quel task è attivo adesso**.

**Cosa ritorna.**

| caso | ritorna |
|---|---|
| `name` è un numero valido e il task è attivo | `0`, e `*out = &ino_task[i]` |
| `name` non è tutto cifre | `-1` |
| `name` è vuoto | `-1` |
| il numero è `>= MAX_TASKS` | `-1` |
| lo slot esiste ma è `TASK_FREE` | `-1` |

**Su `-1` `*out` non si tocca.** È il primo dei tre bug di M9b: `root_lookup` di
`devfs` ritornava `1` invece di `-1`, `vfs_resolve` controlla `< 0`, e quindi
camminava su un puntatore mai inizializzato. Nessun sintomo stabile, una cosa
diversa a ogni boot.

**Cosa NON deve fare.** Non gestisce `"."` e `".."`: non li gestisce nemmeno
`devfs`, e il VFS non li richiede. Non accetta `"+3"`, `" 3"` o `"03"` — un
nome deve corrispondere a un file solo, e tre grafie per lo stesso task sono la
stessa specie di errore del nome di dispositivo troncato in M8.

**Come ci si sbaglia**, in ordine di costo:

1. **Accettare un nome che *comincia* con una cifra.** Un parser che si ferma al
   primo carattere non numerico risolve `"0pippo"` come il task 0. Si scandisce
   **tutta** la stringa e si rifiuta al primo carattere fuori posto, come
   `shell_parse_dec`. **Preso da** «un nome non numerico non si risolve» solo se
   il nome comincia con una lettera — vale la pena aggiungersi il caso
   `"0pippo"` mentre si scrive.
2. **Non controllare `task_attivo`.** `/proc/7` esisterebbe sempre, e
   `cat /proc/7/status` leggerebbe uno slot vuoto. **Preso da** «uno slot libero
   non compare in `/proc`».
3. **Ritornare `1` invece di `-1`.** Vedi sopra: è il bug di M9b, e qui avrebbe
   lo stesso sintomo instabile.

---

### `root_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out) -> int`

**LA funzione difficile della milestone**, e la difficoltà sta in una parola.

**`idx` è una POSIZIONE fra i task attivi, non un indice di task.** Con i task 0
e 3 attivi:

```text
idx = 0   ->   "0"     (task 0)
idx = 1   ->   "3"     (task 3)     <- NON "1", che non e' attivo
idx = 2   ->   0, le voci sono finite
```

**Cosa ritorna.** `1` e il nome se la voce esiste, `0` se `idx` è oltre
l'ultima, `-1` se la domanda non aveva senso — `idx` negativo, oppure `dir`
che non è la radice di procfs.

Tre valori e non due: collassando `0` e `-1`, chi enumera non distingue
«directory finita» da «questo non è una directory», e un ciclo che si ferma su
entrambi sembra funzionare finché non gli passi un file.

**Cosa NON deve fare.** Non deve **memorizzare** dove era arrivato. `idx` è
un'informazione del chiamante, e procfs deve poter rispondere a `idx = 5` senza
che nessuno abbia chiesto `idx = 4`. È la stessa proprietà che rende `readdir`
di minix riavviabile.

**Come ci si sbaglia**, in ordine di costo:

1. **Trattare `idx` come indice di task.** `ls /proc` con i task 0 e 3 attivi
   mostra `0` e poi si ferma — perché `idx = 1` cade su uno slot libero e la
   funzione ritorna `0`, che significa «finito». Il sintomo è una lista
   *plausibile e incompleta*, che è il peggior genere. **Preso da** «readdir
   elenca esattamente i task attivi», che conta.
2. **`readdir` e `lookup` in disaccordo.** Se `readdir` elenca uno slot che
   `lookup` non risolve, `ls /proc` mostra un nome e `cat` su quel nome
   fallisce. È la lezione di M11a, dove l'innesto compariva in una sola delle
   due. **Preso da** «readdir e lookup della radice sono d'accordo», che per ogni
   nome enumerato chiama `lookup`.
3. **`idx` negativo non rifiutato.** Con `idx = -1` un ciclo che sottrae
   indicizza fuori dall'array. **Preso da** «readdir con un indice negativo dà
   -1».
4. **Il nome non ci sta.** `name` è del chiamante e vuole `VFS_NAME_MAX + 1`
   byte; `utoa` di un indice sotto 8 ne scrive due, quindi c'è margine — ma il
   valore di ritorno di `utoa` va controllato lo stesso, perché è l'unico posto
   che sa se è andata.

---

### `task_lookup(struct inode *dir, const char *name, struct inode **out) -> int`

**Cosa deve fare.** Una cosa sola: se `name` è `"status"`, dare
`&ino_status[i]`, dove `i` viene da `PROC_TASK_DA_DIR(dir->ino)`.

**Cosa ritorna.** `0` e `*out` se il nome è `"status"` e il task è ancora
attivo, `-1` altrimenti — e su `-1` `*out` non si tocca.

**Qui `dir` serve DAVVERO**, ed è la terza volta nel progetto: una funzione sola
per otto directory, e `dir->ino` è l'unica cosa che le distingue. La prima nota
sta nelle `inode_ops` da M9b, la seconda è `minix_lookup`.

**Cosa NON deve fare.** Non ricontrolla che `dir` sia una directory: lo ha già
fatto `vfs_resolve` prima di chiamare. Ma **deve** ricontrollare `task_attivo`:
fra la `lookup` della directory e questa può essere passato un tick, e in M16 il
task può essere uscito nel frattempo.

**Come ci si sbaglia.**

1. **Usare `PROC_TASK_DA_STATUS` invece di `PROC_TASK_DA_DIR`.** Le due inverse
   differiscono di `MAX_TASKS`, quindi si ottiene un indice **plausibile e
   falso**: `/proc/0/status` mostrerebbe il task -8, cioè un `task_attivo` che
   rifiuta, cioè un `/proc/0/status` che non esiste. Diagnosi lenta, perché tutto
   il resto funziona. Il companion di questa formula è che chi la usa deve *già*
   sapere che tipo di inode ha in mano.
2. **Accettare qualunque nome.** `/proc/0/pippo` darebbe l'inode di `status`, e
   `cat` stamperebbe il contenuto giusto sotto il nome sbagliato. **Preso da**
   «dentro `/proc/0` non c'è nient'altro».

---

### `task_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out) -> int`

**Cosa deve fare.** La più semplice delle sette: una voce sola.

**Cosa ritorna.** `1` con `name = "status"` e `*ino_out = PROC_INO_STATUS(i)` se
`idx == 0`; `0` per `idx >= 1`; `-1` per `idx` negativo.

**Come ci si sbaglia.** Ritornare `1` per ogni `idx`, che manda in ciclo infinito
chiunque enumeri. `shell_ls` si ferma solo sullo zero.

---

### `status_read(struct inode *ino, uint32_t off, void *buf, uint32_t n) -> int`

**Cosa deve fare.** Generare il testo e consegnarne la fetta che parte da `off`.

**Cosa ritorna.**

| caso | ritorna |
|---|---|
| `off` dentro il testo | quanti byte ha copiato, da 1 a `n` |
| `off >= lunghezza` | `0` — su un file regolare significa fine, ed è così che `shell_cat` esce |
| `n == 0` | `0`, senza toccare `buf` |
| il task non è più attivo | `0`, che si legge come «file vuoto» |
| l'inode non è di procfs, o la generazione fallisce | `-1` |

La quarta riga è una scelta, non un caso dimenticato: un task uscito fra la
`open` e la `read` dà un file **vuoto**, non un errore. È quello che fa Linux, e
il motivo è che `cat` su un processo appena morto deve finire in silenzio invece
di stampare un messaggio d'errore.

**Cosa NON deve fare.**

- **Il buffer di generazione NON è statico.** Statico costerebbe 128 byte una
  volta sola, ma fra il «genero» e il «copio» ci sta un tick del timer, e due
  `cat /proc/*/status` in parallelo si mescolerebbero. Locale non è condiviso con
  nessuno, e non serve nessuna sezione critica. Sono 128 byte su uno stack da
  4096.
- **Non memorizza il testo generato fra una chiamata e l'altra.** Rigenerare a
  ogni `read` è O(righe) su tre righe: la cache è il problema che `seq_file`
  risolve in Linux perché lì i file di `/proc` possono essere lunghi megabyte.

**Come ci si sbaglia**, in ordine di costo:

1. **Ignorare `off`.** Ogni chiamata riparte da capo, quindi `cat` stampa il
   primo pezzo all'infinito — o, con un `n` grande abbastanza, sembra funzionare
   e si rompe solo quando il testo supera il buffer del chiamante. **Preso da**
   «read rispetta l'offset», che legge a pezzi da **7 byte** apposta: un numero
   che non divide niente, così ogni lettura cade in un punto diverso.
2. **Il buffer statico.** Non lo prende nessun test — né host né dentro la VM,
   perché nessuno dei due fa due `cat` di `/proc` contemporanei. È della stessa
   specie del tetto di `risolvi_mount` in M11c e del `FLUSH CACHE` di M10: si
   scrive perché è giusto.
3. **Ritornare la lunghezza generata invece di quella copiata.** Con `n` più
   piccolo del testo, `vfs_read` avanzerebbe l'offset di più di quanto ha
   ricevuto e il chiamante leggerebbe un buffer mezzo vuoto credendolo pieno. È
   la stessa classe di `chardev_read` che ritornava `1` in M9b. **Preso da** «si
   legge, e non è vuoto» insieme al confronto sul contenuto, perché con
   l'avanzamento sbagliato il testo ricomposto ha dei buchi.
4. **`n` ignorato.** Copiare tutto il testo in un buffer che ne aveva chiesti 7
   scavalca di quaranta byte. È il primo dei due bug di `kbd_dev_read` in M8,
   trovato leggendo e non eseguendo. **Preso da** «read rispetta l'offset», ma
   solo perché quel test passa `7`: con un `n` grande non lo vedrebbe nessuno.

---

## Riepilogo: chi prende cosa

| guasto | lo prende |
|---|---|
| `utoa` con le cifre al contrario | «`utoa(123)` non ha le cifre al contrario» (host) |
| `utoa(0)` che dà la stringa vuota | «`utoa(0)` scrive `"0"`» (host) |
| `utoa` che scrive oltre il buffer | «rifiuta un buffer troppo corto» + «ci sta in undici byte» (host) |
| `procfs_init` che legge la tabella dei task | «la sua directory compare SUBITO» (host) |
| `pronto` dimenticato | «`procfs_root` è nulla prima di `procfs_init`» (host) |
| `procfs_root` che non dà lo stesso puntatore | «due `procfs_root()` danno lo stesso puntatore» (host) |
| slot liberi che compaiono | «uno slot libero non compare in `/proc`» (host) |
| `idx` trattato come indice di task | «readdir elenca esattamente i task attivi» (host) |
| `readdir` e `lookup` in disaccordo | «readdir e lookup della radice sono d'accordo» (host) |
| `idx` negativo | «readdir con un indice negativo dà -1» (host) |
| nome non numerico accettato | «un nome non numerico non si risolve» (host) |
| inversa sbagliata in `task_lookup` | «`/proc/0/status` si trova» (host) |
| qualunque nome dentro `/proc/N` | «dentro `/proc/0` non c'è nient'altro» (host) |
| `off` ignorato in `status_read` | «read rispetta l'offset» (host) |
| il `'\t'` al posto degli spazi | «l'allineamento è fatto con spazi» (host) |
| `0x` mancante davanti a `esp` | «contiene l'Esp in esadecimale» (host) |
| `/proc` non montato | «`/proc` è esattamente l'inode di procfs» (self-check) |
| procfs che non vede i task VERI del kernel | «e comincia con `"Pid:"`» (self-check), `ls /proc` (VM) |
| **il buffer di generazione statico** | **nessuno** — si scrive perché è giusto |
| **`R (running)` per tutti** | **nessuno** — si guarda a mano dopo `spin` |
| **una riga in `vfs.c` o `minixfs.c`** | **`git diff --stat`**, Task 3 Passo 8 |

Tre righe dicono «nessuno», ed è la stessa struttura di M10 e M11c. Un test che
non esiste va scritto nel piano **proprio perché** non esiste: così non si toglie
il codice credendolo coperto, e si sa in anticipo cosa va guardato con gli occhi.

L'ultima riga è di specie diversa dalle altre due — non è un guasto che sfugge,
è **la misura della milestone**. Se `git diff --stat kernel/vfs.c
kernel/minixfs.c` non è vuoto, `procfs` funziona lo stesso e M11c era
incompleta: il test verde e la conclusione sbagliata convivono benissimo, ed è
esattamente per questo che il controllo va fatto invece che dedotto.
