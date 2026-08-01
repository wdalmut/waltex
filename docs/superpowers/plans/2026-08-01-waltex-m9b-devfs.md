# M9b — devfs: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** contengono
> interfaccia, test, concetti e tranelli, ma il codice lo scrive Walter
> (vedi `CLAUDE.md`). I task **[CLAUDE]** sono infrastruttura.
>
> **Companion:** `2026-08-01-waltex-m9b-funzioni.md` — una scheda per funzione.

**Obiettivo:** `cat /dev/kbd` funziona, e `cat` non sa cos'è una tastiera.

È la milestone in cui la frase «tutto è un file» smette di essere un proposito e
diventa una cosa che si può digitare al prompt.

**Architettura:** un filesystem concreto di centoventi righe, fatto di **tre
tipi di inode** e nient'altro:

```text
/           directory con UNA voce: "dev"
/dev        directory le cui voci SONO il registro dei dispositivi di M8
/dev/kbd    foglia: le sue read e write chiamano quelle di struct device
```

Nessuno di questi tre memorizza le proprie voci: le **genera** quando gliele si
chiede. È la proprietà che il VFS di M9a compra con `lookup`, e qui si vede per
la prima volta a cosa serviva.

Circa 120 righe di kernel, di cui metà sono il ponte fra due interfacce che
esistono già.

## Cosa aggiunge, e cosa no

M9a ha costruito il meccanismo: tabelle, descrittori, risoluzione. Ma
`vfs_init` non l'ha mai chiamata nessuno, quindi il VFS è codice che gira solo
nei test. M9b lo **accende**.

Il salto è tutto in una riga di `kmain`:

```c
vfs_init(devfs_root());
```

Da lì `/dev/kbd` esiste, e la shell può aprirlo.

## Vincoli globali

Quelli dei blocchi precedenti, più:

- **Il filesystem è di sola lettura per struttura.** Non ci sono `mkdir`,
  `unlink`, `create`: le directory non memorizzano voci, le derivano dal
  registro, quindi non c'è niente in cui aggiungere. Arriva in M11 con il disco.
- **Gli inode sono statici.** `MAX_DEVICES + 2` in tutto: la radice, `/dev`, e
  uno per dispositivo. Nessuna allocazione, e nessuna cache — è l'emendamento
  di M9a, e M9b è la dimostrazione che reggeva.
- **Un dispositivo a caratteri non ha posizione.** `chardev_read` **ignora**
  l'offset che il VFS gli passa: la tastiera non ha un byte numero 12. È la
  prima volta che un `inode_ops->read` scarta legittimamente il suo secondo
  argomento, e vale la pena vederlo.
- **`read` che ritorna 0 continua a significare «adesso non c'è niente».** In
  M9b la conseguenza diventa concreta: `cat` deve sapere quando smettere, e su un
  dispositivo non può dedurlo dal valore di ritorno.

## Struttura dei file al termine di M9b

| File | Responsabilità | Chi |
|---|---|---|
| `include/devfs.h` | `devfs_init`, `devfs_root` | CLAUDE |
| `kernel/devfs.c` | i tre tipi di inode e le loro ops | **WALTER** |
| `kernel/main.c` | `devfs_init()` e `vfs_init(devfs_root())` | CLAUDE |
| `kernel/shell.c` | i comandi `ls` e `cat` | **WALTER** |
| `kernel/selftest.c` | 10 controlli sull'albero vero | CLAUDE |
| `tests/shell.sh` | `ls /dev` e `cat /dev/kbd` | CLAUDE |

## L'interfaccia

```c
/* ---- include/devfs.h ---- */

/* Costruisce i tre tipi di inode a partire dal registro dei dispositivi.

   Va chiamata DOPO tutte le *_init() dei driver, perche' legge il registro che
   loro riempiono: e' l'opposto del vincolo di device_init(), che deve venire
   prima. In kmain finisce quindi in fondo, subito prima di vfs_init. */
void devfs_init(void);

/* L'inode della radice, da passare a vfs_init. Ritorna 0 se devfs_init non e'
   ancora stata chiamata — cosi' l'ordine sbagliato si vede subito invece di
   produrre un VFS con una radice non inizializzata. */
struct inode *devfs_root(void);
```

Due funzioni sole. Tutto il resto sono `static`: tre insiemi di `inode_ops` e gli
inode stessi.

## I tre tipi di inode

