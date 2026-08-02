# M11c — mount vero: la tabella di mount nel VFS

> **Per chi esegue:** i passi hanno checkbox (`- [ ]`). I file di Walter
> (`kernel/vfs.c`, `kernel/minixfs.c`, `kernel/shell.c`) sono descritti per
> **contratto, struttura e trappole**, non con l'implementazione: è la regola di
> `CLAUDE.md`. I file di Claude (`tests/**`, `tools/**`, `include/*.h`,
> `kernel/main.c`, `kernel/selftest.c`) hanno il codice per intero.

**Obiettivo:** spostare l'innesto da `minixfs.c` al VFS, dove appartiene, così
che montare un filesystem non richieda di modificare il filesystem che possiede
il punto di innesto.

**Architettura:** una tabella `punto → root` dentro `vfs.c`, e una sostituzione
di **una riga** dentro `vfs_resolve`, subito dopo ogni `lookup`. Il punto di
mount è una directory che **esiste davvero** sull'immagine minix — è quello che
fa Unix, e la conseguenza migliore è che `readdir` non deve sapere niente del
mount, perché il nome ce l'ha già dal disco. `minixfs_graft` sparisce con le sue
due diramazioni.

**Tecnologie:** C freestanding i386, `mkfs.minix`/`mount` come riferimento,
QEMU, i test host già in piedi.

## Vincoli globali

Copiati dallo spec `docs/superpowers/specs/2026-07-29-waltex-userland-design.md`
e da `CLAUDE.md`. Valgono per ogni task.

- **Niente libc.** Tipi da `include/types.h`. Se serve `strcmp`, sta in
  `kernel/memory.c`.
- **Nessuna allocazione dinamica fino a M12.** La tabella di mount è un array
  statico a capacità fissa, come `MAX_DEVICES`, `MAX_INODES`, `MAX_OPEN_FILES`.
- **`assert()` sempre attivo**, nessun `NDEBUG`.
- **Firme POSIX.** `mount` è la **syscall 21 di Linux i386** (`umount2` è la 52):
  `vfs_mount(const char *path, struct inode *root)` è la forma ridotta che in
  M14 diventa una syscall senza cambiare verso agli argomenti.
- **Errori come ritorno negativo**, mai un `-1` generico dove esiste un valore
  Linux. Qui: `-1` è ammesso perché tutto il VFS attuale usa `-1`; **non
  introdurre** codici nuovi in questa milestone, sarebbe un'incoerenza a metà.
- **Puntatore nullo in `inode_ops` uguale «non supportata»**, non «errore».
- **`kprintf` scrive su VGA e COM1.** I test leggono la seriale: nessun output
  diagnostico solo su VGA.
- **Dopo ogni modifica al kernel si esegue `make test`**, non `make`.
- **Una milestone alla volta.** Niente `umount`, niente `..` che attraversa il
  confine, niente `/proc`: sono M16 e oltre.

---

## Perché questa milestone esiste

`minixfs_graft` funziona. Il problema non è che sia rotta, è **dove sta**:

```text
oggi                                    dopo
────                                    ────
kmain:                                  kmain:
  minixfs_init(hdb)                       minixfs_init(hdb)
  minixfs_graft("dev", devfs_devdir())    vfs_init(minixfs_root())
  vfs_init(minixfs_root())                vfs_mount("/dev", devfs_devdir())

per montare walterfs sotto /mnt:        per montare walterfs sotto /mnt:
  si modifica minixfs.c                   vfs_mount("/mnt", walterfs_root())
```

Il difetto si misura così: **oggi, per montare qualcosa, devi modificare il
filesystem che possiede il punto di innesto.** È al contrario. E il sintomo
visibile è che `minix_lookup` e `minix_readdir` devono restare d'accordo
sull'innesto — cosa che in `CLAUDE.md` è scritta come una trappola, e invece è
il sintomo di una responsabilità nel posto sbagliato.

### La cosa che non si può fare, e perché

L'idea naturale — «prendo l'inode di `/dev` e ci scrivo dentro `ops` e `priv` di
devfs» — non regge, per tre ragioni indipendenti:

- quell'inode vive nella **cache di minixfs** e viene riletto dal disco: alla
  prima `inode_carica` che riusa lo slot, il mount evapora;
- `umount` non avrebbe niente a cui tornare;
- resterebbe `ino` di minix su un inode con `ops` di devfs, cioè due filesystem
  mescolati in una struct.

Quindi **una tabella fuori dai due filesystem**, e la sostituzione dentro il
risolutore. Lo scambio non è nei dati, è nella funzione — esattamente come
l'albero non è nei dati ma nella `lookup`.

### La chiave della tabella è il PUNTATORE, non `ino`

È la nota di M11a che torna a presentare il conto: in `ls /`, `dev` e
`hello.txt` compaiono **entrambi con il numero 2**, perché i numeri di inode
sono unici *dentro* un filesystem e non fra filesystem. Una tabella indicizzata
per `ino` monterebbe due cose diverse sullo stesso posto.

### Il punto di mount deve esistere sul disco

In Unix `mount` non aggiunge un nome: ne **copre** uno. `mount /dev/sdb1 /mnt`
fallisce con `ENOENT` se `/mnt` non c'è.

