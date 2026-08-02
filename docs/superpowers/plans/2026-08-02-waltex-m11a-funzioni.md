# M11a — cosa deve fare ogni funzione

Companion di `2026-08-02-waltex-m11a-minix-lettura.md`. Una scheda per funzione,
sei voci fisse: **compito**, **ritorna**, **non deve**, **chi la chiama**, **come
si sbaglia**, **test**.

I numeri e i dump di questo documento vengono da un'immagine vera fatta con
`mkfs.minix -1 -n 14`, montata e riletta con `od`. Non sono ricordati.

## Indice

1. [`blocco_leggi`](#1-blocco_leggi)
2. [`minixfs_init`](#2-minixfs_init)
3. [`inode_carica`](#3-inode_carica)
4. [`zona_di`](#4-zona_di)
5. [`minix_read`](#5-minix_read)
6. [`minix_lookup`](#6-minix_lookup)
7. [`minix_readdir`](#7-minix_readdir)
8. [`minixfs_root`](#8-minixfs_root)
9. [`minixfs_graft`](#9-minixfs_graft)

---

## Lo stato di minixfs

Cinque `static`, e nient'altro.

```c
static struct blockdev *disco;         /* quello che minixfs_init ha ricevuto  */
static struct minix_sb  sb;            /* il superblocco, letto UNA volta      */
static uint32_t blocco_inodi;          /* derivato: 2 + imap + zmap            */
static struct inode inodi[MAX_INODES]; /* la cache: lookup deve restituire un
                                          puntatore che sopravvive             */
static struct { char nome[VFS_NAME_MAX + 1]; struct inode *root; } innesto;
```

**Il superblocco si legge una volta.** Rileggerlo a ogni accesso sarebbe una
lettura di disco in più per ogni `lookup`, e soprattutto due copie della stessa
verità.

**`blocco_inodi` è derivato e memorizzato**, non ricalcolato: è
`2 + s_imap_blocks + s_zmap_blocks`, e compare in ogni accesso a un inode.

**La cache non è un'ottimizzazione.** `lookup` restituisce un `struct inode *`,
e quel puntatore deve puntare a qualcosa che sopravvive alla chiamata: non si può
restituire l'indirizzo di una locale. In devfs il problema non c'era perché gli
inode erano tutti statici e tutti presenti; qui vengono dal disco.

E c'è un secondo motivo, che è correttezza e non memoria: **due `lookup` dello
stesso path devono dare lo stesso puntatore.** Con due copie, la `i_size` di una
può divergere da quella dell'altra.

Nessun refcount: in M11a non si libera niente. Arrivano in M16 con `fork` e
`dup`.

---

## 1. `blocco_leggi`

```c
static int blocco_leggi(uint32_t blocco, void *buf);
```

**Compito.** Portare in `buf` i 1024 byte del blocco. È l'unico posto del file
che sa che un blocco sono **due settori**.

```text
disco->read(disco, blocco * 2, buf, 2)
```

**Ritorna.** `0` se ci riesce, `-1` altrimenti. Non il numero di settori: chi
chiama non ha niente da farci, e restituire `2` inviterebbe a confrontarlo con
1024.

**Non deve** esistere in tre copie. Se il `* 2` compare sparso, prima o poi uno
dei tre sarà diverso — ed è il genere di errore che dà un file leggibile a metà.

**Non deve** avere una cache. Ogni chiamata va al disco: è lento e deliberato,
perché una cache vuole un allocatore e quello arriva in M12.

**Chi la chiama.** Tutte le altre. `minixfs_init` per il superblocco,
`inode_carica` per la tabella, `zona_di` per l'indiretto, `minix_read` per i
dati.

**Come si sbaglia.** Passando `blocco` dove va `lba`: `disco->read(disco,
blocco, buf, 2)` legge i settori 26 e 27 invece dei settori 52 e 53. Il
superblocco lo prenderesti comunque — blocco 1, settori 2 e 3, e senza il `* 2`
leggeresti i settori 1 e 2, cioè la seconda metà del boot block e la prima del
superblocco: **il magic finirebbe fuori posto e il mount fallirebbe subito**, che
è la fortuna di questo bug.

**Test.** Nessuno diretto: si esercita attraverso tutte le altre. Il primo
controllo che lo prende è quello sul magic.

---

## 2. `minixfs_init`

```c
int minixfs_init(struct blockdev *dev);
```

**Compito.** Leggere il blocco 1, validarlo, e derivare i tre numeri che
serviranno sempre.

```text
se dev == 0                    →  -1
disco = dev
blocco_leggi(1, buf)           →  se fallisce, -1
copia i campi in sb
se sb.magic != 0x137F          →  -1
blocco_inodi = 2 + sb.imap_blocks + sb.zmap_blocks
azzera la cache degli inode
azzera l'innesto
```

**Il magic si controlla e basta un valore.** `0x137F` è minix v1 con nomi da 14;
`0x138F` è la variante da 30 e va **rifiutata**. Con nomi da 30 la voce di
directory è lunga 32 byte invece di 16: leggerla come da 16 non produce nessun
errore, produce nomi finti e numeri di inode presi dal mezzo di un nome.

**Il superblocco NON si legge con una `struct` sovrapposta al buffer.** Serve
`__attribute__((packed))` — e anche con quello, copiare campo per campo è più
chiaro e non dipende dall'endianness della struct. Minix è little-endian come
i386, quindi qui funzionerebbe; ma la copia esplicita dice cosa sta succedendo.

**Ritorna.** `0` o `-1`. E su `-1` **`minixfs_root()` deve continuare a
restituire `0`**: un mount fallito a metà, con `disco` impostato e `sb` a zeri,
è peggio di uno fallito del tutto.

**Non deve** toccare le bitmap. Si leggono per sapere quanti blocchi occupano —
cioè `s_imap_blocks` e `s_zmap_blocks` dal superblocco — e in M11a non si guarda
cosa contengono. L'allocazione è M11b.

**Non deve** assumere `s_log_zone_size == 0`. Vale zero su ogni immagine che
`mkfs.minix` produce oggi, e una zona è un blocco; ma se fosse diverso, ogni
numero di zona andrebbe spostato. **Rifiutalo se non è zero**, invece di
ignorarlo: un valore inatteso che produce dati sbagliati è peggio di un mount
che non riesce.

**Chi la chiama.** `kmain`, con `ata_drive(1)`. Nei test host, con un
`struct blockdev` i cui `read`/`write` sono `fread`/`fseek` su un file.

**Come si sbaglia.**

- **accettando `0x138F`** (sopra);
- **derivando `blocco_inodi` come `2 + imap`**, dimenticando la bitmap delle
  zone. Sull'immagine di prova entrambe valgono 1, quindi il primo blocco della
  tabella verrebbe 3 invece di 4: leggeresti la bitmap delle zone come se
  fossero inode. La radice avrebbe un `i_mode` assurdo — e questo si vede
  subito, il che è di nuovo fortuna;
- **non azzerando la cache.** In `.bss` parte a zeri, quindi funziona al primo
  mount e si rompe al secondo. Lo stesso tranello di `device_init` in M8, e il
  self-check esiste per quello.

**Test.** Sull'host: un mount che riesce, uno con `dev == 0`, uno su
un'immagine con il magic sporcato a mano. Il terzo è quello che conta.

---

## 3. `inode_carica`

```c
static struct inode *inode_carica(uint32_t ino);
```

**Compito.** Dato un numero di inode, restituire un `struct inode` del VFS che
lo rappresenta — dalla cache se c'è già, dal disco se no.

```text
se ino == 0 o ino > sb.ninodes      →  0
se e' gia' in cache                 →  quello STESSO puntatore
altrimenti prendi uno slot libero   →  se non ce n'e', 0
leggi il blocco che lo contiene
copia i campi, traduci i_mode nel type del VFS
```

L'aritmetica, ed è l'unica sottrazione da non sbagliare:

```text
blocco  = blocco_inodi + (ino - 1) / 16        16 inode da 32 byte per blocco
offset  = ((ino - 1) % 16) * 32
```

**L'inode 1 è la radice, non lo 0.** Lo zero significa «nessun inode» ed è il
valore con cui una voce di directory dice «cancellata».

La traduzione di `i_mode` verso `enum inode_type` guarda i bit di tipo, i quattro
alti:

```text
0o040000  directory   →  INODE_DIR
0o100000  file        →  INODE_FILE
0o020000  chardev     →  INODE_CHARDEV     (nell'immagine di M11a non ce ne sono)
altro                 →  INODE_NONE, e si rifiuta
```

Misurato: la radice ha `i_mode = 16877 = 0o40755`; `hello.txt` ha
`33188 = 0o100644`.

`priv` di `struct inode` serve qui: ci vanno le nove zone, perché
`struct inode` del VFS non ha un posto dove metterle. Una struct per slot,
parallela alla cache — o meglio, un campo dentro la struct dello slot, così
`priv` punta a qualcosa che vive quanto l'inode.

**Ritorna.** Il puntatore, oppure `0`. E il puntatore deve essere **stabile**:
la stessa `ino` deve dare lo stesso indirizzo per sempre.

**Non deve** rileggere dal disco un inode che ha già. Non per velocità — perché
due copie possono divergere.

**Chi la chiama.** `minix_lookup`, `minixfs_root`, e in M11b la creazione.

**Come si sbaglia.**

- **`ino * 32` invece di `(ino - 1) * 32`.** Tutti gli inode slittano di uno, e
  la radice sembra funzionare: l'inode 0 letto come 1 è tutto zeri, cioè una
  directory vuota, che non è un errore. Il guasto compare al primo file;
- **cercare uno slot libero con `inodi[i].ino == 0`** e poi non impostare `ino`
  prima di ritornare: lo slot resta libero e il prossimo `lookup` lo riusa,
  cioè la cache non funziona e i puntatori diventano instabili;
- **16 inode per blocco scritto come una costante nuda.** È `1024 / 32`, e
  scriverlo così dice perché.

**Test.** Sull'host: caricare l'inode 1 e verificare che sia una directory con
la `i_size` che `ls` conferma; caricarlo due volte e verificare che il puntatore
sia lo **stesso**; chiedere l'inode 0 e `sb.ninodes + 1` e verificare che diano
`0`.

---

## 4. `zona_di`

```c
static uint32_t zona_di(struct inode *ino, uint32_t n);
```

**Compito.** Data la zona logica `n` di un file — il byte `n * 1024` — dire quale
zona fisica la contiene. È il cuore di M11a.

```text
n < 7                    →  zone[n]                         nessuna lettura in piu'
n < 7 + 512              →  leggi zone[7], prendi [n - 7]   UNA lettura
n < 7 + 512 + 512*512    →  doppio indiretto, zone[8]       DUE letture
oltre                    →  0
```

512 perché un blocco da 1024 byte contiene 512 puntatori da **`uint16`**.

Verificato su un file da 20000 byte: `zone[0..6] = 35..41`, e `zone[7] = 42` è il
blocco indiretto, che contiene `43 44 45 46 47 48 49 50 51 52 53 54 55 0 0 …`.

**Ritorna.** Il numero di zona, oppure **`0`, che significa BUCO** e non errore.
Un file sparso ha zone non allocate in mezzo, e `minix_read` deve leggerle come
zeri. Quando il file finisce lo dice `i_size`, non la prima zona nulla.

Un puntatore indiretto nullo si comporta allo stesso modo: se `zone[7]` è `0` e
`n >= 7`, il buco è l'intero intervallo, e **non** si legge il blocco 0 — che
sarebbe il boot block, e verrebbe interpretato come una tabella di puntatori.

**Non deve** leggere il blocco indiretto quando `n < 7`. È il caso comune —
tutti i file sotto i 7 KB — e una lettura in più per zona raddoppierebbe il costo
di ogni `read`.

**Chi la chiama.** Solo `minix_read`, una volta per zona.

**Come si sbaglia.**

- **i puntatori letti come `uint32`.** In minix **v1** sono a 16 bit; è la v2 ad
  averli a 32. Con `uint32` si leggono 256 puntatori a caso, e il sintomo è che
  i file piccoli funzionano e quelli oltre 7 KB no — cioè il guasto sembra
  dipendere dalla dimensione invece che dal tipo;
- **`zone[7]` trattata come la zona numero 7 dei dati.** È il *blocco che
  contiene i puntatori*, non un blocco di dati. Con questo errore un file da
  20000 byte legge il blocco 42 — la tabella dei puntatori — come se fossero
  byte, e il dump mostra numeri piccoli crescenti al posto del testo;
- **il confine a 7 scritto 8.** L'ottava zona logica è la prima indiretta;
- **il blocco indiretto riletto per ogni zona.** Corretto ma lento: tredici
  letture in più su un file da 20 KB. In M11a è accettabile e vale la pena
  saperlo.

**Test.** Sull'host, ed è **il test più importante di M11a**: leggere
`/enorme.txt` per intero e verificare che siano 20000 caratteri `Z`. Un errore
nell'indiretto non può sopravvivere a quel confronto. Più i casi di confine: la
zona 6 (l'ultima diretta) e la 7 (la prima indiretta), che vanno provate
singolarmente perché è lì che il `>` diventa `>=`.

---

## 5. `minix_read`

```c
static int minix_read(struct inode *ino, uint32_t off, void *buf, uint32_t n);
```

**Compito.** Leggere `n` byte a partire da `off`. È la `read` di `inode_ops`,
quindi la firma non la scegli tu.

La complicazione, ed è tutta qui: **il VFS chiede byte, il disco dà blocchi.**
Una lettura di 100 byte dall'offset 1000 attraversa due zone.

```text
se off >= ino->size                →  0        ("finito", non errore)
se off + n > ino->size             →  n = ino->size - off       ← TRONCA
finche' restano byte:
    zona logica    = off / 1024
    dentro la zona = off % 1024
    quanti         = min(1024 - dentro, rimasti)
    z = zona_di(ino, zona logica)
    se z == 0   →  azzera "quanti" byte           il BUCO
    altrimenti  →  leggi il blocco z, copia "quanti" byte da "dentro"
    avanza
```

**Il troncamento su `i_size` è il punto.** Senza, un file da 26 byte restituisce
1024 byte, di cui 998 sono quello che c'era sul disco prima — e `cat` li stampa.
Non è spazzatura casuale: sono **dati veri di qualcun altro**, quindi hanno
l'aria di essere giusti.

**Ritorna.** Quanti byte ha copiato davvero, `0` se `off` ha raggiunto `size`,
`-1` su errore di disco. È la convenzione di M8, ereditata due volte: su un file
regolare lo zero significa fine, e chi legge lo distingue guardando il tipo
dell'inode.

**Non deve** avanzare nessuna posizione. La posizione sta in `struct file`, e il
VFS passa `off` esplicito — è la decisione di interfaccia di M9a, e qui si vede a
cosa serviva: `minix_read` è pura, e due descrittori sullo stesso file non si
disturbano.

**Non deve** allocare un buffer da 1024 byte a ogni chiamata sullo stack di un
task da 4096. Uno `static` va bene in M11a — c'è un consumatore alla volta — ma
va scritto perché è la prima cosa da cambiare quando arriveranno più shell.

**Chi la chiama.** `vfs_read`, attraverso `ops->read`. Cioè `cat`, senza
saperlo.

**Come si sbaglia.**

- **`i_size` ignorata** (sopra). È il bug numero uno di M11a;
- **`off + n` che va in overflow** nel confronto con `size`: la stessa lezione
  di `ata_range_ok`, e si confronta per sottrazione;
- **la zona nulla trattata come fine del file.** Un file sparso finisce presto e
  in silenzio;
- **leggere il blocco intero quando ne servono 10 byte** — corretto, e non è un
  bug: senza cache non c'è altro modo.

**Test.** Sull'host, contro contenuti che `mount` e `cat` sull'host confermano:
`/hello.txt` per intero (26 byte esatti, non 1024); `/enorme.txt` per intero
(20000 `Z`); una lettura a cavallo di due zone; una che comincia oltre `i_size`;
`/vuoto.txt`, che deve dare `0` subito.

---

## 6. `minix_lookup`

```c
static int minix_lookup(struct inode *dir, const char *name, struct inode **out);
```

**Compito.** Cercare `name` fra le voci di `dir` e consegnarne l'inode.

**Una directory è un file normale** il cui contenuto sono voci da 16 byte. Quindi
`lookup` **non legge il disco da sé**: scorre `dir` con la stessa `minix_read` che
serve i file, e si ferma quando ha trovato o quando `read` dà `0`.

```text
per ogni voce (16 byte) letta da dir:
    ino  = i primi due byte
    nome = i quattordici che seguono

    se ino == 0        →  voce cancellata, si SALTA (non e' la fine)
    se nome == name    →  *out = inode_carica(ino);  ritorna 0
ritorna -1
```

**Il confronto dei nomi è la trappola.** Il nome su disco è lungo **al massimo**
14 e **non è terminato** se ne occupa esattamente 14:

```text
02 00 68 65 6c 6c 6f 2e 74 78 74 00 00 00 00 00     "hello.txt", terminato
07 00 6e 6f 6d 65 5f 64 61 5f 71 75 61 74 74 6f     14 caratteri, NON terminato
```

Un `strcmp` diretto sul campo cammina nella voce successiva. Si copia in un
buffer da `VFS_NAME_MAX + 1` terminandolo a mano, oppure si confronta al massimo
14 caratteri **e** si verifica che `name` non sia più lungo — altrimenti
`"nome_da_quatto"` e `"nome_da_quattordici"` risulterebbero uguali. È lo stesso
difetto che in M8 faceva rifiutare i nomi troppo lunghi invece di troncarli.

**Ritorna.** `0` con `*out` scritto, `-1` altrimenti — e su `-1` **`*out` non si
tocca**. È il bug di `root_lookup` in M9b, dove un `return 1` faceva credere a
`vfs_resolve` di aver trovato: costa una riga e nessun controllo positivo lo
vede.

**Non deve** trattare `ino == 0` come fine dell'elenco. È una voce **cancellata**
in mezzo a voci valide, e fermarsi lì fa sparire tutto quello che segue. In
M11a `mkfs.minix` non ne produce; in M11b, dopo il primo `unlink`, sì.

**Non deve** cercare `.` e `..` in modo speciale: sono voci normali, e sul disco
ci sono davvero — misurato, sono le prime due della radice.

**Chi la chiama.** `vfs_resolve`, attraverso `ops->lookup`, una volta per
componente del path.

**Come si sbaglia.** Oltre ai tre sopra: **azzerando la propria posizione a ogni
voce** invece di avanzare di 16, cioè leggendo la prima voce all'infinito.

**Test.** Sull'host, contro quello che `ls` mostra sull'immagine montata:
`hello.txt` si trova, `dir` si trova ed è una directory, `nonesiste` no,
`hello.tx` no — il prefisso non deve bastare, ed è il controllo che prende il
confronto troncato.

---

## 7. `minix_readdir`

```c
static int minix_readdir(struct inode *dir, int idx, char *name, uint32_t *ino_out);
```

**Compito.** La voce numero `idx`. È l'inverso di `lookup`, e insieme sono tutto
ciò che una directory sa fare.

```text
leggi 16 byte all'offset idx * 16
se read ha dato meno di 16     →  0        elenco finito
copia il nome, TERMINANDOLO
*ino_out = il numero letto
ritorna 1
```

**Ritorna.** I tre valori del contratto: `1` c'è, `0` oltre l'ultima, `-1` la
domanda non aveva senso. Il ciclo di `ls` si ferma sullo zero.

**La domanda scomoda: le voci cancellate.** `lookup` le salta; `readdir` non
può, perché `idx` è la posizione e saltare renderebbe gli indici non stabili —
`ls` chiede 0, 1, 2… e se la voce 1 sparisse, la 2 diventerebbe la 1.

Due scelte difendibili: restituire `1` con un nome vuoto, oppure restituire `1`
e lasciare a `ls` il compito di ignorare `*ino_out == 0`. **Scegli, e scrivilo**
— in M11a non si vede perché voci cancellate non ce ne sono, e in M11b si vedrà
subito.

**Non deve** rileggere l'intera directory per arrivare alla voce `idx`. Con
`minix_read` e un offset non serve: `idx * 16` è direttamente la posizione.

**Chi la chiama.** `vfs_readdir`, cioè `ls`.

**Come si sbaglia.**

- **il nome non terminato** — stesso punto di `lookup`, e qui il danno è
  peggiore: `ls` stamperebbe il nome più tutto quello che segue nel buffer;
- **`idx * 16` calcolato in `int`.** Con directory grandi non è un problema in
  M11a; scriverlo `uint32_t` costa zero;
- **`readdir` e `lookup` in disaccordo.** Sono due funzioni che descrivono lo
  stesso insieme: qui leggono entrambe dal disco con la stessa `minix_read`,
  quindi il rischio è basso — ma è il difetto che in M9b si sarebbe visto come
  «`ls` mostra un nome che `cat` non apre».

**Test.** Sull'host: le voci della radice, nell'ordine, confrontate con il dump
reale — `.`, `..`, `hello.txt`, `etc`, `grande.txt`, `enorme.txt`, `vuoto.txt`.
Più il ritorno `0` un indice oltre l'ultima.

---

## 8. `minixfs_root`

```c
struct inode *minixfs_root(void);
```

**Compito.** L'inode 1, da passare a `vfs_init`.

**Ritorna.** `inode_carica(1)`, oppure `0` se `minixfs_init` non è riuscita. Il
controllo non è decorativo, ed è la stessa nota di `devfs_root`: con una radice
di zeri il VFS avrebbe `type` `INODE_NONE` e `ops` nullo, quindi **ogni** resolve
fallirebbe senza dire perché.

**Non deve** rileggere l'inode 1 ogni volta. Passa dalla cache, quindi è gratis —
ma va detto, perché la tentazione di tenere un puntatore separato porta a due
verità.

**Chi la chiama.** `kmain`, una volta. Nei test host, all'inizio di ogni
verifica.

**Come si sbaglia.** Restituendo un puntatore a una `static struct inode` propria
invece di quello della cache: due `struct inode` per lo stesso file, e la
`i_size` di una può divergere dall'altra.

**Test.** Non nullo dopo un mount riuscito, nullo dopo uno fallito, ed è una
`INODE_DIR` — tre controlli separati, perché una radice mai riempita passa il
primo.

---

## 9. `minixfs_graft`

```c
int minixfs_graft(const char *nome, struct inode *root);
```

**Compito.** Innestare un altro filesystem sotto un nome della radice. **Uno
slot, non una tabella di mount** — quella è fuori scope nello spec.

In pratica: `minix_lookup`, quando `dir` è la radice, controlla **prima**
l'innesto e poi il disco.

```text
in minix_lookup, se dir e' la radice e nome == innesto.nome:
    *out = innesto.root
    ritorna 0
```

**Riceve un `struct inode *` e non sa da dove viene.** `kmain` gli passa
`devfs_root()`, i test un albero finto. È lo stesso espediente di
`vfs_init(root)` e del sink di eco di `lineedit`, ed è ciò che permette a
`minixfs.c` di **non includere `devfs.h`** — cioè al filesystem su disco di non
sapere che esistano i dispositivi.

**Ritorna.** `0`, oppure `-1` se lo slot è già preso, se il mount non è riuscito,
o se il nome è più lungo di `VFS_NAME_MAX`. Rifiutare invece di sostituire: un
secondo innesto silenzioso sarebbe una directory che cambia sotto i piedi.

**Non deve** creare una voce sul disco. L'innesto vive in RAM, e l'immagine non
si tocca — infatti non esiste nessuna directory `dev` dentro
`tests/data/minix.img`. È la stessa cosa che fa Unix: montare non scrive niente
sul filesystem montante.

**Non deve** comparire in `minix_readdir` per sbaglio — e **deve** comparirci di
proposito, altrimenti `ls /` non mostra `dev`. Che è la sottigliezza della
scheda: `lookup` e `readdir` devono restare d'accordo, e qui la fonte è doppia
— il disco più uno slot. Il modo più semplice è che `readdir` sulla radice
tratti l'innesto come una voce in più, all'indice `numero_voci_su_disco`.

**Chi la chiama.** `kmain`, subito dopo `minixfs_init` e prima di `vfs_init`.

**Come si sbaglia.**

- **copiando il puntatore al nome invece del nome.** È la regola di
  `device_register` in M8: `nome` dentro la struct dello slot deve essere un
  **array**, perché il chiamante può passare un letterale oggi e un buffer
  domani;
- **innestando prima del mount.** `minixfs_init` azzera l'innesto, quindi
  l'ordine sbagliato lo cancella in silenzio. Da cui il `-1` se il mount non è
  riuscito;
- **dimenticando `readdir`** (sopra): `cat /dev/kbd` funziona e `ls /` non
  mostra `dev`.

**Test.** Sull'host con un inode finto: `lookup("dev")` sulla radice lo trova,
`readdir` lo elenca, e un `lookup` di un nome che esiste **anche** sul disco
continua a trovare quello del disco.

---

## Riepilogo

| funzione | righe circa | cosa dimostra |
|---|---|---|
| `blocco_leggi` | 10 | che un blocco sono due settori, in un posto solo |
| `minixfs_init` | 45 | che un filesystem si valida prima di crederci |
| `inode_carica` | 50 | perché la cache è correttezza e non velocità |
| `zona_di` | 45 | l'indiretto, cioè il pezzo che i file piccoli non provano |
| `minix_read` | 55 | che `i_size` è l'unica cosa che dice dove finisce un file |
| `minix_lookup` | 45 | che una directory è un file |
| `minix_readdir` | 30 | l'inverso, e perché gli indici devono restare stabili |
| `minixfs_root` | 8 | — |
| `minixfs_graft` | 20 | che montare è una domanda a cui risponde `lookup` |

Circa 310 righe. E alla fine, la riga che è il punto di tutto il blocco:

```c
cat /etc/motd
```

Lo stesso `shell_cat` scritto in M9b, senza una modifica, che legge da un disco
attraverso quattro strati — VFS, minixfs, blockdev, ATA — invece che da una
tastiera attraverso quattro strati diversi.