```text
                    type        ops              priv           ino
/                   DIR         ops_root         —              1
/dev                DIR         ops_dev          —              2
/dev/<nome>         CHARDEV     ops_chardev      struct device* 3, 4, 5…
```

`priv` finalmente serve, ed è esattamente il caso per cui esisteva: l'inode di un
dispositivo deve ricordare **quale** dispositivo è, e lo fa tenendo il puntatore
alla sua voce nel registro. Senza `priv`, `chardev_read` non saprebbe a chi
girare la chiamata.

Numeri di inode: `1` per la radice e `2` per `/dev` sono una scelta, non un
obbligo — su minix la radice è l'inode 1, e in M11 useremo quella convenzione per
davvero. Qui servono solo a far sì che `ls` mostri numeri distinti.

## Il ponte, che è metà della milestone

```text
    VFS                        devfs                     M8
    ─────────────────────────────────────────────────────────────
    ops->read(inode, off, buf, n)
                  │
                  └──> chardev_read
                            │  ino->priv e' il struct device
                            │  l'offset si SCARTA
                            └──> d->read(d, buf, n)
                                       │
                                       └──> kbd_dev_read
                                                 └──> keyboard_getchar
```

Cinque livelli fra il tasto premuto e `cat` che lo stampa, e **ognuno non sa cosa
c'è sotto**. È il costo dell'astrazione, ed è anche la ragione per cui in M15 un
processo utente potrà leggere la tastiera senza sapere che esiste un IRQ.

## `cat` e la fine del file, che è il punto interessante

`cat` deve sapere quando smettere, e i due casi sono diversi **per natura**, non
per convenzione:

| | come finisce |
|---|---|
| un file regolare | `read` dà 0 perché la posizione ha raggiunto `size` |
| un dispositivo | non finisce **mai**: 0 significa «adesso niente» |

Su un dispositivo il valore di ritorno non basta, e non è una mancanza del
progetto: una tastiera non ha una fine. Quindi `cat` deve **guardare il tipo
dell'inode** e comportarsi di conseguenza:

```text
INODE_FILE      leggi finche' read da' 0
INODE_CHARDEV   leggi finche' vedi un '\n', poi smetti
INODE_DIR       errore: il contenuto di una directory si guarda con ls
```

E per guardare il tipo serve `vfs_resolve`, perché da un descrittore non si
risale all'inode — non c'è `fstat`. È il chiamante che in M9a mancava.

## I task

### Task 1 [CLAUDE]: header e i controlli in VM

Scrivo `include/devfs.h`, i dieci self-check e le aggiunte a `tests/shell.sh`.

Il kernel continua a compilare invariato — nessuno chiama ancora niente — e il
rosso è `tests/shell.sh`, che cerca l'output di `ls` e `cat` che non esistono.

I dieci self-check, che girano sull'albero **vero** e non su quello finto:

- `devfs_root()` non è nullo dopo `devfs_init()`
- la radice è una `INODE_DIR`
- `vfs_resolve("/dev")` riesce ed è una directory
- `vfs_resolve("/dev/kbd")` riesce
- `/dev/kbd` è una `INODE_CHARDEV`
- `/dev/kbd` ha major 13 e minor 64 — gli stessi del registro
- `/dev/console` esiste ed è un chardev
- `vfs_resolve("/dev/nonesiste")` fallisce
- `readdir` su `/dev` elenca tre voci
- `vfs_open("/dev/kbd")` più `vfs_read` a ring vuoto dà 0 e non tocca il buffer

L'ultimo è il controllo di fine catena: attraversa VFS, devfs, device layer e
driver, e conferma che i cinque livelli combaciano.

**Verifica:** `make` compila, i 58 self-check di prima passano, `tests/shell.sh`
fallisce sui nuovi controlli. Nessun commit.

---

### Task 2 [WALTER]: `kernel/devfs.c`

Otto funzioni: le due pubbliche — `devfs_init` e `devfs_root` — e i tre insiemi
di `inode_ops`, tutti `static`:

```text
ops_root      root_lookup      root_readdir
ops_dev       dev_lookup       dev_readdir
ops_chardev   chardev_read     chardev_write
```

Le schede sono nel companion, punti da 1 a 8; qui le tre cose da vedere prima.

**Gli inode dei dispositivi si costruiscono una volta**, in `devfs_init`, con un
ciclo su `device_count()`. Non si generano a ogni `lookup`: `lookup` restituisce
un **puntatore**, e un puntatore deve puntare a qualcosa che sopravvive alla
chiamata.