Adottarlo qui costa una riga in `tools/mkminix.sh` e paga subito:
**`minix_readdir` non ha più bisogno di sapere che i mount esistono**, perché il
nome `dev` glielo dà il disco. Metà del problema sparisce invece di spostarsi di
un livello.

E per questo non si crea il mountpoint automaticamente se manca: un mountpoint
che appare dal nulla nasconde un errore di battitura, e `ENOENT` è la risposta
giusta.

## Struttura dei file

| file | chi | cosa cambia |
|---|---|---|
| `include/vfs.h` | Claude | `MAX_MOUNTS`, la dichiarazione di `vfs_mount` |
| `kernel/vfs.c` | **Walter** | la tabella, `risolvi_mount`, `vfs_mount`, due righe in `vfs_resolve`, l'azzeramento in `vfs_init` |
| `include/minixfs.h` | Claude | via la dichiarazione di `minixfs_graft` |
| `kernel/minixfs.c` | **Walter** | via lo slot, via le due diramazioni in `lookup` e `readdir`, via `minixfs_graft` |
| `kernel/main.c` | Claude | `vfs_init` poi `vfs_mount`, marker nuovo |
| `kernel/selftest.c` | Claude | i controlli dell'innesto riscritti in termini di mount |
| `tools/mkminix.sh` | Claude | `mkdir /dev` sull'immagine |
| `tests/data/minix.img` | Claude | rigenerata e ricommittata |
| `tests/host/test_vfs.c` | Claude | il gruppo nuovo, 17 controlli |
| `tests/host/test_minixfs.c` | Claude | via i 6 controlli dell'innesto, 3 conteggi aggiornati |
| `tests/smoke.sh` | Claude | il marker |

Fuori: `tests/shell.sh` non cambia — `ls /` deve continuare a mostrare `dev`, e
il fatto che il controllo passi **provando una cosa diversa** è la conferma che
il taglio è nel punto giusto.

## Il conto dei test, atteso

| | prima | dopo |
|---|---|---|
| host | 419 | 430 (+17 in `test_vfs`, −6 in `test_minixfs`) |
| self-check | 108 | 109 |
| marker | 9 | 9 (uno cambia testo) |

Numeri **da misurare a fine milestone**, non da ricordare:

```bash
make -C tests/host -s run | grep -cE "ok +--"
```

---

## Task 1 — la tabella di mount nel VFS

Tutto sull'host. Il kernel non cambia comportamento: continua a usare
`minixfs_graft`, e `vfs_mount` esiste senza che nessuno la chiami. È
deliberato — la meccanica si prova per intero prima che qualcosa dipenda da lei.

**File:**
- Modifica: `include/vfs.h`
- Modifica: `kernel/vfs.c` (la tabella, `risolvi_mount`, `vfs_mount`,
  `vfs_resolve`, `vfs_init`)
- Test: `tests/host/test_vfs.c`

**Interfacce:**
- Consuma: `vfs_init(struct inode *)`, `vfs_resolve(const char *, struct inode **)`,
  `vfs_readdir(int, int, char *, uint32_t *)` — già esistenti.
- Produce: `int vfs_mount(const char *path, struct inode *root)` e la costante
  `MAX_MOUNTS`. Task 2 li consuma da `kernel/main.c`.

- [ ] **Passo 1: la dichiarazione nell'header**

In `include/vfs.h`, accanto a `TASK_FDS`:

```c
#define MAX_MOUNTS       4
```

e dopo `vfs_mkdir`:

```c
/* Monta un filesystem sopra una directory che ESISTE GIA'. E' la syscall 21 di
   Linux i386, nella forma ridotta che ci serve: niente tipo di filesystem e
   niente flag, perche' il chiamante ci passa gia' la radice pronta.

   0, oppure -1 se il path non si risolve, se non e' una directory, se root e'
   nullo o non e' una directory, o se la tabella e' piena.

   Non crea il punto di mount se manca, ed e' una scelta: in Unix "mount /x"
   con /x inesistente da' ENOENT, e un mountpoint che appare dal nulla
   nasconderebbe un errore di battitura.

   Non modifica NESSUNO dei due filesystem. Il montante non sa di essere
   montato e il montato non sa dove: la relazione vive solo in questa tabella,
   ed e' cio' che permette a minixfs.c di non sapere che i mount esistano.

   Va chiamata DOPO vfs_init, per due ragioni indipendenti: vfs_init azzera la
   tabella, e questa funzione risolve un path, cosa che senza radice fallisce.

   Non esiste vfs_umount, e non e' una dimenticanza: niente lo chiamerebbe, e
   smontare per davvero vuole sapere se ci sono file aperti sotto — cioe' i
   refcount, che arrivano in M16. */
int vfs_mount(const char *path, struct inode *root);
```

- [ ] **Passo 2: scrivere il gruppo di test host (fallisce)**

In `tests/host/test_vfs.c`, un albero secondario da montare e il gruppo di
controlli. L'albero finto esistente ha `/d` (directory, contiene `b`) e `/a`
(file): `/d` è il punto di mount, e `b` è ciò che il mount deve **coprire**.

Prima delle funzioni di test, accanto agli altri inode finti:

```c
/* ---- M11c: un secondo albero, da montare -----------------------------------

   Una radice con dentro "m", piu' TRE radici spoglie che servono a un controllo
   solo: riempire la tabella. Sono deliberatamente minime — quello che si prova
   qui e' il MECCANISMO del mount, non un filesystem.

   Quattro radici montabili e non due, perche' MAX_MOUNTS vale 4 e il controllo
   sulla tabella piena vuole quattro mount che riescano davvero. */
static struct inode ino_mroot, ino_m, ino_mroot2, ino_mroot3, ino_mroot4;

static struct voce voci_mroot[]  = { { "m", &ino_m } };
static struct voce voci_mroot2[] = { { "m2", &ino_m } };

static struct dati_dir dati_mroot  = { voci_mroot,  1 };
static struct dati_dir dati_mroot2 = { voci_mroot2, 1 };
```

`struct voce` e `struct dati_dir` sono i nomi già usati nell'albero finto di
`test_vfs.c` — verificati, non ricordati.

Dentro `albero()`, in fondo prima di `vfs_init(&ino_root)`:

```c
    ino_mroot.ino = 100; ino_mroot.type = INODE_DIR;
    ino_mroot.size = 0; ino_mroot.ops = &ops_dir; ino_mroot.priv = &dati_mroot;

    ino_m.ino = 101; ino_m.type = INODE_FILE;
    ino_m.size = 3; ino_m.ops = &ops_file; ino_m.priv = (void *)"mmm";

    ino_mroot2.ino = 102; ino_mroot2.type = INODE_DIR;
    ino_mroot2.size = 0; ino_mroot2.ops = &ops_dir;
    ino_mroot2.priv = &dati_mroot2;

    ino_mroot3.ino = 103; ino_mroot3.type = INODE_DIR;
    ino_mroot3.size = 0; ino_mroot3.ops = &ops_dir;
    ino_mroot3.priv = &dati_mroot2;

    ino_mroot4.ino = 104; ino_mroot4.type = INODE_DIR;
    ino_mroot4.size = 0; ino_mroot4.ops = &ops_dir;
    ino_mroot4.priv = &dati_mroot2;
```

E il gruppo, da chiamare da `main()` dopo `test_readdir()`:

```c
/* ---- M11c: il mount ---------------------------------------------------------

   Diciassette controlli, e il piu' importante e' l'ottavo: dopo il mount,
   "/d/b" deve FALLIRE. Un mount che aggiunge senza coprire non e' un mount,
   e' quello che faceva minixfs_graft. */
static void test_mount(void)
{
    struct inode *p;
    char nome[VFS_NAME_MAX + 1];
    uint32_t n;
    int fd, r;

    albero();

    /* I rifiuti, prima. Sono la meta' che nessun controllo positivo puo'
       vedere, ed e' la lezione dei tre bug di M9b: un valore di ritorno
       sbagliato sull'insuccesso non ha sintomi finche' qualcuno non ci
       cammina sopra. */
    check("montare su un path inesistente fallisce",
          vfs_mount("/nonesiste", &ino_mroot) == -1);

    check("montare su un file fallisce",
          vfs_mount("/a", &ino_mroot) == -1);

    check("montare una radice nulla fallisce",
          vfs_mount("/d", 0) == -1);

    check("montare una radice che non e' una directory fallisce",
          vfs_mount("/d", &ino_a) == -1);

    /* E dopo quattro rifiuti l'albero deve essere INTATTO: se uno dei quattro
       avesse scritto in tabella prima di controllare, "/d/b" sarebbe gia'
       coperto adesso. */
    check("dopo quattro rifiuti /d/b si risolve ancora",
          vfs_resolve("/d/b", &p) == 0 && p == &ino_db);

    check("il mount riesce", vfs_mount("/d", &ino_mroot) == 0);

    /* L'identita' del PUNTATORE, non del numero. E' la nota di M11a: gli
       inode sono unici dentro un filesystem, non fra filesystem, quindi
       confrontare ino qui non proverebbe niente. */
    check("/d da' esattamente l'inode montato",
          vfs_resolve("/d", &p) == 0 && p == &ino_mroot);

    /* IL controllo. Il mount COPRE: b esiste sotto e non si vede piu'. */
    check("/d/b non si risolve piu': il mount copre",
          vfs_resolve("/d/b", &p) == -1);

    check("/d/m si risolve: il contenuto e' quello montato",
          vfs_resolve("/d/m", &p) == 0 && p == &ino_m);

    /* Il resto dell'albero non si accorge di niente. */
    check("/a non e' cambiato",
          vfs_resolve("/a", &p) == 0 && p == &ino_a);

    check("/ non e' cambiato",
          vfs_resolve("/", &p) == 0 && p == &ino_root);

    /* readdir passa dal fd, quindi dall'inode risolto: deve elencare il
       montato. Se la sostituzione fosse solo dentro lookup e non nel valore
       che vfs_resolve consegna, questo controllo la prenderebbe. */
    fd = vfs_open("/d", O_RDONLY);
    r = (fd >= 0 && vfs_readdir(fd, 0, nome, &n) == 1 && same(nome, "m"));
    if (fd >= 0)
        vfs_close(fd);

    check("readdir su /d elenca le voci del filesystem montato", r);

    /* L'impilamento, che e' anche il solo controllo del ciclo esterno di
       risolvi_mount. Il secondo mount ha come punto la RADICE DEL PRIMO —
       perche' vfs_resolve("/d") ora da' quella — quindi risolvere /d deve
       seguire la catena due volte. Con un ciclo solo si fermerebbe a
       ino_mroot, e il controllo lo vede. */
    check("un secondo mount sullo stesso punto riesce",
          vfs_mount("/d", &ino_mroot2) == 0);

    check("e /d segue la catena fino all'ultimo montato",
          vfs_resolve("/d", &p) == 0 && p == &ino_mroot2);

    /* vfs_init azzera la tabella. Senza, ogni gruppo di controlli
       erediterebbe i mount del precedente — e in kmain un secondo vfs_init
       lascerebbe in piedi mount verso inode di un filesystem smontato. */
    albero();

    check("dopo vfs_init la tabella e' vuota: /d/b torna",
          vfs_resolve("/d/b", &p) == 0 && p == &ino_db);

    /* La tabella si riempie, e il rifiuto e' esplicito invece che silenzioso.

       Quattro RADICI DIVERSE, non quattro volte la stessa: ogni mount ha come
       punto la radice del precedente — perche' vfs_resolve("/d") gliela
       consegna gia' sostituita — quindi ne esce una catena lineare

           d -> mroot -> mroot2 -> mroot3 -> mroot4

       e non un ciclo. Riusando due sole radici a giro si costruirebbe
       mroot->mroot2 e mroot2->mroot, cioe' un ciclo, e il quinto mount
       verrebbe rifiutato da "punto == root" invece che dalla tabella piena:
       il controllo passerebbe per la ragione sbagliata. */
    check("quattro mount riempiono la tabella",
          vfs_mount("/d", &ino_mroot)  == 0 &&
          vfs_mount("/d", &ino_mroot2) == 0 &&
          vfs_mount("/d", &ino_mroot3) == 0 &&
          vfs_mount("/d", &ino_mroot4) == 0);

    check("e il quinto viene rifiutato",
          vfs_mount("/d", &ino_mroot) == -1);
}
```

