# M11a — minix v1, lettura: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** contengono
> interfaccia, test, concetti e tranelli, ma il codice lo scrive Walter
> (vedi `CLAUDE.md`). I task **[CLAUDE]** sono infrastruttura.
>
> **Companion:** `2026-08-02-waltex-m11a-funzioni.md` — una scheda per funzione.

**Obiettivo:** `cat /etc/motd` legge un file vero da un filesystem minix vero, e
lo stesso `cat` di M9b non cambia di una riga.

## Due decisioni da confermare prima di cominciare

### 1. M11 si divide in due, come M6 e M9

Lo spec dice già «lettura, poi scrittura». La lettura da sola è superblocco,
bitmap da saltare, tabella degli inode, mappatura delle zone con indiretto e
doppio indiretto, directory: circa 350 righe. La scrittura aggiunge
l'allocazione sulle bitmap, la creazione di inode, l'inserimento nelle
directory e la crescita dei file — un'altra milestone intera, che tocca anche i
percorsi di scrittura del VFS.

**M11a è di sola lettura**, e il filesystem si monta come tale. Non è una
limitazione temporanea da nascondere: è la stessa disciplina di M9b, dove devfs
è di sola lettura *per struttura*.

### 2. Dove si aggancia minix — la decisione che consiglio

Non c'è una tabella di mount, ed è fuori scope nello spec. Ma minix deve pure
comparire da qualche parte, e ci sono due modi:

| | l'albero | costo |
|---|---|---|
| **A. minix è la radice** *(consigliato)* | `/` da disco, `/dev` innestata | minix deve funzionare perché il sistema abbia un filesystem |
| B. devfs resta la radice | `/dev` e `/mnt`, con minix sotto `/mnt` | nessun rischio, ma i path cambieranno in M15 |

**Consiglio A**, perché è la forma a cui il blocco punta: in M16 `init` caricherà
`/bin/sh`, e quel path deve essere assoluto e su disco. Ogni giorno passato con
`/mnt/bin/sh` è un giorno di path che poi cambiano.

E si implementa **senza toccare `vfs.c`** e senza accoppiare minix a devfs:

```c
minixfs_graft("dev", devfs_root());   /* uno slot, non una tabella */
```

`minixfs_graft` riceve un `struct inode *` e non sa da dove viene — è lo stesso
espediente di `vfs_init(root)` e del sink di eco di `lineedit`. La `lookup` della
radice minix controlla prima l'innesto, poi il disco.

Con la protezione ovvia: se `minixfs_init` fallisce — nessun disco, magic
sbagliato — `kmain` ripiega su `vfs_init(devfs_root())` e lo dice. Il kernel
resta usabile, `/dev` c'è, e il motivo è sulla seriale invece di essere una
radice muta.

## Il formato, verificato e non ricordato

Tutti i numeri qui sotto vengono da un'immagine vera fatta con
`mkfs.minix -1 -n 14` (util-linux 2.39.3), montata, popolata e riletta con `od`.

```text
blocco 0        boot block, 1024 byte, minix non lo usa
blocco 1        SUPERBLOCCO
blocchi 2..     bitmap degli inode      s_imap_blocks blocchi
poi             bitmap delle zone       s_zmap_blocks blocchi
poi             TABELLA DEGLI INODE     s_ninodes * 32 byte, arrotondati a blocco
poi             le zone dei dati, e la prima e' s_firstdatazone
```

**Il blocco è 1024 byte, il settore è 512: due settori per blocco.** È l'unica
aritmetica che `struct blockdev` non fa per te.

### Il superblocco, letto da un'immagine reale

```text
offset  campo             tipo      valore misurato (immagine da 2 MiB)
  0     s_ninodes         uint16    704
  2     s_nzones          uint16    2048
  4     s_imap_blocks     uint16    1
  6     s_zmap_blocks     uint16    1
  8     s_firstdatazone   uint16    26
 10     s_log_zone_size   uint16    0        (zona = blocco)
 12     s_max_size        uint32    268966912
 16     s_magic           uint16    0x137F   <- 14 caratteri per nome
 18     s_state           uint16    1
```

