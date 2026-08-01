# M10 — cosa deve fare ogni funzione

Companion di `2026-08-01-waltex-m10-ata.md`. Una scheda per funzione, sei voci
fisse: **compito**, **ritorna**, **non deve**, **chi la chiama**, **come si
sbaglia**, **test**.

## Indice

1. [`ata_wait`](#1-ata_wait)
2. [`ata_identify`](#2-ata_identify)
3. [`ata_init`](#3-ata_init)
4. [`ata_dev_read`](#4-ata_dev_read)
5. [`ata_dev_write`](#5-ata_dev_write)
6. [`ata_drive` e `ata_drive_count`](#6-ata_drive-e-ata_drive_count)
7. [`shell_blk`](#7-shell_blk)
8. [`shell_rdsect`](#8-shell_rdsect)
9. [`shell_wrsect`](#9-shell_wrsect)

---

## Il protocollo, una volta sola

Le porte del canale primario, e cosa significano. Sono le stesse per ogni
comando: cambia solo il byte che si scrive in `0x1F7`.

| porta | in scrittura | in lettura |
|---|---|---|
| `0x1F0` | dati, **16 bit** | dati, 16 bit |
| `0x1F1` | features | codice d'errore |
| `0x1F2` | quanti settori | — |
| `0x1F3` | LBA bit 0-7 | — |
| `0x1F4` | LBA bit 8-15 | — |
| `0x1F5` | LBA bit 16-23 | — |
| `0x1F6` | **drive + LBA bit 24-27** | — |
| `0x1F7` | il comando | lo **stato** |
| `0x3F6` | controllo | stato **senza** togliere l'IRQ |

I bit di `0x1F7`, e l'ordine in cui vanno guardati:

```text
bit 7  BSY   occupato.  FINCHE' E' ACCESO GLI ALTRI SETTE NON SIGNIFICANO NIENTE
bit 6  DRDY  pronto a ricevere un comando
bit 5  DF    guasto del dispositivo
bit 3  DRQ   ha dati da trasferire, o li aspetta
bit 0  ERR   errore: il motivo sta in 0x1F1
```

Il byte di `0x1F6` è bit-packing, la categoria di bug più costosa del progetto:

```text
  7   6   5   4    3   2   1   0
  1  LBA  1  DRV   ─── LBA 27-24 ───

0xE0 | (drive << 4) | ((lba >> 24) & 0x0F)
```

`0xE0` accende i due bit fissi e il bit LBA. `drive` è 0 per il master e 1 per lo
slave. I quattro bit bassi sono la parte alta dell'indirizzo — **è l'unico posto
in cui indirizzo e selezione del disco convivono nello stesso registro**, ed è
esattamente il genere di dimenticanza che in M2 mangiava il limite della GDT.

Le porte del canale secondario sono `0x170-0x177` e `0x376`. **In M10 non le
tocchiamo**: un disco basta, e il secondo canale raddoppierebbe la superficie
senza aggiungere un concetto.

---

## 1. `ata_wait`

```c
static int ata_wait(uint16_t base, uint8_t mask, uint8_t val, int limite);
```

**Compito.** Girare finché i bit selezionati da `mask` nel registro di stato non
valgono `val`, o finché non scadono i tentativi. È l'unica attesa del file: ogni
altra funzione ci passa.

I due usi:

```text
ata_wait(base, BSY, 0, N)          aspetta che il disco si liberi
ata_wait(base, BSY|DRQ, DRQ, N)    aspetta che sia libero E abbia dati
```

Il secondo è quello che conta, e la maschera doppia non è un vezzo: **DRQ va
guardato solo con BSY spento**. Chiederli insieme in una condizione sola è ciò
che rende impossibile leggerli nell'ordine sbagliato.

**Ritorna.** `0` se la condizione si è verificata, `-1` se sono scaduti i
tentativi o se ERR o DF si sono accesi durante l'attesa.

Controllare ERR **dentro** il ciclo, non dopo: su un comando rifiutato BSY si
spegne e DRQ non si accende mai, quindi un'attesa che guarda solo i due bit
richiesti arriva fino in fondo al tetto e riporta «timeout» invece di «errore».
Due diagnosi diverse per la stessa causa.

**Non deve** girare senza limite. È la trappola numero uno di M10: su un canale
vuoto il bus fluttuante legge `0xFF` — cioè BSY acceso — e il kernel si ferma
dopo «timer a 100 Hz» senza un messaggio, con lo stesso sintomo di una tripla
fault.

**Non deve** leggere `0x1F7` per il polling quando esiste `0x3F6`: leggere il
registro di stato principale **cancella l'interrupt pendente**. In M10 non usiamo
gli interrupt e quindi non si vede, ma la lettura alternata è gratis e non lascia
una mina per il giorno in cui si passasse all'IRQ 14.

**Chi la chiama.** `ata_identify`, `ata_dev_read`, `ata_dev_write`.

**Come si sbaglia.**

- il tetto come `int` che si decrementa **e** una condizione `>= 0`: con
  `unsigned` non termina mai;
- controllare ERR dopo il ciclo invece che dentro (sopra);
- un tetto troppo basso. Su QEMU un settore è istantaneo, su hardware vero un
  disco che si sveglia prende secondi. Cento milioni di giri è un numero
  ragionevole e non costa niente sul caso normale.

**Test.** Nessuno diretto: si esercita attraverso gli altri. Il controllo che la
riguarda è indiretto e vale più di uno diretto — **il kernel boota**, con e senza
disco attaccato.

---

## 2. `ata_identify`

```c
static int ata_identify(uint16_t base, int drive, uint32_t *nsectors);
```

**Compito.** Chiedere al disco chi è, e ricavarne la capacità.

La sequenza, e l'ordine è vincolato:

```text
1.  0x1F6  ←  0xA0 | (drive << 4)      seleziona il disco (qui senza bit LBA)
2.  quattro letture di 0x3F6           i 400 ns
3.  0x1F2..0x1F5  ←  0                 azzerare e' parte del protocollo
4.  0x1F7  ←  0xEC                     IDENTIFY DEVICE
5.  leggi 0x1F7
      == 0        →  nessun disco, ritorna -1
6.  aspetta BSY spento
7.  se LBA mid o LBA high != 0  →  non e' un disco ATA (e' ATAPI), -1
8.  aspetta DRQ
9.  insw(0x1F0, buf, 256)               256 word = 512 byte
10. nsectors = word[60] | (word[61] << 16)
```

Il passo 5 è la rilevazione vera: **stato zero significa che non c'è niente**. Il
bus fluttuante darebbe `0xFF`, un disco presente qualcosa con DRDY. Lo zero è
l'unico valore che nessun dispositivo produce.

Il passo 7 esiste perché QEMU può presentare un CD-ROM sullo stesso canale, e un
dispositivo ATAPI risponde a IDENTIFY con un errore dopo aver messo la sua firma
(`0x14 0xEB`) nei registri LBA. Senza il controllo, si finisce ad aspettare un
DRQ che non arriva.

**I 400 ns del passo 2 si fanno leggendo `0x3F6` quattro volte**, non con un
ciclo vuoto. Il chip vuole quattro cicli di bus; un `for` vuoto il compilatore lo
può eliminare, e con `volatile` diventa comunque un'attesa di durata sconosciuta.

**Ritorna.** `0` con `*nsectors` scritto, `-1` se il disco non c'è o non è ATA.
Su `-1` **non tocca `*nsectors`** — la convenzione di `vfs_resolve` e di
`shell_parse_hex`, e per la stessa ragione: il chiamante deve poter tenere quello
che aveva.

**Non deve** interpretare il resto dei 512 byte. Il modello, il numero di serie e
i bit di capacità sono informazione che oggi non usiamo, e leggerla vorrebbe dire
scrivere il codice che la corregge — le stringhe di IDENTIFY hanno i byte
scambiati a coppie, il che è divertente e inutile.

**Chi la chiama.** Solo `ata_init`, due volte.

**Come si sbaglia.**

- **word 60-61 lette come una sola.** La capacità si tronca a 65535 settori. Su
  un'immagine da 2048 non si vede: il self-check sul numero esatto esiste per
  questo, e in M11 l'immagine sarà più grande;
- leggere meno di 256 word. Il disco resta con DRQ acceso e il **comando
  successivo** si comporta in modo assurdo. Il guasto si presenta altrove;
- saltare i 400 ns. Su QEMU funziona sempre.

**Test.** Il self-check sulla capacità: `2048`, il numero che `mkdisk.sh` ha
scelto. Prende insieme IDENTIFY e la costruzione dell'immagine.

---

## 3. `ata_init`

```c
void ata_init(void);
```

**Compito.** Provare i due dischi del canale primario e riempire una
`struct blockdev` per ognuno che risponde.

```text
per drive in 0, 1:
    se ata_identify(0x1F0, drive, &n) == 0:
        drives[ndrives].name     = "hda" oppure "hdb"
        drives[ndrives].nsectors = n
        drives[ndrives].read     = ata_dev_read
        drives[ndrives].write    = ata_dev_write
        drives[ndrives].priv     = ...        ← QUAL E' IL DISCO
        ndrives++
```

**La riga di `priv` è tutta la milestone.** È il primo posto del progetto in cui
`priv` serve davvero: due `struct blockdev` con lo **stesso** puntatore a `read`,
e l'unica cosa che le distingue è cosa c'è lì dentro. In M8 `priv` non lo usava
nessuno e la sua ragione d'essere era una promessa; qui la promessa si riscuote.

Cosa metterci: il numero del drive (0 o 1) e la porta base, che in M10 è sempre
`0x1F0` ma non lo sarà se un giorno arriva il canale secondario. Una piccola
struct statica per disco, non un intero castato a puntatore.

**Ritorna.** Niente. **Un canale vuoto non è un errore**: è una configurazione
legittima, e `make run` senza `-drive` deve continuare a funzionare.

**Non deve** fare `assert` sulla presenza di un disco. È la differenza con
`device_register`, dove l'assert è giusto perché un driver che non riesce a
iscriversi è un bug nostro; qui l'assenza è una proprietà dell'ambiente.

**Chi la chiama.** `kmain`, dopo `keyboard_init()`. In polling non ha vincoli
d'ordine veri — la posizione è solo la convenzione del progetto, ogni sottosistema
con la sua `*_init()` esplicita in ordine visibile.

**Come si sbaglia.**

- iscrivere il disco **prima** di sapere la capacità, e lasciare `nsectors` a
  zero. Ogni controllo di limite in `read` passerebbe o fallirebbe a caso;
- usare un array locale per `priv`. Vale la regola di M8, un piano sotto: quando
  la funzione ritorna, quella memoria non è più sua. Qui però è al contrario di
  M8 — `blockdev_register` non esiste, quindi **nessuno copia**, e `struct
  blockdev` deve essere `static` lei stessa.

**Test.** `ata_drive_count() == 1`, `ata_drive(0) != 0`, `ata_drive(1) == 0`.

---

## 4. `ata_dev_read`

```c
static int ata_dev_read(struct blockdev *b, uint32_t lba, void *buf, uint32_t count);
```

**Compito.** Leggere `count` settori a partire da `lba` dentro `buf`.

```text
se count == 0                      →  ritorna 0, senza toccare niente
se lba + count > b->nsectors       →  ritorna -1
                                      (e attenzione all'overflow della somma)

aspetta BSY spento
0x1F6  ←  0xE0 | (drive << 4) | ((lba >> 24) & 0x0F)
i 400 ns
0x1F2  ←  count
0x1F3  ←  lba & 0xFF
0x1F4  ←  (lba >> 8) & 0xFF
0x1F5  ←  (lba >> 16) & 0xFF
0x1F7  ←  0x20                     READ SECTORS

per ogni settore:
    aspetta BSY spento E DRQ acceso
    insw(0x1F0, p, 256)
    p += 512
```

**L'attesa sta dentro il ciclo, una per settore.** Il disco alza DRQ, consegna un
settore, lo riabbassa, poi ne prepara un altro. Aspettare una volta sola e poi
leggere `count * 256` word è il modo di ottenere un buffer con il primo settore
giusto e il resto spazzatura.

**Ritorna.** `count` in caso di successo, `-1` altrimenti. **Settori, non byte** —
e la differenza con `struct device` è deliberata: qui non esistono trasferimenti
parziali. O il settore intero, o l'errore.

**Non deve** avanzare nessuna posizione. Un `blockdev` non ha una posizione:
l'indirizzo arriva a ogni chiamata. È lo stesso motivo per cui `inode_ops->read`
riceve l'offset esplicito invece di tenerlo — la posizione appartiene a chi
legge, non a cosa si legge.

**Chi la chiama.** Attraverso `b->read`, mai per nome: `shell_rdsect` in M10, e
`minixfs` in M11, che è il chiamante vero e che non saprà mai che dietro c'è ATA.

**Come si sbaglia.**

- **`lba + count` che va in overflow.** Con `lba` vicino a 2³², la somma gira e
  il controllo di limite passa. Si confronta `lba > nsectors - count` con
  `count` già verificato non nullo, oppure si promuove a 64 bit;
- `insw` con `count` in byte invece che in word. Metà buffer, e l'altra metà del
  settore resta nel disco a confondere il comando dopo;
- `insw` senza `cld`. Il buffer si riempie all'indietro: leggibile, ordinato,
  sbagliato;
- dimenticare che `0x1F2 ← 0` significa **256 settori**, non zero. È il motivo
  per cui il caso `count == 0` va intercettato prima, non lasciato passare.

**Test.** Il self-check che rilegge il settore 1 e lo confronta byte per byte con
il pattern che `mkdisk.sh` ha scritto **dall'host**. È l'unico controllo di M10
che può accorgersi di un driver che legge il settore sbagliato in modo coerente:
tutti gli altri sono d'accordo con se stessi.

---

## 5. `ata_dev_write`

```c
static int ata_dev_write(struct blockdev *b, uint32_t lba, const void *buf, uint32_t count);
```

**Compito.** Uguale alla lettura, con tre differenze.

```text
comando 0x30 invece di 0x20
outsw invece di insw
DOPO l'ultimo settore:  0x1F7 ← 0xE7   FLUSH CACHE, e si aspetta BSY spento
```

**Il flush non è opzionale.** Senza, il disco può tenere i dati in un buffer
proprio: su hardware vero si perdono a un reset, su QEMU possono non raggiungere
il file. È metà della difesa del test bidirezionale — l'altra metà è
`cache=writethrough` nella riga di QEMU, dall'altro lato del confine.

**Ritorna.** `count`, oppure `-1`. Se il flush fallisce **si ritorna `-1` anche
se i settori sono partiti**: dal punto di vista di chi ha chiamato, un dato che
non è garantito su disco non è scritto.

**Non deve** leggere-modificare-riscrivere. Non c'è granularità sotto il settore,
e chi vuole cambiare un byte legge il settore, lo modifica e lo riscrive — lo
farà `minixfs` in M11, ed è suo compito, non di questo strato.

**Chi la chiama.** `shell_wrsect` in M10, `minixfs` in M11.

**Come si sbaglia.**

- **il flush dimenticato**: tutto funziona, i self-check passano, e
  `tests/disk.sh` diventa intermittente. È il passo 4 del Task 5 del piano —
  togliere il flush di proposito e verificare che il test diventi rosso;
- `outsw` con un `const void *` senza il cast giusto: il vincolo `"+S"` vuole un
  puntatore modificabile, e forzarlo con un cast che butta il `const` va fatto
  in modo visibile, non di nascosto;
- controllare ERR solo alla fine. Un settore rifiutato a metà lascia gli altri
  scritti, e riportare successo è peggio che riportare un errore parziale.

**Test.** Il self-check che scrive nel settore 2 e rilegge. E `tests/disk.sh`,
che è lo stesso controllo fatto **dall'altro lato**: l'unico che distingue «il
kernel crede di aver scritto» da «sul file c'è».

---

## 6. `ata_drive` e `ata_drive_count`

```c
struct blockdev *ata_drive(int i);
int ata_drive_count(void);
```

**Compito.** Dare accesso all'array statico dentro `ata.c` senza renderlo
globale. È `device_at`/`device_count` di M8 e `task_slot` di M6a: la terza volta
che il progetto usa lo stesso espediente, e per la stessa ragione — un array
`static` che serve da fuori per l'enumerazione e per i test.

**Ritorna.** `ata_drive`: il disco `i`, oppure `0` se `i` è negativo o
`>= ata_drive_count()`. Il controllo sul negativo non è pedanteria — la nota è
già in `device.h`: `drives[-1]` legge i byte prima dell'array, che in `.bss` sono
un'altra variabile.

**Non deve** identificare niente al volo. Se `ata_init` non è stata chiamata,
`ata_drive_count()` è zero e `ata_drive(0)` è `0`. Nessuna inizializzazione
implicita o lazy: è un vincolo di `CLAUDE.md`.

**Chi la chiama.** `kmain` per stampare il marker, `selftest.c`, `shell_blk`, e
in M11 `minixfs_init(ata_drive(0))`.

**Come si sbaglia.** Restituendo l'indirizzo di una `struct blockdev` locale, o
una copia. Deve essere l'indirizzo dello slot vero: chi lo riceve chiamerà
`b->read(b, ...)` e `b` deve essere quello con il `priv` giusto.

**Test.** I primi due self-check di M10.

---

## 7. `shell_blk`

```c
static void shell_blk(int argc, char **argv);
```

**Compito.** Elencare i dischi con la loro capacità. È `devs` per i blocchi.

```text
waltex> blk
  hda  2048 settori  (1024 KB)
```

I kilobyte si calcolano `nsectors / 2`, e vale la pena stamparli: `2048 settori`
non dice niente a colpo d'occhio, `1024 KB` sì.

**Ritorna.** Niente, come tutti i comandi.

**Non deve** stampare niente su VGA soltanto. Vale il vincolo di `CLAUDE.md`:
`kprintf` e basta, perché la seriale è ciò che leggono i test.

**Chi la chiama.** `shell_exec`, attraverso la tabella.

**Come si sbaglia.** Dereferenziando `ata_drive(i)` senza controllare che non sia
nullo, in un ciclo che va oltre `ata_drive_count()`. È lo stesso `>=` mancante
che in M9b faceva stampare a `ls /dev` una quarta voce di spazzatura — e senza
paging l'indirizzo 0 è leggibile, quindi non c'è nessuna fault a fermarti.

**Test.** A mano. Nessuno automatico: la capacità è già coperta dai self-check, e
un test sull'incolonnamento si romperebbe a ogni ritocco estetico.

---

## 8. `shell_rdsect`

```c
static void shell_rdsect(int argc, char **argv);
```

**Compito.** Leggere un settore e farne il dump esadecimale. È lo strumento con
cui ispezionerai il superblocco minix per tutta M11 — quindi vale la pena farlo
comodo adesso.

```text
waltex> rdsect 1
  0000:  03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
  0010:  73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
  ...
```

**Il numero del settore in decimale, non in esadecimale.** `peek` prende
l'indirizzo in esadecimale perché gli indirizzi si scrivono così; i numeri di
settore no, e in M11 li leggerai dal superblocco in decimale. Serve quindi un
`shell_parse_dec` accanto a `shell_parse_hex` — quindici righe, e lo stesso
contratto: `*out` scritto solo in caso di successo, perché `0` è un risultato
valido e «fallito» no.

**La formattazione esiste già** dentro `shell_peek`. Questa è la prima volta che
due comandi vogliono lo stesso output, e conviene estrarla — una `hexdump(const
void *, uint32_t n, uint32_t base)` che entrambi chiamano. Riscriverla vuol dire
due incolonnamenti che divergeranno.

**Il buffer da 512 byte sullo stack** è un ottavo dello stack di un task. Va
bene, ed è visibile: è la ragione per cui la dimensione degli stack sta in
`CLAUDE.md` invece di essere un dettaglio di `task.c`.

**Ritorna.** Niente.

**Non deve** stampare 512 byte quando l'utente ne voleva 16. Un secondo
argomento opzionale per la lunghezza, come `peek`, e un valore predefinito
ragionevole: 512 righe di dump riempiono lo schermo trentadue volte.

**Chi la chiama.** `shell_exec`.

**Come si sbaglia.** Ignorando il valore di ritorno di `read` e facendo il dump
comunque: con un LBA fuori range si stamperebbero 512 byte di stack non
inizializzato, con l'aria di essere dati veri.

**Test.** A mano, contro il pattern noto: `rdsect 1` deve cominciare con
`03 0a 11 18`.

---

## 9. `shell_wrsect`

```c
static void shell_wrsect(int argc, char **argv);
```

**Compito.** Riempire un settore con un pattern dato e scriverlo.

```text
waltex> wrsect 2 deadbeef
  scritti 512 byte nel settore 2
```

**Un settore si scrive intero, sempre.** Non esiste la scrittura parziale, quindi
i byte dell'argomento vanno estesi a 512 in un modo che devi **scegliere e
dichiarare nella riga di help**: ripetendo il pattern fino in fondo, oppure
azzerando il resto. Le due cose sono ugualmente difendibili e distinguibili solo
rileggendo — che è precisamente il motivo per cui va scritto.

**Ritorna.** Niente, ma **deve dire cosa ha fatto**. Un comando che scrive su
disco e tace è un comando di cui non ti fidi il giorno in cui M11 non parte.

**Non deve** accettare un settore senza limiti. `wrsect 0 ff` distrugge il boot
block, e in M11 quello sarà il superblocco: o si rifiuta il settore 0, o si
scrive nell'help che è permesso. Non a caso.

**Chi la chiama.** `shell_exec`, e `tests/disk.sh` attraverso `sendkeys.py`. È
l'unico comando del progetto **guidato da un test automatico** per provocare la
condizione da misurare, come `spin` in `tests/tasks.sh`.

**Come si sbaglia.**

- ignorando il ritorno di `write` e dichiarando successo. Il test bidirezionale
  lo prenderebbe, ma solo dopo aver fatto perdere tempo a capire da quale lato
  sta il problema;
- riusando il buffer di `rdsect` come variabile globale «tanto è grande». Due
  comandi che condividono un buffer sono due comandi che non possono girare da
  task diversi, e in M16 gireranno.

**Test.** `tests/disk.sh`: il comando lo digita il test, e poi l'**host** rilegge
`build/disk.img` con `od`. È il solo controllo del progetto in cui la verifica
avviene fuori dalla macchina che ha fatto il lavoro.

---

## Riepilogo

| funzione | righe circa | chi | cosa dimostra |
|---|---|---|---|
| `ata_wait` | 20 | CLAUDE | che un'attesa senza tetto è un kernel che non boota |
| `ata_identify` | 45 | CLAUDE | che un disco si presenta prima di servire |
| `ata_init` | 35 | CLAUDE | `priv`: due dischi, una sola `read` |
| `ata_dev_read` | 40 | CLAUDE | l'indirizzo esplicito, e nessuna posizione |
| `ata_dev_write` | 45 | CLAUDE | perché il flush esiste |
| `ata_drive` × 2 | 15 | CLAUDE | l'accesso a un array `static`, la terza volta |
| `shell_blk` | 15 | **WALTER** | — |
| `shell_rdsect` | 35 | **WALTER** | lo strumento di M11 |
| `shell_wrsect` | 35 | **WALTER** | il lato kernel del test bidirezionale |

Circa 200 righe di driver e 85 di shell. La milestone più corta del blocco, e
serve a consegnare a M11 tre righe:

```c
struct blockdev *disk = ata_drive(0);
disk->read(disk, lba, buf, 1);
disk->write(disk, lba, buf, 1);
```

Da lì in poi `minixfs.c` non saprà mai che esiste una porta `0x1F0`.