E in `main()`:

```c
    test_mount();
```

- [ ] **Passo 3: verificare che il test fallisca**

```bash
make -C tests/host test_vfs
```

Atteso: **errore di compilazione**, `implicit declaration of function 'vfs_mount'`
se il Passo 1 non è stato fatto, oppure `undefined reference to 'vfs_mount'` se
l'header c'è e `vfs.c` no. Un errore di link **è** un test che fallisce, ed è la
stessa cosa che in M7 ha scoperto lo `strcmp` mancante.

- [ ] **Passo 4: implementare in `kernel/vfs.c` — file di Walter**

Quattro pezzi. Struttura e contratto qui sotto, il dettaglio funzione per
funzione in `2026-08-02-waltex-m11c-funzioni.md`.

**(a) La tabella**, accanto a `files[]` e `fds[][]`:

```text
struct mount { struct inode *punto; struct inode *root; }
static struct mount mounts[MAX_MOUNTS];
```

`punto == 0` significa slot libero — stessa convenzione di `files[i].inode == 0`
in M9a, e per la stessa ragione: il campo che serve comunque fa da marcatore.

**(b) `vfs_init` azzera anche questa tabella.** Una riga, e senza di lei il
Passo 2 fallisce al controllo «dopo vfs_init la tabella è vuota».

**(c) `risolvi_mount(struct inode *ino) -> struct inode *`**, `static`, e la
forma è due cicli annidati:

```text
ripeti al massimo MAX_MOUNTS volte:
    cerca uno slot occupato con punto == ino
    se non c'e':  esci
    ino = quello slot .root
ritorna ino
```

Il ciclo esterno serve all'impilamento e **il tetto non è pedanteria**: montare
A su B e poi B su A costruisce un ciclo, e un ciclo senza tetto è la stessa
morte silenziosa di `while (status & BSY)` su un canale ATA vuoto. È la regola
di M10 applicata a un ciclo invece che a un'attesa.

**(d) Due righe in `vfs_resolve`.** La prima sull'inizializzazione di `current`
— così anche `vfs_resolve("/")` rispetta un eventuale mount sulla radice — e la
seconda **subito dopo che `lookup` ha risposto 0**, prima che `current` si
sposti:

```text
if (current->ops->lookup(current, nome, &prossimo) < 0)
    return -1;

prossimo = risolvi_mount(prossimo);      <-- qui
current  = prossimo;
```

Il posto è obbligato: **prima** del controllo `current->type != INODE_DIR` del
giro seguente, perché è il filesystem *montato* a dover essere una directory,
non il punto coperto.

**(e) `vfs_mount(const char *path, struct inode *radice)`**, e l'ordine dei
controlli è la lezione di M11b — **non si tocca la tabella finché non si sa che
l'operazione può riuscire**:

```text
1.  radice == 0                       -> -1
2.  radice->type != INODE_DIR         -> -1
3.  vfs_resolve(path, &punto) < 0     -> -1
4.  punto->type != INODE_DIR          -> -1
5.  punto == radice                   -> -1     (il ciclo di lunghezza uno)
6.  cerca uno slot libero; non c'e'   -> -1
7.  SOLO ORA: mounts[i] = { punto, radice };  ritorna 0
```

Il parametro si chiama **`radice`, non `root`**, e non è pignoleria: in `vfs.c`
esiste già uno `static struct inode *root`, e un parametro omonimo lo ombra.
Ogni riga scritta pensando alla globale farebbe la cosa sbagliata **in
silenzio** — il compilatore non ha niente da dire, i due hanno lo stesso tipo.
Nell'header il parametro si chiama `root` perché lì è solo documentazione e la
firma è quella POSIX.