`0x137F` è minix v1 con nomi da **14**; `0x138F` è la variante da 30, e va
**rifiutata** — con nomi da 30 la voce di directory è lunga 32 byte invece di 16,
quindi leggere una directory darebbe spazzatura senza nessun errore.

Verifica di coerenza che vale la pena fare a mano una volta:
`2 + 1 + 1 = 4` è il primo blocco della tabella degli inode;
`704 * 32 = 22528` byte sono 22 blocchi; `4 + 22 = 26 = s_firstdatazone`. ✓

### L'inode, 32 byte

```text
offset  campo       tipo         nota
  0     i_mode      uint16       0o40755 per una directory, 0o100644 per un file
  2     i_uid       uint16
  4     i_size      uint32       i BYTE, non le zone
  8     i_time      uint32
 12     i_gid       uint8
 13     i_nlinks    uint8
 14     i_zone[9]   uint16 x 9   7 dirette, 1 indiretta, 1 doppia indiretta
```

**La radice è l'inode 1, non 0.** Lo zero significa «nessun inode», ed è il
valore con cui una voce di directory dice «cancellata». Da cui l'unica
sottrazione da non sbagliare:

```text
inode i sta a:  (primo_blocco_inode * 1024) + (i - 1) * 32
```

Misurato: inode 1 ha `i_zone[0] = 26`, cioè esattamente `s_firstdatazone`.

### Le voci di directory, 16 byte

```text
offset 0   uint16   numero di inode, 0 = voce libera
offset 2   char[14] nome, riempito di zeri, NON terminato se lungo 14
```

Una directory è **un file normale** il cui contenuto sono queste voci, e la sua
`i_size` è un multiplo di 16. Il dump reale della radice:

```text
01 00 2e 00 00 ...                      ino 1  "."
01 00 2e 2e 00 ...                      ino 1  ".."
02 00 68 65 6c 6c 6f 2e 74 78 74 00 ..  ino 2  "hello.txt"
03 00 64 69 72 00 ...                   ino 3  "dir"
05 00 67 72 61 6e 64 65 2e 74 78 74 ..  ino 5  "grande.txt"
```

`i_size` era 80 = cinque voci per sedici byte.

### La mappatura delle zone, che è il cuore di M11a

Data la zona logica `n` di un file — cioè il byte `n * 1024` — quale zona fisica
la contiene:

```text
n < 7                 →  i_zone[n]                             diretta
n < 7 + 512           →  indiretto[n - 7]        in i_zone[7]  UNA lettura in piu'
n < 7 + 512 + 512*512 →  doppio indiretto        in i_zone[8]  DUE letture in piu'
```

512 perché un blocco da 1024 byte contiene 512 puntatori da `uint16`.

Verificato su un file da 20000 byte: `i_zone[0..6] = 35..41` — le prime sette
zone, 7168 byte — e `i_zone[7] = 42`, che è il blocco indiretto e contiene
`43 44 45 46 47 48 49 50 51 52 53 54 55 0 0 …`. Le tredici zone che mancano.

**Una zona a zero significa buco**, non fine del file: un file sparso ha zone
non allocate in mezzo, e vanno lette come zeri. Lo dice `i_size` quando il file
finisce, non la prima zona nulla.

## Vincoli globali

Quelli dei blocchi precedenti, più:

- **Sola lettura.** Nessuna `write`, nessuna `create`, nessuna `mkdir`. La
  bitmap si legge per capire il layout e non si tocca.
- **Nessuna cache dei blocchi.** Ogni accesso va al disco, e camminare
  `/a/b/c` costa una lettura per componente più una per inode. È lento e
  deliberato: la cache vuole un allocatore, che arriva in M12. Ma **arriva
  l'emendamento di M9a**: la cache degli inode qui diventa necessaria per un
  motivo diverso — vedi sotto.
- **Il numero di inode è statico**, `MAX_INODES 64` come da spec.
- **Solo `-1 -n 14`.** Il magic `0x138F` si rifiuta.
- **Il blocco è 1024 e il settore 512.** Nessuna costante 512 dentro
  `minixfs.c` che non sia `SECTOR_SIZE` moltiplicato per due.

