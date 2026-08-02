# M11b — minix v1, scrittura: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** contengono
> interfaccia, test, concetti e tranelli, ma il codice lo scrive Walter
> (vedi `CLAUDE.md`). I task **[CLAUDE]** sono infrastruttura.
>
> **Companion:** `2026-08-02-waltex-m11b-funzioni.md` — una scheda per funzione.

**Obiettivo:** dal prompt si crea un file, e `fsck.minix` sull'host dice che
l'immagine è coerente.

```text
waltex> mkdir /nuovo
waltex> write /nuovo/ciao.txt salve dal kernel
  scritti 17 byte
```

e poi, fuori dalla VM:

```text
$ fsck.minix -f build/minix.img     →  exit 0
$ mount -t minix ... && cat mnt/nuovo/ciao.txt
salve dal kernel
```

## Il riferimento cambia verso, ed è la novità della milestone

In M11a `mkfs.minix` scriveva e il nostro parser leggeva. Qui **scrive il
kernel**, e sono `fsck.minix` e `mount` a dire se ci hanno creduto.

E `mount` da solo non basta come oracolo. Misurato: spegnendo un bit nella
bitmap degli inode dell'immagine di riferimento —

```text
$ fsck.minix -f rotta.img
Inode 2 marked unused, but used for file '/hello.txt'      exit 4

$ mount -t minix rotta.img mnt && ls mnt
enorme.txt  etc  grande.txt  hello.txt  vuoto.txt          ← funziona lo stesso
```

Un filesystem incoerente si monta e si legge benissimo: il danno esce alla
**prossima allocazione**, quando l'inode 2 viene riusato e due file finiscono
sullo stesso. È il genere di guasto che compare tre operazioni dopo la causa, ed
è esattamente ciò contro cui serve un oracolo esterno.

**Quindi `tests/minixwrite.sh` chiama `fsck.minix`, non solo `mount`.**

## Cosa è dentro e cosa è fuori

| dentro | fuori |
|---|---|
| allocare un inode e una zona sulle bitmap | **`unlink` e `rmdir`** |
| creare un file, creare una directory | `truncate`, `rename` |
| scrivere, e far crescere un file | i permessi fatti valere |
| far crescere una directory | il doppio indiretto in scrittura |

**`unlink` è fuori di proposito, e la ragione è diagnostica.** Con la sola
allocazione le bitmap possono solo *crescere*: se `fsck` trova un bit acceso che
non dovrebbe esserlo, il colpevole è l'allocatore, punto. Aggiungendo la
liberazione, ogni disaccordo diventa ambiguo — è l'alloc che accende di troppo o
la free che spegne di meno? Due direzioni insieme in una milestone che tocca
strutture su disco è la ricetta per una giornata persa.

Arriva subito dopo, e a quel punto il ciclo crea-cancella-crea è il test più
cattivo che si possa fare a un allocatore.

Il **doppio indiretto in scrittura** è fuori per aritmetica: con l'immagine da
256 KB non ci si arriva — servirebbero file oltre 519 KB. La lettura lo gestisce
già; la scrittura lo rifiuta con un errore esplicito invece di fingere.

## Le bitmap, verificate e non ricordate

I due numeri che seguono vengono dall'immagine vera, letta con `od`.

```text
blocco 2   bitmap degli inode    ff 00 00 00 00 00 00 00 00 00 00 00 fe ff ff ff
blocco 3   bitmap delle zone     ff ff ff 7f 00 00 00 00 ...
```

### Il bit 0 è riservato, in entrambe

Vale sempre 1 e non corrisponde a niente. È il motivo per cui l'inode 0 non
esiste — e per cui lo zero può significare «nessun inode» in una voce di
directory.

### E i due indici NON si calcolano allo stesso modo

Questa è la trappola numero uno di M11b:

```text
tabella degli inode:   inode i  →  offset (i - 1) * 32
bitmap degli inode:    inode i  →  bit i                  ← NIENTE meno uno
bitmap delle zone:     zona z   →  bit z - s_firstdatazone + 1
```

La verifica, sull'immagine di prova:

- **imap.** Ci sono 7 inode in uso, da 1 a 7. Il primo byte è `ff`, cioè i bit da
  0 a 7: il bit 0 riservato più i bit 1-7. Se l'indice fosse `i - 1`, il byte
  sarebbe `7f`.
- **zmap.** `s_firstdatazone` è 7, e le zone occupate sono la 7 fino alla 36 —
  trenta in tutto. I primi quattro byte sono `ff ff ff 7f`, cioè i bit da 0 a 30:
  il bit 0 riservato più trenta bit. Quindi la zona 7 è il bit **1**, e la zona
  36 è il bit 30.

