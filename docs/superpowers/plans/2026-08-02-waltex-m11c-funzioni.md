# M11c, funzione per funzione

Companion di `2026-08-02-waltex-m11c-mount.md`. Per ogni funzione: cosa deve
fare, **cosa ritorna in ogni caso**, cosa non deve fare, come ci si sbaglia, e
quale test lo prende.

La colonna «come ci si sbaglia» non è un elenco di possibilità: sono i modi in
cui questa cosa specifica va storta, in ordine di quanto costano da diagnosticare.

---

## Il quadro in una figura

```text
vfs_resolve("/dev/kbd")
    │
    ├─ current = risolvi_mount(root)          ← anche la radice puo' essere coperta
    │
    ├─ componente "dev"
    │     minix_lookup(root, "dev")  →  inode di /dev su DISCO (directory vuota)
    │     risolvi_mount(quello)      →  inode di devfs /dev        ← LA SOSTITUZIONE
    │     current = risultato
    │
    └─ componente "kbd"
          devfs dev_lookup(current, "kbd")  →  inode di kbd
          risolvi_mount(quello)             →  nessuna corrispondenza, invariato
          current = risultato
```

Due cose da leggere in questa figura:

- **`minix_lookup` viene chiamata comunque**, e risponde con l'inode della
  directory vuota che sta sul disco. Il suo risultato viene poi buttato via.
  Costa una lettura di settore per componente coperto, ed è il prezzo giusto:
  l'alternativa è che il filesystem sappia dei mount, che è il difetto che
  questa milestone chiude;
- **la sostituzione è dopo la `lookup`, non dentro.** È per questo che nessun
  filesystem la implementa.

---

## `kernel/vfs.c` — file di Walter

### `risolvi_mount(struct inode *ino) -> struct inode *`

`static`. La sola funzione nuova che non ritorna un codice di errore, perché non
può fallire.

**Cosa deve fare.** Se `ino` è il punto di un mount, ritornare la radice del
filesystem montato; e ripetere, perché sulla radice montata può essercene un
altro. Se non è il punto di nessun mount, ritornare `ino` invariato.

**Cosa ritorna.**

| caso | ritorna |
|---|---|
| `ino` non compare come `punto` in nessuno slot | `ino` |
| `ino` è il `punto` di uno slot | la `root` di quello slot, ricontrollata |
| catena di due o più mount | l'ultima radice della catena |
| tabella vuota | `ino` |
| `ino == 0` | `0` — nessuno slot ha `punto == 0`, quindi il ciclo esce subito |

**Cosa NON deve fare.** Non scrive in tabella. Non legge il disco. Non valida
`ino`: chi la chiama l'ha appena ottenuto da una `lookup` che ha risposto 0.

**Come ci si sbaglia**, in ordine di costo:

1. **Nessun ciclo esterno.** Un mount sopra un mount si ferma al primo, e il
   sintomo è che `/d` dà la radice sbagliata mentre `/d/qualcosa` dà `-1`: sembra
   un bug della `lookup` del filesystem montato, che è il posto sbagliato dove
   guardare. **Preso da** «e `/d` segue la catena fino all'ultimo montato».
2. **Ciclo esterno senza tetto.** Montando A su B e B su A il kernel si ferma
   senza un messaggio, sintomo identico a una tripla fault — è la stessa morte
   di `while (status & BSY)` su un canale ATA vuoto. **Non lo prende nessun
   test**, perché nessun test costruisce un ciclo: il tetto va scritto perché è
   giusto, non perché qualcosa lo imponga. Come il `FLUSH CACHE` di M10.

   Il tetto giusto è **`MAX_MOUNTS` giri, ed è esatto, non abbondante**: la
   catena più lunga possibile ha un anello per slot occupato, quindi
   `MAX_MOUNTS` sostituzioni la percorrono tutta. Uno di meno e il controllo
   «e il quinto viene rifiutato» del Task 1 si ferma un anello prima.
3. **Slot liberi non saltati.** Confrontare `mounts[i].punto == ino` senza aver
   escluso `punto == 0` funziona per caso — `ino` non è mai nullo quando arriva
   da una `lookup` riuscita — ma smette di funzionare il giorno che qualcuno la
   chiama con uno zero. Si controlla `punto != 0` e basta.
4. **Confrontare `ino->ino` invece del puntatore.** Con la radice su minix e
   devfs montata, `dev` e `hello.txt` hanno **entrambi** il numero 2: la tabella
   monterebbe devfs anche su `hello.txt`. **Preso da** «il mount non ha coperto
   `/etc`» nei self-check, e in modo più diretto da «`/a` non è cambiato».

---

### `vfs_mount(const char *path, struct inode *root) -> int`