- [ ] **Passo 5: verificare che i test passino**

```bash
make -C tests/host -s run
```

Atteso: `test_vfs` verde con **17 controlli in più**, e i 75 di prima
**invariati** — è il controllo che dice che M11c non ha rotto M9a. Nessun altro
binario cambia.

- [ ] **Passo 6: il kernel compila ancora e i test girano**

```bash
make test
```

Atteso: tutto verde e **invariato** nella VM. `vfs_mount` esiste e nessuno la
chiama: `-Wall -Wextra` non protesta per una funzione non `static` mai usata.

- [ ] **Passo 7: commit**

```bash
git add include/vfs.h kernel/vfs.c tests/host/test_vfs.c
git commit -m "M11c: tabella di mount nel VFS, vfs_mount"
```

---

## Task 2 — lo scambio: `/dev` sul disco, via la graft

**Questo task non si può spezzare**, e vale la pena sapere perché prima di
provarci. Con `/dev` sull'immagine *e* la graft ancora attiva, `minix_readdir`
elenca la voce del disco **e** quella dell'innesto: `ls /` mostrerebbe `dev` due
volte. Immagine, rimozione della graft e passaggio a `vfs_mount` sono un solo
stato coerente.

**File:**
- Modifica: `tools/mkminix.sh`
- Modifica: `tests/data/minix.img` (rigenerata, binaria)
- Modifica: `include/minixfs.h`, `kernel/minixfs.c`
- Modifica: `kernel/main.c`, `kernel/selftest.c`, `tests/smoke.sh`
- Modifica: `tests/host/test_minixfs.c`

**Interfacce:**
- Consuma: `vfs_mount(const char *path, struct inode *root)` e `MAX_MOUNTS` dal
  Task 1; `devfs_devdir()` e `minixfs_root()`, già esistenti.
- Produce: niente di nuovo. Toglie `int minixfs_graft(const char *, struct inode *)`.

- [ ] **Passo 1: `/dev` sull'immagine**

In `tools/mkminix.sh`, dentro il blocco `sudo sh -c`, **come ultima riga** —
l'ordine conta: creando `dev` per ultimo, tutti i numeri di inode esistenti
restano dove sono, e in particolare `hello.txt` resta l'inode 2, su cui c'è un
controllo host.

```sh
    mkdir -p '$MNT/dev'
```

E nel commento sopra il blocco, dopo la riga di `vuoto.txt`:

```sh
#   dev/         directory VUOTA. Non e' un file di prova: e' il PUNTO DI MOUNT
#                di devfs. Esiste sul disco perche' e' cosi' che funziona mount
#                in Unix — si copre una directory che c'e' gia', non si aggiunge
#                un nome — e il guadagno e' che minix_readdir non deve sapere
#                niente dei mount: il nome "dev" glielo da' il disco.
```

- [ ] **Passo 2: rigenerare l'immagine e verificarla a mano**

```bash
./tools/mkminix.sh tests/data/minix.img
fsck.minix -f tests/data/minix.img
```

Atteso: `fsck` esce con **0**. E il controllo di coerenza di M11a rifatto sui
byte nuovi, perché un'immagine nuova non si dà per buona:

```bash
od -An -tu2 -j1024 -N16 tests/data/minix.img
```

Atteso: `ninodes s_nzones s_imap_blocks s_zmap_blocks s_firstdatazone
s_log_zone_size ...` — e `2 + imap + zmap` più `ninodes * 32` arrotondato a
blocchi deve dare esattamente `s_firstdatazone`. Sull'immagine di M11a faceva
`2+1+1 = 4`, `96*32 = 3` blocchi, `4+3 = 7`. Una directory in più non cambia il
numero di inode dichiarati, quindi il conto deve tornare **identico**.

```bash
git add tests/data/minix.img && git diff --cached --stat
```

Atteso: un file binario cambiato, poche centinaia di byte di delta.

- [ ] **Passo 3: aggiornare i conteggi in `tests/host/test_minixfs.c`**

Tre controlli guardano la dimensione della radice, e adesso le voci sono otto.
`112` diventa `128` (8 × 16 byte), `sette` diventa `otto`, e la lista dei nomi
attesi guadagna `dev` **in fondo** — è l'ordine in cui `mkdir` l'ha scritta.

```c
    check("la radice misura 128 byte, cioe' otto voci", root->size == 128);
```

```c
    check("readdir della radice da' otto voci", n == 8);
```

Nel controllo dell'ordine, la lista attesa diventa:

```c
    static const char *attese[] = {
        ".", "..", "hello.txt", "etc", "grande.txt", "enorme.txt",
        "vuoto.txt", "dev"
    };
```

> **Se l'ordine reale differisce**, non si aggiusta il test a occhio: si legge
> con `od -An -c -j$((7 * 1024)) -N 128 tests/data/minix.img` e si scrive quello
> che c'è. Il test deve dire cosa contiene il disco, non cosa speravamo.

E il controllo «oltre l'ultima voce readdir dà 0» va spostato di un indice se
usa un numero letterale.

- [ ] **Passo 4: togliere i sei controlli dell'innesto da `test_minixfs.c`**

Il gruppo che comincia con `"innestare prima del mount o con nome troppo lungo
e' rifiutato"` e finisce con `"l'innesto non e' sul disco: dopo un rimount non
c'e' piu'"` — sei `check`, più le chiamate a `minixfs_graft` e l'eventuale
inode finto che gli serviva.