Il byte `fe` all'offset 12 della imap è l'altra conferma: 96 inode significa che
l'ultimo valido è il bit 96, che cade in quel byte ed è libero (0); i bit da 97 a
103 non esistono e `mkfs` li lascia a 1. Un allocatore che non si ferma a
`s_ninodes` li troverebbe occupati e non farebbe danni **per caso** — che è
peggio di farli.

## La quinta casella di `inode_ops`

Le quattro attuali non servono a creare, e in `CLAUDE.md` avevo scritto che
`vfs.c` era finito fino a M17. Non è vero, e questa è la correzione:

```c
struct inode_ops {
    int (*read   )(struct inode *, uint32_t off, void *, uint32_t);
    int (*write  )(struct inode *, uint32_t off, const void *, uint32_t);
    int (*lookup )(struct inode *, const char *name, struct inode **);
    int (*readdir)(struct inode *, int idx, char *name, uint32_t *ino);
    int (*create )(struct inode *dir, const char *name,
                   enum inode_type tipo, struct inode **out);   /* NUOVA */
};
```

**Una casella e non due**, con il tipo come argomento: su minix un file e una
directory differiscono per due cose sole — i bit di tipo in `i_mode`, e il fatto
che una directory nasce con `.` e `..` già dentro. Due funzioni sarebbero due
copie della stessa allocazione.

E `devfs` la lascia a zero, che significa «non supportata»: la convenzione di M8,
per la terza volta. `mkdir /dev/x` fallisce da sé, senza un caso a parte.

## Vincoli globali

Quelli dei blocchi precedenti, più:

- **Scrittura sincrona, nessuna cache sporca.** Ogni modifica a un inode o a una
  bitmap va sul disco prima che la funzione ritorni. È lento e deliberato: una
  cache con i blocchi sporchi vuole una politica di scaricamento, e quella è la
  milestone della memoria.
- **Ogni allocazione si scrive PRIMA di essere usata.** Se si mette una zona in
  `i_zone[n]` e poi si fallisce nell'accendere il bit, il filesystem ha una zona
  usata e marcata libera — cioè il guasto che `fsck` ha appena mostrato.
- **`i_nlinks` va tenuto.** Un file nasce con 1, una directory con 2 (`.` e la
  voce nel genitore), e creando una directory il genitore passa da N a N+1 per
  via del `..`. È il primo campo che `fsck` controlla, e sbagliarlo è il modo più
  facile di far fallire il test.
- **`i_time` si può lasciare a zero.** Non c'è un orologio da cui prenderlo e
  `fsck` non se ne lamenta — verificato. Scriverlo con `timer_ticks()` sarebbe
  peggio: un timestamp finto è più difficile da diagnosticare di uno assente.
- **L'immagine di lavoro è `build/minix.img`, una copia.** `tests/data/minix.img`
  è il riferimento committato e non si tocca — la regola c'è già dal Makefile di
  M11a, e da qui in poi serve davvero.

## Struttura dei file al termine di M11b

| File | Responsabilità | Chi |
|---|---|---|
| `include/vfs.h` | la quinta casella, `vfs_mkdir`, `O_CREAT` | CLAUDE |
| `kernel/vfs.c` | `O_CREAT` in `vfs_open`, `vfs_mkdir` | **WALTER** |
| `kernel/minixfs.c` | bitmap, allocazione, `create`, `write` | **WALTER** |
| `kernel/shell.c` | `write` e `mkdir` | **WALTER** |
| `tests/host/test_minixfs.c` | i controlli di scrittura, su una copia | CLAUDE |
| `tests/minixwrite.sh` | **il controllo con `fsck`** | CLAUDE |
| `kernel/selftest.c` | la catena dentro la VM | CLAUDE |

## L'interfaccia

```c
/* ---- include/vfs.h, le aggiunte ---- */

#define O_CREAT  0100        /* il valore POSIX, verificato dagli header host */

/* Crea "name" dentro "dir" e ne consegna l'inode.

   0 e *out se ci riesce, -1 altrimenti — e su -1 *out NON viene toccato, la
   convenzione di lookup.

   Il tipo distingue file e directory, e su minix la differenza e' tutta in due
   punti: i bit di tipo in i_mode, e le voci "." e ".." che una directory ha
   dalla nascita.

   NON deve riuscire se "name" esiste gia': chi crea deve poterlo sapere. Sta al
   chiamante decidere cosa farne — vfs_open con O_CREAT apre e basta, vfs_mkdir
   fallisce. */
int (*create)(struct inode *dir, const char *name,
              enum inode_type tipo, struct inode **out);

/* Il descrittore, come prima. Con O_CREAT, se il path non esiste si crea l'ultimo
   componente — e SOLO l'ultimo: "mkdir -p" non esiste, e un componente
   intermedio mancante resta un errore.

   Senza O_CREAT il comportamento non cambia di una virgola: e' il controllo che
   dice che M11b non ha rotto M11a. */
int vfs_open(const char *path, int flags);

/* La syscall 39 di Linux i386. Fallisce se il path esiste gia', se il genitore
   non esiste, o se il filesystem non ha create. */
int vfs_mkdir(const char *path);
```