**Cosa deve fare.** Verificare che l'operazione possa riuscire, e **solo dopo**
scrivere uno slot.

**Cosa ritorna.**

| caso | ritorna |
|---|---|
| tutto a posto | `0`, e lo slot è scritto |
| `radice == 0` | `-1` |
| `radice->type != INODE_DIR` | `-1` |
| `path` non si risolve (inesistente, relativo, troppo lungo) | `-1` |
| il punto risolto non è una directory | `-1` |
| `punto == radice` | `-1` (il ciclo di lunghezza uno) |
| nessuno slot libero | `-1` |
| si monta due volte sullo stesso path | `0`, e si **impila** |

L'ultima riga merita una parola. Non è un caso speciale scritto apposta: `path`
si risolve con `vfs_resolve`, che applica già `risolvi_mount`, quindi il punto
del secondo mount è la **radice del primo**. Ne esce l'impilamento di Unix,
gratis, e la scelta di lasciarlo passare invece di rifiutarlo è deliberata —
riconoscerlo vorrebbe dire confrontare anche con le `root` in tabella, cioè
scrivere codice per vietare un comportamento corretto.

**Cosa NON deve fare.**

- **Non crea il punto di mount se manca.** In Unix è `ENOENT`, e un mountpoint
  che appare dal nulla nasconde un errore di battitura.
- **Non scrive niente sul disco.** Il filesystem montante non sa di essere
  montato. Il controllo che lo prova è manuale: si monta l'immagine sull'host e
  `/dev` dev'essere vuota.
- **Non tocca `root` (la variabile globale del VFS).** Il parametro si chiama
  `root` come lei, ed è la trappola di nome più vicina di tutta la milestone: in
  `vfs.c` esiste già uno `static struct inode *root`. Se il parametro lo ombra,
  ogni riferimento dentro la funzione va al parametro — che è quello che si
  vuole — ma **una riga scritta pensando alla globale fa la cosa sbagliata in
  silenzio**. Vale la pena chiamare il parametro `radice` e togliere il dubbio.

**Come ci si sbaglia**, in ordine di costo:

1. **Scrivere in tabella prima di aver finito i controlli.** È il bug di M11b —
   `minix_create` che allocava l'inode e poi rifiutava il nome — nella sua forma
   più piccola: uno slot occupato da un mount mai avvenuto, che riduce
   `MAX_MOUNTS` di uno per sempre e non ha nessun sintomo finché la tabella non
   si esaurisce. **Preso da** «dopo quattro rifiuti `/d/b` si risolve ancora».
2. **Dimenticare che il punto dev'essere una directory.** Montare su un file dà
   un albero in cui `/a/qualcosa` funziona, cioè un file che si comporta da
   directory. **Preso da** «montare su un file fallisce».
3. **Dimenticare che la radice montata dev'essere una directory.** Il sintomo
   arriva più tardi e altrove: `vfs_resolve` fa il controllo `type != INODE_DIR`
   sul *giro seguente*, quindi il primo componente dopo il mount fallisce e
   sembra un problema di `lookup`. **Preso da** «montare una radice che non è una
   directory fallisce».
4. **Ritornare un valore positivo sull'errore.** È la classe dei tre bug di M9b
   — `root_lookup` che ritornava `1`, `chardev_read` che ritornava `1`. Qui
   nessun chiamante ci camminerebbe sopra subito, il che la rende peggio: si
   scopre in M12. **Preso da** ognuno dei quattro controlli di rifiuto, che
   confrontano con `== -1` e non con `!= 0`.

---

### `vfs_resolve` — due righe in più

**Cosa cambia.** L'inizializzazione di `current`, e una riga dopo la `lookup`.

```text
struct inode *current = risolvi_mount(root);
...
    if (current->ops->lookup(current, nome, &prossimo) < 0)
        return -1;

    prossimo = risolvi_mount(prossimo);
    current  = prossimo;
```

**Cosa ritorna.** Invariato: `0` e `*out`, oppure `-1` senza toccare `*out`. I
75 controlli di M9a devono passare **senza una modifica** — è il controllo che
dice che M11c non ha rotto niente.

**Cosa NON deve fare.**

- **Non chiamare `risolvi_mount` prima della `lookup`.** `current` è già stato
  sostituito quando è entrato nel ciclo: rifarlo è un giro di tabella per niente.
- **Non spostare il controllo `current->type != INODE_DIR`.** Sta in cima al
  giro, quindi vede l'inode **già sostituito**, e questo è esattamente ciò che
  serve: dev'essere una directory il filesystem montato, non la directory
  coperta.

**Come ci si sbaglia**, in ordine di costo:

1. **Sostituire dentro `*out` alla fine invece che dentro il ciclo.** Il caso
   `/dev` funzionerebbe e `/dev/kbd` no, perché il secondo componente verrebbe
   cercato dentro la directory *coperta* — che sull'immagine è vuota. Sintomo:
   `ls /dev` mostra niente e `cat /dev/kbd` dà errore, con `/dev` che si risolve
   benissimo. **Preso da** «`/d/m` si risolve: il contenuto è quello montato» e,
   nella VM, da `ls /dev` in `tests/shell.sh`.
2. **Sostituire `current` invece di `prossimo`.** Sostituire l'inode *da cui* si
   cerca invece di quello *trovato* significa che il mount si applica un
   componente troppo tardi. Su un albero a due livelli i sintomi sono confusi.
   **Preso da** «`/d` dà esattamente l'inode montato».
3. **Dimenticare la riga sull'inizializzazione.** Nessun sintomo oggi — nessuno
   monta sulla radice — ma è mezzo meccanismo che manca, e il giorno che serve
   il difetto è invisibile: `vfs_resolve("/")` è l'unico cammino che non passa da
   nessuna `lookup`.

---

### `vfs_init` — una riga in più

**Cosa deve fare.** Azzerare anche `mounts[]`, accanto alle due tabelle che già
azzera.

**Come ci si sbaglia.** Dimenticarla, e il sintomo è a due facce: nei test host
ogni gruppo eredita i mount del gruppo precedente — quindi i controlli passano o
falliscono a seconda dell'ordine in cui si chiamano; nel kernel, il ramo di
ripiego di `kmain` fa un secondo `vfs_init(devfs_root())` lasciando in piedi un
mount verso inode di un filesystem che non è più montato. **Preso da** «dopo
`vfs_init` la tabella è vuota: `/d/b` torna».

Attenzione al valore di «libero», che in questo file è diverso per ogni tabella:
`files[i].inode == 0`, `fds[t][k] == -1`, e adesso `mounts[i].punto == 0`. Il
`-1` dei descrittori esiste perché lì lo zero è un fd valido — sarà stdin in
M15. Qui lo zero va bene, come per `files`.

---

## `kernel/minixfs.c` — file di Walter

### `minix_lookup` — la diramazione se ne va

**Cosa cambia.** Spariscono le righe intorno a `minixfs.c:1092` che controllano
l'innesto prima del disco.

**Cosa ritorna.** Invariato: `0` e `*out` se il nome c'è sul disco, `-1`
altrimenti — e su `-1` `*out` non si tocca.

**Cosa NON deve fare.** Non deve sapere che i mount esistano. Dopo questa
milestone `minixfs.c` non ha nessuna riga che parli di innesti o di altri
filesystem, ed è la proprietà da conservare: se un giorno qualcosa ce la
rimette, la milestone è stata annullata.

**Come ci si sbaglia.** Togliere la diramazione da `lookup` e non da `readdir`.
Il risultato è che `ls /` mostra `dev` **due volte** — una dal disco e una
dall'innesto — mentre tutto il resto funziona. **Preso da** «readdir della radice
dà otto voci» in `test_minixfs.c`, che conta.

---

### `minix_readdir` — la diramazione se ne va

**Cosa cambia.** Spariscono le righe intorno a `minixfs.c:1163` che elencano
l'innesto come voce in più dopo quelle su disco.

**Cosa ritorna.** Invariato: `1` se la voce esiste, `0` se `idx` è oltre
l'ultima, `-1` se la domanda non aveva senso.

**Come ci si sbaglia.** Lasciare che `idx` conti ancora la voce fantasma, cioè
togliere la copia del nome ma non l'aggiustamento dell'indice. Sintomo: l'ultima
voce vera non si vede, e `readdir` risponde `0` un indice troppo presto.
**Preso da** «readdir della radice elenca le otto voci nell'ordine giusto».

---

### `minixfs_init` — una riga in meno

Via `memset(&innesto, 0, sizeof(innesto));`. Con la `struct` rimossa non
compilerebbe, quindi l'errore è immediato — è il caso raro in cui il compilatore
fa da test.

---

### `minixfs_graft` — rimossa

Non c'è niente da conservare. La ragione per cui sparisce sta nel commento
sostitutivo in `include/minixfs.h`, ed è il posto giusto: chi cerca la funzione
guarda l'header.

---

## `kernel/main.c` — file di Claude

### `kmain` — l'ordine si rovescia

**Cosa cambia.** Da

```text
minixfs_init  →  minixfs_graft  →  vfs_init
```

a

```text
minixfs_init  →  vfs_init  →  vfs_mount
```

**Perché l'ordine è obbligato**, e sono due ragioni indipendenti — vale la pena
saperle tutte e due, perché se una cadesse l'altra reggerebbe comunque:

- `vfs_init` **azzera** la tabella di mount: montare prima significa non montare;
- `vfs_mount` **risolve un path**, e senza radice `vfs_resolve` ritorna `-1` alla
  prima riga.

**Cosa cambia nel ripiego.** Prima, un innesto fallito faceva ripiegare l'intero
albero su devfs. Adesso no: la radice su disco resta, `/dev` resta la directory
vuota che è sull'immagine, e il marker diverso dice cosa è successo. Perdere il
filesystem intero perché un mount non è andato sarebbe sproporzionato — e
soprattutto **adesso è possibile**, perché il punto di mount esiste comunque.

**Come ci si sbaglia.**

1. **Montare `devfs_root()` invece di `devfs_devdir()`.** È l'errore di M11a, che
   quattro self-check hanno preso: la radice di devfs ha una sola voce e si
   chiama `dev`, quindi si ottiene `/dev/dev/kbd`. **Preso da** «`/dev/kbd` si
   risolve, con la radice su minix» nei self-check, e da `ls /dev` in
   `tests/shell.sh`.
2. **Non aggiornare il marker in `tests/smoke.sh`.** Il kernel funziona e il test
   fallisce su un marker mancante, che è il modo giusto di sbagliare.
3. **Lasciare il ripiego a `vfs_init(devfs_root())` anche sul mount fallito.**
   Compila, boota, e nasconde il guasto: `/dev` funzionerebbe comunque e nessuno
   si accorgerebbe che il disco è sparito. È il genere di ripiego che rende un
   sistema più difficile da diagnosticare, non più robusto.

---

## `kernel/selftest.c` — file di Claude

### `check_minix` — tre controlli al posto di due

**La cosa da capire prima di scriverli.** Dopo M11c `/dev` **esiste
sull'immagine** come directory vuota. Quindi:

```c
report("/dev esiste", vfs_resolve("/dev", &ino) == 0);   /* NON PROVA PIU' NIENTE */
```

passerebbe anche a mount completamente fallito. È esattamente il caso in cui un
controllo vecchio sopravvive a un cambiamento e comincia a mentire — la stessa
specie del filtro `tr -d 'AB'` di `keyboard.sh`, caduto in M7 quando i task sono
diventati silenziosi.

Ciò che prova il mount è **l'identità del puntatore**:

| controllo | cosa prende che gli altri non prendono |
|---|---|
| `vfs_resolve("/dev") == devfs_devdir()` | la sostituzione è avvenuta, sull'inode giusto |
| `/dev/kbd` si risolve ed è `INODE_CHARDEV` | la catena continua **dentro** il montato — la directory su disco è vuota |
| `/etc` è ancora una directory | la sostituzione **non** è avvenuta dove non doveva |

Il terzo sembra ozioso e non lo è: è il solo che prende una `risolvi_mount` che
confronta `ino->ino` invece del puntatore, perché su minix `etc` e `dev` sono
inode diversi ma `dev` e `hello.txt` no.

---

## Riepilogo: chi prende cosa

| guasto | lo prende |
|---|---|
| sostituzione mancante del tutto | «`/d` dà esattamente l'inode montato» (host) |
| sostituzione fuori dal ciclo | «`/d/m` si risolve» (host), `ls /dev` (VM) |
| sostituzione su `current` invece di `prossimo` | «`/d` dà esattamente l'inode montato» (host) |
| il mount non copre | «`/d/b` non si risolve più» (host) |
| confronto su `ino` invece del puntatore | «il mount non ha coperto `/etc`» (self-check) |
| ciclo esterno mancante | «`/d` segue la catena» (host) |
| **ciclo esterno senza tetto** | **nessuno** — si scrive perché è giusto |
| slot scritto prima dei controlli | «dopo quattro rifiuti `/d/b` si risolve ancora» (host) |
| tabella non azzerata da `vfs_init` | «dopo `vfs_init` la tabella è vuota» (host) |
| tabella piena non rilevata | «e il quinto viene rifiutato» (host) |
| diramazione tolta da `lookup` ma non da `readdir` | «readdir della radice dà otto voci» (host) |
| `devfs_root()` al posto di `devfs_devdir()` | «`/dev/kbd` si risolve» (self-check), `ls /dev` (VM) |
| marker non aggiornato | `tests/smoke.sh` |
| **il mount scrive sul disco** | **la verifica manuale con `mount -o loop,ro`** |

Due righe di questa tabella dicono «nessuno», e non è una lacuna del piano: è la
stessa struttura di M10, dove il `FLUSH CACHE` mancante non lo prendeva nessun
test e il flush è rimasto lo stesso. Un test che non esiste va scritto nel piano
proprio perché non esiste — così non si toglie il codice credendolo coperto.