**Non si sostituiscono con controlli equivalenti sul mount**: `vfs_mount` è già
coperta dai diciassette del Task 1, e `test_minixfs` non linka `vfs.c`. Duplicarli
qui vorrebbe dire aggiungere una dipendenza per riprovare la stessa cosa.

Attenzione al controllo `"il rimontaggio riesce"` subito dopo: **resta**, perché
serve al gruppo della scrittura che viene dopo. Solo i sei dell'innesto se ne
vanno.

- [ ] **Passo 5: verificare che i test host falliscano nel modo giusto**

```bash
make -C tests/host -s run
```

Atteso: `test_minixfs` compila e passa con **6 controlli in meno**; i tre
conteggi aggiornati sono verdi. Se `"la radice misura 128 byte"` fallisce,
l'immagine non è stata rigenerata — non si aggiusta il numero, si rifà
l'immagine.

- [ ] **Passo 6: togliere `minixfs_graft` — `include/minixfs.h`**

Via l'intero blocco di commento e la dichiarazione
`int minixfs_graft(const char *nome, struct inode *root);`.

Al suo posto, dove stava, una riga che spiega l'assenza — perché è il genere di
funzione che qualcuno riscriverebbe:

```c
/* Non c'e' nessuna minixfs_graft, e l'assenza e' il punto di M11c: montare non
   e' affare del filesystem montante. La tabella sta in vfs.c e la sostituzione
   in vfs_resolve, quindi minixfs non sa che i mount esistano — che e' anche il
   motivo per cui /dev e' una directory VERA sull'immagine invece di un nome
   inventato dalla lookup. */
```

- [ ] **Passo 7: togliere `minixfs_graft` — `kernel/minixfs.c`, file di Walter**

Quattro rimozioni, e nessuna aggiunta:

1. la `struct` anonima `innesto` con il suo commento (intorno a
   [`minixfs.c:118`](../../../kernel/minixfs.c#L118));
2. la riga `memset(&innesto, 0, sizeof(innesto));` in `minixfs_init`
   ([`minixfs.c:373`](../../../kernel/minixfs.c#L373));
3. la diramazione in `minix_lookup` ([`minixfs.c:1089-1095`](../../../kernel/minixfs.c#L1089));
4. la diramazione in `minix_readdir` ([`minixfs.c:1159-1167`](../../../kernel/minixfs.c#L1159));
5. la funzione `minixfs_graft` per intero ([`minixfs.c:1178`](../../../kernel/minixfs.c#L1178)).

**Restano** i due commenti su `dir` che «potrebbe essere l'inode dell'innesto,
che appartiene a devfs» in `minix_write` e `minix_create` — ma vanno
**riformulati**, perché adesso la ragione è diversa e più forte: il VFS può
consegnare a chiunque un inode che non è di minix, e il controllo che l'inode
sia nostro non dipende più dall'esistenza della graft.

- [ ] **Passo 8: `kmain` monta invece di innestare — `kernel/main.c`**

Il blocco intorno a [`main.c:90`](../../../kernel/main.c#L90) diventa:

```c
   /* La radice viene dal DISCO, e /dev si MONTA sopra una directory che sul
      disco esiste gia'. E' la forma a cui il blocco punta: in M16 init
      carichera' /bin/sh da un path assoluto, e ogni giorno passato con
      /mnt/bin/sh sarebbe un giorno di path che poi cambiano.

      L'ORDINE E' OBBLIGATO, per due ragioni indipendenti: vfs_init azzera la
      tabella di mount, e vfs_mount risolve un path — cosa che senza radice
      fallisce. Fino a M11b era l'opposto, perche' la graft viveva dentro
      minixfs e vfs_init veniva per ultima.

      Il ripiego non e' cerimonia. Senza disco, o con un'immagine che non e'
      minix, il kernel resta usabile — /dev c'e' — e il motivo si legge sulla
      seriale invece di presentarsi come una radice muta in cui ogni resolve
      fallisce.

      E il mount fallito NON fa piu' ripiegare su devfs: la radice su disco
      resta buona, /dev resta la directory vuota che e' sull'immagine, e il
      marker diverso dice cosa e' successo. Perdere il filesystem intero
      perche' un mount non e' andato sarebbe una reazione sproporzionata. */
   if (minixfs_init(ata_drive(1)) == 0) {
      vfs_init(minixfs_root());

      if (vfs_mount("/dev", devfs_devdir()) == 0) {
         kprintf("waltex: radice minix su hdb, /dev montata\n");
      } else {
         kprintf("waltex: radice minix su hdb, mount di /dev fallito\n");
      }
   } else {
      vfs_init(devfs_root());
      kprintf("waltex: nessun filesystem su hdb, radice su devfs\n");
   }
```

- [ ] **Passo 9: il marker in `tests/smoke.sh`**

Nella riga `MARKERS=(...)`, `"waltex: radice minix su hdb, /dev innestata"`
diventa:

```sh
"waltex: radice minix su hdb, /dev montata"
```

- [ ] **Passo 10: i self-check dell'innesto, riscritti — `kernel/selftest.c`**

In `check_minix`, i due controlli intorno a
[`selftest.c:625`](../../../kernel/selftest.c#L625) vanno sostituiti. La ragione
è precisa e vale la pena scriverla nel codice: **`/dev` adesso esiste sul disco
come directory vuota, quindi risolverlo non prova più niente sul mount.**

Servono tre controlli al posto di due:

```c
    /* IL MOUNT, e i tre controlli non sono ridondanti fra loro.

       Attenzione: dopo M11c "/dev" esiste sull'immagine come directory VUOTA,
       quindi vfs_resolve("/dev") riesce anche a mount fallito — e' esattamente
       il caso in cui il vecchio controllo "l'innesto c'e'" avrebbe mentito.
       Cio' che prova il mount e' l'IDENTITA' del puntatore. */
    report("/dev e' esattamente l'inode di devfs, cioe' il mount ha coperto",
           vfs_resolve("/dev", &ino) == 0 && ino == devfs_devdir());

    /* E il contenuto, che sul disco non c'e': /dev sull'immagine e' vuota. */
    report("/dev/kbd si risolve, con la radice su minix",
           vfs_resolve("/dev/kbd", &ino) == 0 && ino->type == INODE_CHARDEV);

    /* Il mount non ha nascosto i vicini: e' il controllo che prende una
       sostituzione fatta sull'inode sbagliato. */
    report("il mount non ha coperto /etc",
           vfs_resolve("/etc", &ino) == 0 && ino->type == INODE_DIR);
```

`selftest.c` include già `devfs.h` (riga 13), quindi `devfs_devdir()` è
disponibile senza aggiungere niente.

- [ ] **Passo 11: `make test`**

```bash
make test
```

Atteso: tutto verde. In particolare:
- `tests/shell.sh` **invariato** — `ls /` mostra ancora `dev`, ma adesso perché
  è sul disco, e `ls /dev` mostra ancora i tre dispositivi, ma adesso attraverso
  la tabella di mount. Lo stesso controllo prova una cosa diversa, ed è la
  conferma che il taglio è nel punto giusto;
- `tests/minixwrite.sh` **invariato**, e con lui `fsck.minix -f` che esce 0
  dopo che il kernel ha scritto su un'immagine che ora ha una directory in più.

Se `smoke.sh` fallisce sul marker, il Passo 9 non è stato fatto. Se `shell.sh`
fallisce su `ls /dev`, la sostituzione in `vfs_resolve` è nel posto sbagliato —
si guarda il Task 1 Passo 4 (d).

- [ ] **Passo 12: la prova che il taglio funziona davvero**

Non è un test automatico, è il controllo che dà senso alla milestone. Nella VM:

```text
waltex> ls /
waltex> ls /dev
waltex> cat /dev/kbd
```

e poi, **da fuori**, la verifica che nessuno se ne accorga dal disco:

```bash
mkdir -p /tmp/m && sudo mount -o loop,ro build/minix.img /tmp/m && ls -la /tmp/m/dev; sudo umount /tmp/m
```

Atteso: `/tmp/m/dev` è una directory **vuota** — `.` e `..` e basta. Il mount
non ha scritto niente sul disco, che è tutta la differenza fra montare e creare.

- [ ] **Passo 13: commit**

```bash
git add tools/mkminix.sh tests/data/minix.img include/minixfs.h kernel/minixfs.c \
        kernel/main.c kernel/selftest.c tests/smoke.sh tests/host/test_minixfs.c
git commit -m "M11c: /dev e' un mount vero, minixfs_graft rimossa"
```

---

## Task 3 — la documentazione

**File:**
- Modifica: `CLAUDE.md`
- Modifica: `docs/superpowers/specs/2026-07-29-waltex-userland-design.md`

**Interfacce:** nessuna. Task di sola documentazione.

- [ ] **Passo 1: `CLAUDE.md`, la sezione di stato**

Sostituire il blocco di M11a che comincia con **«L'innesto, che e' tutto cio'
che c'e' di un mount»** e le due note che seguono («si innesta
`devfs_devdir()`, NON `devfs_root()`» e «`lookup` e `readdir` devono essere
d'accordo sull'innesto») con:

```markdown
M11c chiusa: il **mount vero**. La tabella sta in `vfs.c`, la sostituzione è una
riga in `vfs_resolve`, e `minixfs_graft` non esiste più.

**Il difetto che ha chiuso, misurato:** fino a M11b, per montare qualcosa
bisognava *modificare il filesystem che possedeva il punto di innesto*. Un
`walterfs` sotto `/mnt` avrebbe voluto una `minixfs_graft` più grande. È al
contrario, e le due note di M11a che dicevano «`lookup` e `readdir` devono
essere d'accordo sull'innesto» non erano trappole: erano il sintomo.

- **il punto di mount ESISTE sul disco.** In Unix `mount` non aggiunge un nome,
  ne **copre** uno — `mount /x` con `/x` inesistente dà `ENOENT`. Da cui
  `mkdir dev` in `tools/mkminix.sh`, e il guadagno grosso: **`minix_readdir` non
  sa più niente dei mount**, perché il nome `dev` glielo dà il disco. Metà del
  problema è sparita invece di spostarsi di un livello;
- **la chiave della tabella è il PUNTATORE, non `ino`.** È la nota di M11a che
  presenta il conto: `dev` e `hello.txt` hanno entrambi il numero 2, perché gli
  inode sono unici *dentro* un filesystem. Una tabella per numero monterebbe due
  cose diverse sullo stesso posto;
- **non si può «scambiare l'inode»**, ed è la prima idea che viene. Quell'inode
  vive nella cache di minixfs e viene riletto dal disco: alla prima
  `inode_carica` che riusa lo slot il mount evapora. Lo scambio non è nei dati,
  è nel risolutore — come l'albero non è nei dati ma nella `lookup`;
- **`risolvi_mount` ha un ciclo esterno CON UN TETTO.** Serve all'impilamento —
  montare sopra un punto già montato — e il tetto perché montare A su B e B su A
  costruisce un ciclo. È la regola di M10 («ogni attesa vuole un tetto»)
  applicata a un ciclo invece che a un'attesa;
- **l'ordine in `kmain` si è ROVESCIATO**, per due ragioni indipendenti:
  `vfs_init` azzera la tabella, e `vfs_mount` risolve un path. Fino a M11b la
  graft veniva prima di `vfs_init`;
- **un mount fallito non fa più ripiegare su devfs.** La radice su disco resta
  buona e `/dev` resta la directory vuota che è sull'immagine. Perdere il
  filesystem intero perché un mount non è andato sarebbe sproporzionato.

Il controllo che dà senso alla milestone non è automatico: si monta l'immagine
sull'host e si guarda che `/dev` sia **vuota**. Montare non scrive niente sul
filesystem montante, e questa è tutta la differenza fra montare e creare.

E il controllo migliore è quello che **non è cambiato**: `tests/shell.sh` cerca
`dev` in `ls /` e i tre dispositivi in `ls /dev` esattamente come prima. Le due
righe passano provando cose diverse — la prima adesso legge il disco, la seconda
attraversa la tabella di mount — ed è la conferma che il taglio è nel punto
giusto.
```

- [ ] **Passo 2: `CLAUDE.md`, la roadmap e lo stato**

Nella riga di stato:

```markdown
Stato: **primo blocco chiuso, M7, M8, M9, M10 e M11 chiuse.** M12 (memoria) è la
prossima.
```

E nella tabella del secondo blocco, dopo la riga di M11b:

```text
M11c mount           tabella di mount nel VFS, minixfs_graft rimossa  CHIUSA
```

- [ ] **Passo 3: `CLAUDE.md`, i numeri dei test**

Riscrivere il paragrafo «Stato dei test» con i numeri **misurati**:

```bash
make -C tests/host -s run | grep -cE "ok +--"
```

e la stessa cosa sul log seriale per i self-check. Attesi 430 e 109 — se non
tornano, vince la misura.

- [ ] **Passo 4: lo spec**

In `docs/superpowers/specs/2026-07-29-waltex-userland-design.md`, la sezione
«Fuori scope, dichiarato» elenca **«più filesystem montati insieme»**. Va
emendata, con la stessa forma con cui in `CLAUDE.md` si è emendata la frase
sbagliata su `vfs.c` — si annota, non si riscrive la storia:

```markdown
*(Emendato in M11c: la tabella di mount c'è, con `MAX_MOUNTS 4`. Restano fuori
`umount` — vuole i refcount di M16 — e `..` che attraversa il confine
all'indietro, che serve solo quando arriva `chdir`.)*
```

- [ ] **Passo 5: commit**

```bash
git add CLAUDE.md docs/superpowers/specs/2026-07-29-waltex-userland-design.md
git commit -m "docs: M11c, il mount vero e cosa ha chiuso"
```

---

## Cosa resta fuori, dichiarato

- **`umount`.** Niente lo chiamerebbe, e smontare per davvero vuole sapere se ci
  sono file aperti sotto il punto di mount — cioè i refcount, che arrivano in
  M16 con `fork` e `dup`.
- **`..` che attraversa il confine all'indietro.** `/dev/..` oggi fallisce,
  perché la directory di devfs non ha una voce `..`. In Unix vero il risolutore
  guarda la tabella al contrario. Serve quando arriva `chdir` — M14 — e non
  prima.
- **Il mountpoint creato automaticamente se manca.** In Unix `mount` dà
  `ENOENT`, ed è giusto: un mountpoint che appare dal nulla nasconde un errore
  di battitura.
- **Un comando `mount` nella shell.** Non c'è un secondo filesystem da montare a
  mano: `hda` è un disco a pattern grezzo. Il giorno che ci fosse, il comando è
  tre righe e la meccanica c'è già — che è il punto.

## Autoverifica di questo piano

**Copertura.** Le quattro cose che la milestone deve produrre — la tabella nel
VFS, la sostituzione nel risolutore, `/dev` vero sul disco, la graft rimossa —
hanno un task ciascuna o stanno nel Task 2 per la ragione di atomicità
dichiarata all'inizio. La documentazione è il Task 3.

**Segnaposto.** Nessun «TBD», nessun «gestire gli errori»: i sette controlli di
`vfs_mount` sono elencati in ordine, i diciassette test host sono scritti per
intero, e i tre self-check pure.

**Coerenza dei nomi.** `vfs_mount`, `risolvi_mount`, `MAX_MOUNTS`,
`mounts[].punto`, `mounts[].root` sono gli stessi in ogni task. `devfs_devdir()`
— **non** `devfs_root()` — è quello che si monta, ed è l'errore che in M11a
quattro self-check hanno preso: la radice di devfs ha una sola voce e si chiama
`dev`, quindi montando quella si ottiene `/dev/dev/kbd`.