### L'emendamento di M9a che scade qui

In M9a avevo scritto: «la cache di inode non esiste, serve in M11, quando gli
inode vengono dal disco». Ci siamo, e la ragione è concreta: **`lookup`
restituisce un `struct inode *`, e quel puntatore deve puntare a qualcosa che
sopravvive alla chiamata.**

In devfs era facile — gli inode erano statici e c'erano tutti. Qui vengono dal
disco, e non si può restituire l'indirizzo di una variabile locale.

Serve quindi un array `static struct inode inodi[MAX_INODES]` dentro
`minixfs.c`, con una ricerca per numero: se l'inode 42 è già in memoria si
restituisce quello, altrimenti si occupa uno slot libero e lo si legge. **Senza
refcount** — non si libera niente in M11a, e i refcount arrivano in M16 con
`fork` e `dup`. Uno slot esaurito è `-ENOSPC`, deterministico e visibile.

Che due `lookup` dello stesso path debbano dare lo **stesso** puntatore non è
un'ottimizzazione: è la correttezza. Con due copie, la seconda avrebbe una
`i_size` che può divergere dalla prima.

## Due dischi, e finalmente `priv` serve

L'immagine di M10 e quella di minix **non possono essere lo stesso disco**: il
settore 2 su cui `disk.sh` scrive è la prima metà del superblocco minix.

```text
hda   build/disk.img    l'immagine a pattern di M10, invariata
hdb   build/minix.img   il filesystem
```

`ata_init` prova già master e slave, quindi non cambia una riga. Ma tre cose
succedono la prima volta:

- il self-check `ata_drive(1) non esiste` **diventa il suo contrario**;
- `blk` elenca due righe;
- e **`priv` viene esercitato davvero**: due `struct blockdev` con lo stesso
  puntatore a `read`, e solo `priv` a distinguerle. Finora era una promessa.

## L'immagine di riferimento, e perché si committa

`tools/mkminix.sh` la costruisce con `mkfs.minix`, la monta, ci mette dei file e
la smonta. **Vuole `sudo`**, perché montare è privilegiato.

Da cui il problema: `make test` non può volere `sudo`. La soluzione è committare
l'immagine costruita in `tests/data/minix.img`, 256 KB quasi tutti zeri, e usare
lo script solo per rigenerarla.

Non indebolisce niente. Il riferimento resta **un'implementazione che non è la
nostra** — `mkfs.minix` di util-linux e il modulo `minix` del kernel Linux — e
committarla la rende anche *stabile*: il test confronta sempre contro gli stessi
byte, invece che contro l'umore della versione di util-linux installata.

Il contenuto, scelto perché ogni file prova una cosa diversa:

```text
/hello.txt        26 byte      una zona sola, il caso base
/etc/             directory    una lookup a due livelli
/etc/motd         piccolo      il file che cat leggera' dentro la VM
/grande.txt       5000 byte    cinque zone DIRETTE
/enorme.txt       20000 byte   sfonda le 7 dirette: esercita l'INDIRETTO
/vuoto.txt        0 byte       size 0 e zone[0] == 0
```

`/enorme.txt` è quello che conta: 20000 byte sono 20 zone, sette dirette e
tredici attraverso il blocco indiretto. Senza un file così, la mappatura si
prova solo nel ramo facile.

## Struttura dei file al termine di M11a

| File | Responsabilità | Chi |
|---|---|---|
| `include/minixfs.h` | `minixfs_init`, `minixfs_root`, `minixfs_graft` | CLAUDE |
| `kernel/minixfs.c` | superblocco, inode, zone, directory | **WALTER** |
| `kernel/main.c` | il secondo disco, il montaggio, il ripiego | CLAUDE |
| `kernel/selftest.c` | i controlli sull'immagine vera | CLAUDE |
| `tools/mkminix.sh` | costruisce l'immagine di riferimento (vuole sudo) | CLAUDE |
| `tests/data/minix.img` | l'immagine, committata | CLAUDE |
| `tests/host/test_minixfs.c` | **il grosso della verifica** | CLAUDE |
| `tests/shell.sh` | `cat /etc/motd` dentro la VM | CLAUDE |
| `Makefile` | il secondo `-drive` | CLAUDE |