**`dev_lookup` scorre il registro con `device_at`, non con `device_find`.** Serve
l'**indice**, perché `ino_devices[i]` corrisponde a `device_at(i)`, e
`device_find` restituisce il dispositivo senza dire dov'era. Le due funzioni
fanno lavori simili con esiti diversi, e questa è la volta in cui serve l'altra.

**`chardev_read` scarta l'offset.** Un dispositivo non ha una posizione: la
tastiera non ha un byte numero 12. Il VFS gliela passa comunque, perché la firma
è unica per tutti i tipi di inode, e il dispositivo la ignora — con un `(void)off`
esplicito, che dice al lettore che è una scelta e non una dimenticanza.

**Verifica:** `make` compila e linka. I self-check restano rossi finché il Task 3
non collega `kmain`.

---

### Task 3 [CLAUDE]: accendere il VFS

Due righe in `kmain`, dopo `keyboard_init()` e prima di `task_init()`:

```c
devfs_init();
vfs_init(devfs_root());
```

L'ordine è vincolato da entrambi i lati: `devfs_init` legge il registro, quindi
va **dopo** i driver; `vfs_init` prende la radice da `devfs`, quindi va **dopo**
`devfs_init`. È l'opposto di `device_init()`, che deve venire prima di tutti — e
i due vincoli insieme incorniciano le `*_init()` dei driver.

**Verifica:** i dieci self-check nuovi passano. `make test` ancora rosso su
`shell.sh`.

---

### Task 4 [WALTER]: `ls` e `cat`

Due voci nella tabella dei comandi. Schede 9 e 10 del companion.

`shell_ls` — `ls [path]`, con `/` come default, in due forme come `devs`: su una
directory enumera con `vfs_readdir`, su qualunque altra cosa mostra quella sola
voce con il suo tipo. Il ciclo si ferma quando `readdir` dà **0**, non `-1`: i
due valori sono distinti apposta.

`shell_cat` — `cat <path>`, con la distinzione fra file e dispositivo descritta
sopra. È la funzione che rende vera la frase «tutto è un file», e in M11 leggerà
file veri da un disco minix **senza una riga di modifica**.

Schede 9 e 10 del companion.

**Verifica:** `make test` verde. `ls /dev` elenca tre dispositivi, e
`cat /dev/kbd` restituisce la riga che digiti.

---

### Task 5 [CLAUDE]: chiusura di M9b

`README.md` e `CLAUDE.md` — e stavolta il README cambia davvero, perché c'è
qualcosa di nuovo da mostrare al prompt. Poi **propongo** il commit,
`M9b: devfs, /dev sopra il registro dei dispositivi`, eseguendolo se confermi.

## Come si sbaglia

**Generare gli inode dentro `lookup`.** Restituiresti un puntatore a una
variabile locale, che sparisce al ritorno. Il chiamante lo userebbe subito dopo,
e leggerebbe stack di qualcun altro — funzionando quasi sempre, che è il modo
peggiore di essere rotti.

**Costruire gli inode dei dispositivi prima che i driver si iscrivano.**
`device_count()` darebbe 0 e `/dev` sarebbe vuota, senza nessun errore.

**Far avanzare la posizione su un dispositivo.** Il VFS lo fa già — `f->off +=
letti` — e va bene così: la posizione esiste nel file aperto anche per un
dispositivo, semplicemente nessuno la guarda. Ma se anche `chardev_read` la
usasse, la seconda lettura partirebbe da un offset che per la tastiera non
significa niente.

**`cat` su un dispositivo che aspetta `read` == 0.** Non arriva mai: zero
significa «adesso niente», e la shell resterebbe piantata senza modo di uscire —
non ci sono segnali, quindi nemmeno Ctrl-C.

**`ls` che chiama `read` invece di `readdir`.** `read` su una directory dà −1 per
contratto, e in M11 darebbe byte grezzi di strutture minix.

## Lettura di accompagnamento

**Linux 0.01**, `fs/namei.c` e `fs/inode.c`: lì il filesystem è uno solo e minix,
quindi il livello `inode_ops` non esiste ancora — Linus lo aggiungerà nel 1992
con il VFS. Interessante vedere il *prima*: le funzioni chiamano direttamente le
routine di minix, e aggiungere un secondo filesystem avrebbe voluto dire un `if`
in ogni punto.

**xv6**, `console.c`: la sua `devsw[]` indicizzata per major è il nostro ponte,
in venti righe, con la stessa idea — un dispositivo è una coppia di funzioni.