---

## I task

### Task 1 [CLAUDE]: l'interfaccia e i test che non passano

`include/vfs.h` con la quinta casella e `vfs_mkdir`, i controlli di scrittura in
`test_minixfs.c`, e `tests/minixwrite.sh`.

I test host lavorano su una **copia** dell'immagine, rifatta a ogni esecuzione:
un test che scrive nel proprio input non è ripetibile, ed è la lezione di
`disk.sh`. La copia la fa il test stesso all'avvio, con `fopen` in scrittura su
un file temporaneo.

- [ ] **Passo 1:** `include/vfs.h` — `create`, `O_CREAT`, `vfs_mkdir`.
- [ ] **Passo 2:** `test_minixfs.c` — i controlli nuovi, su copia.
- [ ] **Passo 3:** `tests/minixwrite.sh` — la VM crea, l'host verifica con
      `fsck.minix` **e** con `mount` + `cat`.
- [ ] **Passo 4: verifica.** Il kernel non compila più — `inode_ops` ha una
      casella in più e le tabelle in `devfs.c` e `minixfs.c` non la nominano.
      È voluto: dice esattamente dove intervenire.

---

### Task 2 [WALTER]: le bitmap

Le due funzioni più piccole della milestone e le più facili da sbagliare. Schede
1 e 2 del companion.

```text
bitmap_trova_libero(primo_blocco, quanti_bit)  →  il primo bit a zero, o 0
bitmap_accendi(primo_blocco, bit)              →  legge, modifica, riscrive
```

Le tre cose da vedere prima:

**Il bit 0 è riservato in entrambe le bitmap**, quindi la ricerca comincia da 1 e
lo zero può fare da «non trovato».

**Il limite è `s_ninodes` per la imap e `s_nzones - s_firstdatazone + 1` per la
zmap.** Senza, si trovano liberi i bit che `mkfs` ha lasciato a 1 fuori dal
filesystem — e siccome sono a 1, non si trovano affatto: l'allocatore sembra
funzionare finché il disco non è pieno.

**Una bitmap può occupare più di un blocco.** Sull'immagine di prova ne occupa
uno solo, quindi un codice che assume un blocco passa tutti i test. Vale la pena
scriverlo giusto adesso: il ciclo è sui blocchi, e dentro sui byte.

- [ ] **Passo 1:** `bitmap_trova_libero`. Scheda 1.
- [ ] **Passo 2:** `bitmap_accendi`. Scheda 2.
- [ ] **Passo 3: verifica a mano.** Con `rdsect hdb 4` guardi la bitmap degli
      inode prima e dopo — il blocco 2 sono i settori 4 e 5.

---

### Task 3 [WALTER]: allocare e scrivere

Schede 3-7. È il grosso della milestone.

```text
inode_alloca(tipo)          bit nella imap, i 32 byte, e li scrive
inode_scrivi(ino)           rimanda sul disco un inode della cache
zona_alloca()               bit nella zmap
zona_assegna(ino, n, z)     mette z in i_zone[n], o nel blocco indiretto
minix_write(ino, off, ...)  scrive byte, allocando le zone che mancano
```

**L'ordine dentro `inode_alloca` non è negoziabile:** prima si accende il bit,
poi si scrive l'inode. Al contrario, un fallimento in mezzo lascia un inode
scritto e marcato libero, che la prossima allocazione riuserà.

**`zona_assegna` è il posto dove il blocco indiretto va allocato**, la prima
volta che si supera la settima zona. E il blocco appena allocato **va azzerato**
prima di usarlo: contiene quello che c'era prima, che verrebbe letto come una
tabella di puntatori.

**`minix_write` è il gemello di `minix_read`, con una differenza sola** — dove
`read` trova una zona a zero e restituisce zeri, `write` la alloca. E alla fine
aggiorna `i_size` se il file è cresciuto, e lo riscrive.

- [ ] **Passo 1:** `inode_scrivi` — serve a tutte le altre. Scheda 3.
- [ ] **Passo 2:** `inode_alloca`. Scheda 4.
- [ ] **Passo 3:** `zona_alloca`. Scheda 5.
- [ ] **Passo 4:** `zona_assegna`, con l'indiretto. Scheda 6.
- [ ] **Passo 5:** `minix_write`. Scheda 7.
- [ ] **Passo 6: verifica.** I test host di scrittura sui file **esistenti**
      passano. La creazione è il task dopo.