## L'interfaccia

```c
/* ---- include/minixfs.h ---- */

/* Monta il filesystem che sta su dev. Legge e valida il superblocco, e da quel
   momento minixfs_root() risponde.

   Ritorna 0 se ci riesce, -1 se dev e' nullo, se il superblocco non si legge, o
   se il magic non e' 0x137F. Il magic della variante a 30 caratteri, 0x138F, si
   RIFIUTA: con nomi da 30 la voce di directory e' lunga 32 byte invece di 16, e
   leggerla come se fosse da 16 non produce nessun errore, produce nomi finti. */
int minixfs_init(struct blockdev *dev);

/* L'inode della radice — l'inode 1 — da passare a vfs_init. 0 se il mount non
   e' riuscito, come devfs_root(). */
struct inode *minixfs_root(void);

/* Innesta un altro filesystem sotto un nome della radice: UNO slot, non una
   tabella di mount, che e' fuori scope nello spec.

   Riceve un struct inode * e non sa da dove viene — kmain gli passa
   devfs_root(), i test un albero finto. E' lo stesso espediente di
   vfs_init(root), ed e' cio' che permette a minixfs.c di non includere devfs.h.

   Va chiamata DOPO minixfs_init. Ritorna 0, oppure -1 se lo slot e' gia' preso
   o se il mount non e' riuscito. */
int minixfs_graft(const char *nome, struct inode *root);
```

---

## I task

### Task 1 [CLAUDE]: l'immagine e i test host

`tools/mkminix.sh`, `tests/data/minix.img`, `include/minixfs.h`, e
`tests/host/test_minixfs.c` con il suo `struct blockdev` su file.

**È il task che rende M11a la milestone più provata del progetto.** Il
`struct blockdev` che M10 ha consegnato ha due puntatori a funzione: sull'host
diventano `fread` e `fseek` su `tests/data/minix.img`. Niente QEMU, niente
disco, millisecondi.

```c
/* dentro test_minixfs.c */
static int file_read(struct blockdev *b, uint32_t lba, void *buf, uint32_t n)
{
    FILE *f = (FILE *)b->priv;          /* <- ecco a cosa serviva priv */
    if (fseek(f, (long)lba * SECTOR_SIZE, SEEK_SET) != 0) return -1;
    return fread(buf, SECTOR_SIZE, n, f) == n ? (int)n : -1;
}
```

Quattro righe, e `minixfs.c` non si accorge della differenza. È la stessa idea
del sink di `kprintf` e dell'albero finto di `test_vfs.c`, e questa volta paga
più di sempre.

- [ ] **Passo 1: `tools/mkminix.sh`** — `dd`, `mkfs.minix -1 -n 14`, `mount`,
      i sei file, `umount`. Deve **fallire con un messaggio chiaro** se `sudo`
      non c'è, invece di produrre un'immagine vuota.
- [ ] **Passo 2: generare e committare `tests/data/minix.img`.**
- [ ] **Passo 3: verifica dell'immagine con `od`**, prima di scriverci un test
      sopra: magic `137f` all'offset 1040, e la radice all'offset 4096.
- [ ] **Passo 4: `include/minixfs.h`.**
- [ ] **Passo 5: `tests/host/test_minixfs.c`**, che non compila ancora.
- [ ] **Passo 6: verifica.** `make test` verde e invariato — il file di test
      non entra ancora nella lista dei binari.

---

### Task 2 [WALTER]: `kernel/minixfs.c`

Le schede sono nel companion. Le tre cose da vedere prima.

**Il superblocco si legge una volta e si tiene**, in una `static`, insieme ai
tre numeri derivati che servono a ogni accesso: il primo blocco della tabella
degli inode, il primo blocco dei dati, e quanti inode ci sono. Ricalcolarli a
ogni `lookup` è la seconda verità che può divergere dalla prima.

**Un blocco sono due settori**, e la conversione sta in **una** funzione. Se il
`* 2` compare in tre posti, uno dei tre prima o poi sarà diverso.

**`lookup` deve restituire un puntatore stabile**, quindi la cache degli inode
non è opzionale — vedi sopra. Il primo bug di M11a sarà restituire l'indirizzo
di una `struct inode` locale, e il sintomo comparirà due `lookup` dopo.

- [ ] **Passo 1: `blocco_leggi`** — un blocco da 1024, due settori. Scheda 1.
- [ ] **Passo 2: `minixfs_init`** — superblocco, magic, i derivati. Scheda 2.
- [ ] **Passo 3: `inode_carica`** — dal disco a `struct inode`, con la cache.
      Scheda 3.
- [ ] **Passo 4: `zona_di`** — la mappatura, e l'indiretto. Scheda 4.
- [ ] **Passo 5: `minix_read`** — byte, non zone. Scheda 5.
- [ ] **Passo 6: `minix_lookup` e `minix_readdir`.** Schede 6 e 7.
- [ ] **Passo 7: `minixfs_root` e `minixfs_graft`.** Schede 8 e 9.
- [ ] **Passo 8:** i test host passano. È qui che si chiude il Task 2, non alla
      compilazione.

---

### Task 3 [CLAUDE]: il secondo disco e il montaggio

`Makefile`: il secondo `-drive`. `kmain`: `minixfs_init(ata_drive(1))`,
`minixfs_graft("dev", devfs_root())`, `vfs_init(minixfs_root())`, e il ripiego
su devfs se il mount fallisce.

Più il self-check di M10 che si capovolge: `ata_drive(1)` adesso **esiste**.

- [ ] **Passo 1:** il secondo `-drive` nel Makefile e nei cinque script.
- [ ] **Passo 2:** il montaggio in `kmain`, con il ripiego e il marker.
- [ ] **Passo 3:** `check_ata_presente` aggiornato: due dischi.
- [ ] **Passo 4:** i self-check di M11a sull'immagine vera.
- [ ] **Passo 5:** `tests/shell.sh` — `ls /`, `ls /etc`, `cat /etc/motd`,
      e `ls /dev` che deve continuare a funzionare **attraverso l'innesto**.
- [ ] **Passo 6: verifica.** `make test` verde.

---

### Task 4 [CLAUDE]: chiusura

- [ ] `README.md`, `CLAUDE.md`, commit proposto
      `M11a: minix v1 in lettura, superblocco, inode, zone`.

---

## Dove ci si farà male

1. **`(i - 1) * 32` scritto `i * 32`.** Tutti gli inode slittano di uno. La
   radice sembra funzionare — l'inode 0 letto come 1 dà zeri, e una directory
   vuota non è un errore — e il guasto compare al primo file.
2. **Il puntatore restituito da `lookup` che non sopravvive.** Vedi la cache.
3. **`i_size` ignorata nell'ultima zona.** Si leggono 1024 byte quando il file
   ne ha 26, e `cat` stampa 998 byte di spazzatura che *erano già sul disco* —
   quindi hanno l'aria di essere dati.
4. **L'indiretto letto come `uint32`.** In minix **v1** i puntatori di zona sono
   a 16 bit; è la v2 ad averli a 32. Con `uint32` si leggono 256 puntatori a
   caso e il file grande è corrotto dalla zona 8 in poi — cioè il file piccolo
   funziona e il grande no.
5. **La voce di directory con nome lungo esattamente 14.** Non è terminata:
   copiarla con una `strlen` cammina nella voce successiva.
6. **Il magic `0x138F` accettato.** Voci da 32 byte lette come da 16: nomi
   plausibili, tutti sbagliati, e nessun errore.

## Verifica di M11a, in una riga

**`minixfs.c` compilato sull'host cammina l'immagine di `mkfs.minix` e trova
esattamente quello che `mount` e `ls` mostrano** — e poi la stessa immagine,
dentro QEMU, dà gli stessi risultati attraverso `cat`.