---

### Task 4 [WALTER]: creare

Schede 8 e 9: `dirent_inserisci` e `minix_create`.

**`dirent_inserisci` cerca una voce con `ino == 0`, e se non ce n'è fa crescere
la directory** — che è un file come gli altri, quindi la crescita è
`minix_write` più `i_size += 16`. Sull'immagine di prova la radice ha 112 byte in
una zona da 1024, quindi c'è posto per 64 voci prima che la crescita serva
davvero: **il caso interessante non si presenta finché non crei 57 file.** Il
test lo provoca apposta.

**Una directory nasce con `.` e `..`.** Il primo punta a se stessa, il secondo al
genitore, e il genitore guadagna un link. Tre numeri da tenere allineati, e sono
i tre che `fsck` controlla per primi.

- [ ] **Passo 1:** `dirent_inserisci`. Scheda 8.
- [ ] **Passo 2:** `minix_create`, e la tabella `ops_minix` che guadagna la
      quinta voce. Scheda 9.
- [ ] **Passo 3: verifica.** I test host di creazione passano.

---

### Task 5 [WALTER]: il lato VFS

`O_CREAT` in `vfs_open` e `vfs_mkdir`. Schede 10 e 11.

**`O_CREAT` entra dove `vfs_resolve` fallisce**, e la parte scomoda è che serve
il *genitore* del path, che `vfs_resolve` non restituisce. Il modo più semplice è
risolvere il path senza l'ultimo componente — e la scelta è fra copiare il path e
troncarlo, oppure aggiungere a `vfs_resolve` un parametro. La prima è più corta e
non tocca una funzione che funziona.

- [ ] **Passo 1:** la quinta voce nella tabella di `devfs.c`, a **zero**.
- [ ] **Passo 2:** `O_CREAT` in `vfs_open`. Scheda 10.
- [ ] **Passo 3:** `vfs_mkdir`. Scheda 11.
- [ ] **Passo 4: verifica.** I 75 test di `test_vfs.c` passano ancora invariati —
      è il controllo che dice che M11a non si è rotta.

---

### Task 6 [WALTER]: `write` e `mkdir` nella shell

Schede 12 e 13. Due comandi corti, e sono il modo in cui `tests/minixwrite.sh`
provoca la condizione che misura — come `spin` in `tests/tasks.sh` e `wrsect` in
M10.

```text
waltex> mkdir /nuovo
waltex> write /nuovo/ciao.txt salve dal kernel
  scritti 17 byte
```

`write` rimette gli spazi fra gli argomenti, come `echo`, e aggiunge un `\n`
finale — così `cat` sull'host mostra qualcosa di leggibile.

- [ ] **Passo 1:** `mkdir`. Scheda 12.
- [ ] **Passo 2:** `write`. Scheda 13.

---

### Task 7 [CLAUDE]: la catena e la chiusura

I self-check dentro la VM, `tests/minixwrite.sh` in coda a `make test`,
`README.md`, `CLAUDE.md`, e il commit proposto
`M11b: minix v1 in scrittura, bitmap e creazione`.

---

## Dove ci si farà male

In ordine di quanto costa diagnosticarlo.

1. **L'indice della bitmap con il `- 1` di troppo.** La tabella degli inode usa
   `i - 1`, le bitmap no. Sbagliando, si alloca l'inode 3 e si accende il bit 2:
   `fsck` lo dice, ma solo dopo che hai creato qualcosa.
2. **La zmap con l'offset sbagliato.** `zona z → bit z - firstdatazone + 1`, e i
   modi di sbagliarlo sono tre: dimenticare il `+1`, dimenticare
   `firstdatazone`, o farli entrambi e ottenere per caso il risultato giusto su
   un'immagine sola.
3. **Il bit acceso dopo l'uso invece che prima.** Un fallimento in mezzo lascia
   un filesystem che si monta e si legge, e si rompe alla prossima allocazione.
4. **`i_nlinks` sbagliato su `mkdir`.** Sono tre numeri: la directory nuova a 2,
   il genitore a +1, e la voce `..`. `fsck` li controlla tutti e tre.
5. **Il blocco indiretto allocato e non azzerato.** Contiene i dati di prima,
   letti come puntatori: il file diventa illeggibile dalla ottava zona in poi,
   cioè solo quando supera i 7 KB.
6. **`i_size` non riscritta.** Il file c'è, il contenuto pure, e vale zero byte.
   `cat` non stampa niente e la diagnosi va a finire su `read`.

## Verifica di M11b, in una riga

**Il kernel crea, e `fsck.minix` dice `exit 0`** — poi `mount` sull'host mostra
il file con il contenuto giusto. E `make test` verde due volte di fila, con
l'immagine rifatta a ogni giro.
