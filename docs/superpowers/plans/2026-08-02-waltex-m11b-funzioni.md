# M11b — cosa deve fare ogni funzione

Companion di `2026-08-02-waltex-m11b-minix-scrittura.md`. Una scheda per
funzione, sei voci fisse: **compito**, **ritorna**, **non deve**, **chi la
chiama**, **come si sbaglia**, **test**.

I numeri vengono da `tests/data/minix.img` letta con `od`, non dalla memoria.

## Indice

1. [`bitmap_trova_libero`](#1-bitmap_trova_libero)
2. [`bitmap_accendi`](#2-bitmap_accendi)
3. [`inode_scrivi`](#3-inode_scrivi)
4. [`inode_alloca`](#4-inode_alloca)
5. [`zona_alloca`](#5-zona_alloca)
6. [`zona_assegna`](#6-zona_assegna)
7. [`minix_write`](#7-minix_write)
8. [`dirent_inserisci`](#8-dirent_inserisci)
9. [`minix_create`](#9-minix_create)
10. [`vfs_open` con `O_CREAT`](#10-vfs_open-con-o_creat)
11. [`vfs_mkdir`](#11-vfs_mkdir)
12. [`shell_mkdir`](#12-shell_mkdir)
13. [`shell_write`](#13-shell_write)

---

## I tre indici, una volta sola

La cosa da tenere in testa per tutta la milestone, perché **non si calcolano allo
stesso modo**:

```text
tabella degli inode:   inode i  →  blocco_inodi + (i-1)/32,  offset ((i-1)%32)*32
bitmap degli inode:    inode i  →  bit i                     NIENTE meno uno
bitmap delle zone:     zona z   →  bit z - s_firstdatazone + 1
```

La verifica, misurata:

| | valore reale | cosa dimostra |
|---|---|---|
| imap, primo byte | `ff` | 7 inode in uso (1-7) più il bit 0 riservato. Con l'indice `i-1` sarebbe `7f` |
| zmap, primi 4 byte | `ff ff ff 7f` | bit 0-30: il riservato più 30 zone. `firstdatazone` è 7 e le zone usate sono 7-36, quindi zona 7 → bit 1 |
| imap, byte 12 | `fe` | il bit 96 — l'ultimo inode valido — è libero; i bit 97-103 non esistono e `mkfs` li lascia a 1 |

**Il bit 0 è riservato in entrambe** e vale sempre 1. È il motivo per cui l'inode
0 non esiste, e per cui lo zero può significare «nessun inode» in una voce di
directory.

Quel `fe` merita una riga in più: un allocatore che non si ferma a `s_ninodes`
trova quei bit **già a 1**, quindi non alloca niente e non fa danni — **per
caso**. Il giorno che `mkfs` li lasciasse a zero, allocherebbe inode che non
esistono.

---

## 1. `bitmap_trova_libero`

```c
static uint32_t bitmap_trova_libero(uint32_t primo_blocco, uint32_t blocchi,
                                    uint32_t max_bit);
```

**Compito.** Il primo bit a zero, cercando dal bit 1.

```text
per ogni blocco della bitmap:
    leggilo
    per ogni byte:
        se vale 0xFF  →  salta, sono otto bit occupati
        per ogni bit:
            se il bit e' 0 e il suo numero e' <= max_bit  →  ritornalo
ritorna 0
```

**Ritorna.** Il numero del bit, oppure **`0` che significa «pieno»** — e lo zero
può fare da valore d'errore proprio perché il bit 0 è riservato e non è mai
allocabile. È lo stesso espediente di `files[i].inode == 0` nel VFS: il
marcatore è dentro il dato.

**Non deve** cercare oltre `max_bit`, che è `s_ninodes` per la imap e
`s_nzones - s_firstdatazone + 1` per la zmap. Senza il limite si trovano i bit
che `mkfs` ha lasciato a 1 fuori dal filesystem: siccome sono a 1 **non si
trovano affatto**, quindi il difetto non produce nessun sintomo finché il disco
non è pieno.

**Non deve** assumere che la bitmap stia in un blocco. Sull'immagine di prova ci
sta, quindi un codice sbagliato passa tutti i test. Il ciclo esterno sui blocchi
costa tre righe adesso e non si scrive mai più.

**Non deve** accendere il bit. Trovare e occupare sono due operazioni, e
tenerle separate è ciò che permette di scrivere il bit *prima* di usare la
risorsa — vedi la scheda 4.

**Chi la chiama.** `inode_alloca` e `zona_alloca`.

**Come si sbaglia.**

- **cominciare dal bit 0.** Si alloca il bit riservato, e da lì in poi inode 0 e
  zona `firstdatazone - 1` cominciano a esistere;
- **`byte & (1 << bit)` con `bit` che va da 7 a 0** invece che da 0 a 7. minix
  usa l'ordine naturale: il bit 1 è `byte[0] & 0x02`. Verificabile sul primo byte
  `ff`, che purtroppo è simmetrico — serve un'immagine con un buco per vederlo, e
  il test lo costruisce;
- **cercare `!= 0xFF` e poi non ricontrollare il singolo bit.** L'ottimizzazione
  è giusta, dimenticare il ciclo interno dopo no.

**Test.** Sull'host, su una copia: allocare finché non ritorna 0, e verificare
che il numero di allocazioni riuscite sia esattamente `s_ninodes` meno quelli già
in uso.

---

## 2. `bitmap_accendi`

```c
static int bitmap_accendi(uint32_t primo_blocco, uint32_t bit);
```

**Compito.** Leggere il blocco che contiene quel bit, accenderlo, riscriverlo.

```text
blocco = primo_blocco + bit / 8192        8192 bit in un blocco da 1024 byte
byte   = (bit % 8192) / 8
maschera = 1 << (bit % 8)
```

**Ritorna.** `0` o `-1`. Un fallimento qui è grave: significa che l'allocazione
non si può completare, e chi ha chiamato deve **rinunciare**, non proseguire.

**Non deve** essere «leggi, modifica, scrivi» su un blocco tenuto in cache fra
una chiamata e l'altra. In M11b la scrittura è sincrona: il blocco si rilegge
ogni volta. È lento — due accessi al disco per bit — e non lascia stato sporco da
scaricare, che è la ragione per cui la cache dei blocchi aspetta M12.

**Chi la chiama.** `inode_alloca` e `zona_alloca`, subito dopo
`bitmap_trova_libero`.

**Come si sbaglia.**

- **8192 scritto 1024.** I bit in un blocco sono 1024 **byte** per 8, ed è la
  divisione che si sbaglia guardando la costante sbagliata;
- **`|=` scritto `=`.** Il byte diventa la sola maschera, e sette bit vicini si
  spengono: sette inode in uso tornano liberi in un colpo. `fsck` li elenca tutti
  e sette, il che rende la diagnosi facile — ma solo se si arriva a lanciarlo;
- **scrivere il blocco sbagliato.** Con una bitmap da un blocco solo, `blocco +
  bit / 8192` vale sempre `blocco`, quindi l'errore non si vede.

**Test.** Accendere un bit, rileggere la bitmap dal disco e confrontare il byte
intero — non solo il bit. È il confronto che prende il `=` al posto di `|=`.

---

## 3. `inode_scrivi`

```c
static int inode_scrivi(struct inode *ino);
```

**Compito.** Rimandare sul disco un inode che sta in cache: legge il blocco della
tabella, sovrascrive i suoi 32 byte, riscrive il blocco.

È il gemello di `inode_carica`, con la stessa aritmetica — e per questo l'aritmetica
va **estratta** invece che scritta due volte:

```text
blocco = blocco_inodi + (ino - 1) / 32
offset = ((ino - 1) % 32) * 32
```

Deve ricostruire i 32 byte dai campi che minixfs tiene: `i_mode` dal `type`, `i_size`
dalla `size`, `i_zone` dall'array in `priv`. E i campi che il VFS non ha —
`i_uid`, `i_gid`, `i_time` — vanno **preservati** rileggendoli dal disco, non
azzerati: sono di qualcun altro.

**Ritorna.** `0` o `-1`.

**Non deve** scrivere un inode che non è in cache. La cache è la sola verità in
RAM; un inode non caricato non ha modifiche da salvare.

**Chi la chiama.** `inode_alloca`, `zona_assegna`, `minix_write` quando `i_size`
cambia, e `minix_create` per il genitore.

**Come si sbaglia.**

- **azzerare `i_uid` e `i_time`.** `fsck` non se ne lamenta, ma `ls -l`
  sull'host mostra file che cambiano proprietario quando il kernel li tocca —
  cioè un sintomo che sembra scollegato;
- **ricostruire `i_mode` dal solo tipo**, perdendo i bit dei permessi. Stesso
  genere: si preserva rileggendo e cambiando solo cosa serve;
- **dimenticare di chiamarla.** È il bug più frequente di M11b, perché tutto
  funziona finché il filesystem resta montato: le modifiche sono in cache. Al
  rimount spariscono.

**Test.** Modificare la `size` di un inode, scriverla, **rimontare**, rileggere.
Il rimount è la parte che conta: senza, si sta verificando la RAM.

---

## 4. `inode_alloca`

```c
static struct inode *inode_alloca(enum inode_type tipo);
```

**Compito.** Un inode nuovo, marcato in uso sulla bitmap e già scritto sul disco.

```text
1.  ino = bitmap_trova_libero(imap, ...)      se 0  →  ritorna 0, disco pieno
2.  bitmap_accendi(imap, ino)                 se fallisce  →  ritorna 0
3.  prendi uno slot di cache
4.  riempilo:  type = tipo,  size = 0,  tutte le nove zone a 0
    i_nlinks = 1 per un file, 2 per una directory
5.  inode_scrivi()
6.  ritorna il puntatore
```

**L'ordine dei passi 2 e 5 non è negoziabile: prima il bit, poi l'inode.** Al
contrario, un fallimento in mezzo lascia un inode scritto e marcato libero —
esattamente lo stato che `fsck` segnala come *«Inode 2 marked unused, but used
for file»*, e che `mount` accetta senza dire niente.

**`i_nlinks` parte da 2 per una directory** perché una directory nasce con `.`
che punta a se stessa, più la voce nel genitore. È il primo numero che `fsck`
controlla.

**Ritorna.** Il puntatore all'inode in cache, oppure `0`. Zero copre due casi
distinti — disco pieno e cache piena — e in M11b va bene che siano
indistinguibili: entrambi sono «non si può creare».

**Non deve** inserire niente in nessuna directory. Un inode allocato e non
ancora collegato è uno stato legittimo, ed è esattamente quello in cui si trova
fra il passo 6 e `dirent_inserisci`. Se qualcosa fallisce lì, resta un inode
orfano — che `fsck` segnala come *«Unattached inode»* e che in M11b **si accetta
come debito**, perché liberarlo vorrebbe dire la `free` che abbiamo tenuto fuori.

**Chi la chiama.** Solo `minix_create`.

**Come si sbaglia.** Dimenticando di azzerare le nove zone. Lo slot di cache
viene riusato, quindi contiene le zone del file di prima: il file nuovo nasce
lungo zero ma con zone che appartengono a qualcun altro, e alla prima scrittura
si sovrascrivono i dati di un altro file.

**Test.** Creare, rimontare, e verificare che il bit sia acceso rileggendo la
bitmap. Più il caso del disco pieno: allocare finché non fallisce, e verificare
che fallisca invece di restituire l'inode 0.

---

## 5. `zona_alloca`

```c
static uint32_t zona_alloca(void);
```

**Compito.** Una zona libera, marcata sulla bitmap e **azzerata**.

```text
bit = bitmap_trova_libero(zmap, ...)     se 0  →  ritorna 0
bitmap_accendi(zmap, bit)
zona = bit + s_firstdatazone - 1         ← l'inverso dell'indice della zmap
azzera il blocco e scrivilo
ritorna zona
```

**L'azzeramento non è cortesia.** Il blocco contiene quello che c'era prima: se
diventa un blocco di dati, il file ha in coda i resti di un file cancellato; se
diventa un blocco indiretto, quei resti vengono letti **come puntatori a zone**.
Il secondo caso è il peggiore, perché il danno si sposta su file che non
c'entrano.

**Ritorna.** Il numero di zona, oppure `0` per «pieno» — e lo zero funziona da
errore perché la zona 0 è il boot block, che non è mai allocabile.

**Non deve** restituire il numero del *bit*. Sono due spazi di numerazione
diversi, e la conversione è `bit + s_firstdatazone - 1`. Confonderli su questa
immagine sposta ogni zona di sei posizioni: si scrive sopra la tabella degli
inode.

**Chi la chiama.** `zona_assegna` e, per il blocco indiretto, se stessa
indirettamente.

**Come si sbaglia.** La conversione al contrario, `bit - firstdatazone + 1`, che
su `firstdatazone = 7` e bit piccoli dà numeri negativi — cioè, in `uint32_t`,
numeri enormi che il controllo di limite di `blocco_leggi` non ha.

**Test.** Allocare una zona, verificare che sia `>= s_firstdatazone`, e che il
blocco corrispondente sia tutto zeri.

---

## 6. `zona_assegna`

```c
static int zona_assegna(struct inode *ino, uint32_t n, uint32_t z);
```

**Compito.** Mettere la zona `z` come zona logica `n` del file. È l'inverso di
`zona_di`, e ne condivide la struttura:

```text
n < 7             →  zone[n] = z,  inode_scrivi
n < 7 + 512       →  se zone[7] == 0:  allocane una nuova per l'INDIRETTO
                     leggi il blocco indiretto, metti z in posizione n-7,
                     riscrivilo
oltre             →  -1, il doppio indiretto non e' supportato in scrittura
```

**Il caso interessante è l'indiretto che non esiste ancora.** La prima volta che
un file supera la settima zona serve un blocco in più — che si alloca con
`zona_alloca`, che lo azzera, e il cui numero va in `zone[7]` **e nell'inode sul
disco**. Due scritture, e saltare la seconda dà un file che funziona fino al
rimount.

**Il doppio indiretto si rifiuta**, e con un errore esplicito. Sull'immagine da
256 KB non ci si arriva nemmeno — servirebbero file oltre 519 KB — quindi
implementarlo sarebbe codice mai eseguito. La lettura lo gestisce già, ed è
giusto che sia asimmetrico: leggere un'immagine fatta da altri è un caso reale,
scriverne una così no.

**Ritorna.** `0` o `-1`.

**Non deve** allocare la zona `z`: la riceve. Chi alloca è `minix_write`, che sa
se serve davvero.

**Chi la chiama.** Solo `minix_write`.

**Come si sbaglia.** Scrivendo `z` nel blocco indiretto alla posizione `n`
invece che `n - 7`. Le prime sette zone finiscono duplicate nell'indiretto, e le
ultime sette del blocco cadono fuori.

**Test.** Far crescere un file oltre 7168 byte e verificare, rimontando, che
`zone[7]` non sia più zero e che il contenuto si rilegga tutto.

---

## 7. `minix_write`

```c
static int minix_write(struct inode *ino, uint32_t off, const void *buf,
                       uint32_t n);
```

**Compito.** Il gemello di `minix_read`, con **una** differenza: dove `read`
trova una zona a zero e restituisce zeri, `write` la alloca.

```text
finche' restano byte:
    zona logica    = off / 1024
    dentro la zona = off % 1024
    quanti         = min(1024 - dentro, rimasti)

    z = zona_di(ino, zona logica)
    se z == 0:
        z = zona_alloca()             se 0  →  esci con quello che hai fatto
        zona_assegna(ino, logica, z)

    leggi il blocco, copiaci dentro "quanti" byte, riscrivilo
    avanza

se off finale > ino->size:
    ino->size = off finale
    inode_scrivi(ino)
```

**Si legge il blocco prima di scriverlo**, anche quando si sovrascrive: la
granularità del disco è il settore, e i byte intorno a quelli che stai scrivendo
vanno preservati. L'unica eccezione sarebbe un blocco intero allineato, e non
vale la pena distinguerla.

**`i_size` si aggiorna solo se il file è CRESCIUTO.** Scrivere in mezzo a un file
non lo accorcia, e un `size` che diminuisce farebbe sparire la coda.

**Ritorna.** Quanti byte ha scritto davvero, o `-1`. Meno di `n` è un esito
legittimo — disco pieno — e chi chiama deve guardarlo.

**Non deve** avanzare nessuna posizione: `off` arriva esplicito, è la decisione
di interfaccia di M9a.

**Chi la chiama.** `vfs_write`, attraverso `ops->write`.

**Come si sbaglia.**

- **`i_size` non riscritta.** Il file c'è, il contenuto pure, e `cat` non stampa
  niente perché la size è zero. La diagnosi va a finire su `read`, che è
  innocente;
- **la zona allocata e non assegnata.** La `write` seguente ne alloca un'altra
  per lo stesso posto: la prima resta marcata occupata e non appartiene a
  nessuno, cioè una perdita che solo `fsck` vede;
- **uscire con `-1` a metà** quando il disco si riempie, invece di restituire
  quanti byte sono passati. Il file resta scritto a metà comunque, e dire di non
  aver scritto niente è una bugia.

**Test.** Scrivere in mezzo a un file esistente e verificare che i byte intorno
non cambino. Far crescere `hello.txt` da 26 a 3000 byte e rileggerlo. E scrivere
finché il disco non si riempie.

---

## 8. `dirent_inserisci`

```c
static int dirent_inserisci(struct inode *dir, const char *nome, uint32_t ino);
```

**Compito.** Aggiungere una voce da 16 byte a una directory.

```text
per ogni voce di dir:
    se ino == 0  →  RIUSA questo posto
se non c'e' posto:
    scrivi la voce in fondo, cioe' all'offset dir->size
    dir->size += 16
    inode_scrivi(dir)
```

**Riusare prima di crescere** non è ottimizzazione: senza `unlink` non ci sono
voci libere in mezzo, quindi il ramo del riuso in M11b non si esercita mai. Va
scritto lo stesso, perché il giorno che arriva `unlink` è la differenza fra una
directory che cresce all'infinito e una che no.

**Il nome si scrive in 14 byte riempiti di zeri**, e uno lungo esattamente 14
**non è terminato**. È la stessa asimmetria di `copia_nome` in lettura, e va
gestita nell'altro verso: si azzerano i 14 byte e poi si copia, senza il
terminatore.

**Ritorna.** `0` o `-1`. Fallisce se il nome è più lungo di 14, e **rifiuta
invece di troncare**: troncare farebbe collidere due file diversi, che è
l'errore del nome di dispositivo in M8.

**Non deve** controllare se il nome esiste già. È compito di `minix_create`, che
fa la `lookup` prima — e tenerlo separato significa che `dirent_inserisci` fa una
cosa sola.

**Chi la chiama.** `minix_create`, due volte quando crea una directory: una nel
genitore, e le voci `.` e `..` dentro la nuova.

**Come si sbaglia.** Facendo crescere la directory con `dir->size += 16` senza
`inode_scrivi`: al rimount la directory ha la size vecchia e l'ultima voce
sparisce. Il file esiste ancora, l'inode è allocato, ma nessuno lo nomina —
`fsck` lo chiama *«Unattached inode»*.

**Test.** Creare 60 file nella radice: sull'immagine di prova la radice ha 112
byte in una zona da 1024, quindi ci stanno 64 voci — **il ramo della crescita non
si esercita finché non ne crei 57.** Il test lo provoca apposta, e poi rimonta e
li conta.

---

## 9. `minix_create`

```c
static int minix_create(struct inode *dir, const char *name,
                        enum inode_type tipo, struct inode **out);
```

**Compito.** Mettere insieme le quattro operazioni precedenti.

```text
1.  se dir non e' una directory              →  -1
2.  se name esiste gia' (lookup)             →  -1
3.  ino = inode_alloca(tipo)                 →  se 0, -1
4.  dirent_inserisci(dir, name, ino->ino)
5.  se tipo == INODE_DIR:
        dirent_inserisci(ino, ".",  ino->ino)
        dirent_inserisci(ino, "..", dir->ino)
        dir->i_nlinks++  e  inode_scrivi(dir)
6.  *out = ino
```

**Il passo 5 è dove `fsck` ti aspetta.** Tre numeri devono tornare: la directory
nuova ha `i_nlinks == 2` (`.` più la voce nel genitore), il genitore ne guadagna
uno per via di `..`, e le due voci devono esserci davvero. `fsck.minix` li
controlla tutti e tre e li nomina uno per uno.

**Il passo 2 usa `lookup`**, cioè codice di M11a che funziona già. Riscrivere la
scansione qui sarebbe una seconda implementazione dello stesso confronto di nomi.

**Ritorna.** `0` con `*out` scritto, `-1` altrimenti, e su `-1` `*out` non si
tocca — la convenzione di `lookup`.

**Non deve** riuscire su un nome che esiste. Sta al chiamante decidere: `vfs_open`
con `O_CREAT` apre quello che c'è, `vfs_mkdir` fallisce.

**Non deve** funzionare sull'innesto. `dir` potrebbe essere l'inode di devfs
consegnato da `minixfs_graft`, e creare un file dentro `/dev` deve fallire. Il
controllo è che `dir->ops` sia `&ops_minix` — cioè che l'inode sia nostro.

**Chi la chiama.** `vfs_open` con `O_CREAT`, e `vfs_mkdir`, attraverso
`ops->create`.

**Come si sbaglia.** Dimenticando `dir->i_nlinks++` su `mkdir`. Il filesystem si
monta, `ls` funziona, e `fsck` dice *«Inode 1 has 3 links, counted 4»* — cioè un
messaggio che si capisce solo sapendo cos'è `..`.

**Test.** `mkdir`, rimontare, e verificare i tre numeri. Più `fsck.minix`, che è
l'unico che li controlla tutti insieme.

---

## 10. `vfs_open` con `O_CREAT`

```c
int vfs_open(const char *path, int flags);
```

**Compito.** Quello di prima, più: se `O_CREAT` è acceso e il path non esiste, si
crea l'**ultimo** componente.

```text
se vfs_resolve(path) riesce            →  come prima
se fallisce e O_CREAT non c'e'         →  -1, come prima
altrimenti:
    spezza il path in genitore + ultimo componente
    risolvi il GENITORE                →  se fallisce, -1
    se il genitore non ha ops->create  →  -1
    ops->create(genitore, ultimo, INODE_FILE, &ino)
    poi file_alloc e fd_alloc come sempre
```

**Solo l'ultimo componente.** `mkdir -p` non esiste, e un componente intermedio
mancante resta un errore: `open("/a/b/c", O_CREAT)` con `/a/b` inesistente
fallisce.

**La parte scomoda è ottenere il genitore**, che `vfs_resolve` non restituisce.
Due strade: copiare il path in un buffer e troncarlo all'ultima barra, oppure
aggiungere un parametro a `vfs_resolve`. La prima è più corta e non tocca una
funzione che funziona e ha 75 test addosso.

Attenzione al caso `/nome` — un file nella radice: dopo il troncamento il
genitore è la stringa vuota, che `vfs_resolve` rifiuta perché non comincia con
`/`. Il genitore giusto è `"/"`.

**Non deve** cambiare niente quando `O_CREAT` non c'è. È il controllo che dice
che M11b non ha rotto M11a: i 75 test di `test_vfs.c` devono passare invariati.

**Come si sbaglia.** Creando il file **prima** di verificare che ci sia posto
nelle tabelle: se `file_alloc` fallisce dopo, resta un file vuoto sul disco che
nessuno ha chiesto.

**Test.** Aprire con `O_CREAT` un path che esiste — deve aprire, non fallire.
Uno che non esiste — deve crearlo. Uno con il genitore inesistente — deve
fallire. E su devfs, dove `create` è nullo, deve fallire da sé.

---

## 11. `vfs_mkdir`

```c
int vfs_mkdir(const char *path);
```

**Compito.** La syscall 39 di Linux i386. È `vfs_open` con `O_CREAT` meno il
descrittore, più `INODE_DIR` al posto di `INODE_FILE`.

**Ritorna.** `0`, oppure `-1` se il path esiste già, se il genitore non esiste,
o se il filesystem non sa creare.

**Non deve** aprire niente e non deve restituire un descrittore: `mkdir` non
lascia niente di aperto. È la differenza che giustifica una funzione a parte
invece di un flag.

**Non deve** creare i componenti intermedi, come sopra.

**Chi la chiama.** `shell_mkdir`, e in M14 diventa la syscall 39 senza cambiare
firma.

**Come si sbaglia.** Condividendo troppo con `vfs_open`: la tentazione è una
funzione comune che prende il tipo. Va bene, ma allora la parte comune è
«risolvi il genitore e chiama create», **non** «apri»: se `mkdir` finisse per
allocare uno slot nella tabella dei file aperti, dopo 32 `mkdir` il sistema non
aprirebbe più niente.

**Test.** `mkdir` di un path nuovo, di uno esistente, e con il genitore
mancante. Più il controllo che la tabella dei file aperti sia rimasta vuota.

---

## 12. `shell_mkdir`

```c
static void shell_mkdir(int argc, char **argv);
```

**Compito.** Un argomento, una chiamata, un messaggio.

**Non deve** tacere in caso di successo. È un comando che modifica un disco, e la
conferma è ciò che distingue «fatto» da «fallito in silenzio» quando poi `fsck`
si lamenta.

**Test.** `tests/minixwrite.sh` lo digita.

---

## 13. `shell_write`

```c
static void shell_write(int argc, char **argv);
```

**Compito.** Creare o sovrascrivere un file con il testo dato.

```text
waltex> write /nuovo/ciao.txt salve dal kernel
  scritti 17 byte
```

**Gli spazi si rimettono a mano**, come in `shell_echo_cmd`: `shell_split` li ha
sostituiti con dei NUL, quindi `argv` contiene parole e non una riga. E si
aggiunge un `\n` finale, così `cat` sull'host mostra qualcosa di leggibile.

**Il buffer è locale**, e la riga di comando è lunga al massimo 128 byte, quindi
ne bastano altrettanti — non 512 come in `rdsect`.

**Non deve** avere una modalità «aggiungi». `O_APPEND` non esiste in M11b, e un
comando che sovrascrive va bene finché lo dice la riga di help.

**Chi la chiama.** `shell_exec`, e `tests/minixwrite.sh` attraverso
`sendkeys.py`: è il comando con cui il test provoca la condizione che misura,
come `spin` in `tasks.sh` e `wrsect` in M10.

**Come si sbaglia.** Ignorando il ritorno di `vfs_write` e dichiarando scritti i
byte che si volevano invece di quelli che sono passati. Con il disco pieno il
messaggio direbbe una cosa e `fsck` un'altra.

**Test.** `tests/minixwrite.sh`, che dopo aver digitato chiude la VM e verifica
con `fsck.minix` **e** con `mount` + `cat`.

---

## Riepilogo

| funzione | righe circa | chi | cosa dimostra |
|---|---|---|---|
| `bitmap_trova_libero` | 30 | **WALTER** | che il bit 0 è riservato, e perché lo zero può fare da errore |
| `bitmap_accendi` | 20 | **WALTER** | leggi-modifica-riscrivi, e il `\|=` |
| `inode_scrivi` | 35 | **WALTER** | che i campi di qualcun altro si preservano |
| `inode_alloca` | 35 | **WALTER** | l'ordine: prima il bit, poi il dato |
| `zona_alloca` | 25 | **WALTER** | perché un blocco appena allocato va azzerato |
| `zona_assegna` | 40 | **WALTER** | l'indiretto che nasce quando serve |
| `minix_write` | 55 | **WALTER** | che `i_size` va riscritta, e quando |
| `dirent_inserisci` | 40 | **WALTER** | che una directory è un file che cresce |
| `minix_create` | 45 | **WALTER** | i tre numeri di `i_nlinks` |
| `vfs_open` + `O_CREAT` | 30 | **WALTER** | come si ottiene il genitore di un path |
| `vfs_mkdir` | 20 | **WALTER** | perché non è un flag di `open` |
| `shell_mkdir` | 15 | **WALTER** | — |
| `shell_write` | 30 | **WALTER** | — |

Circa 420 righe, ed è la milestone più tua del blocco: nessuna funzione è mia.

E alla fine, la riga che chiude M11:

```text
$ fsck.minix -f build/minix.img
```

Zero. Cioè: un filesystem scritto da un kernel di tremila righe, e
un'implementazione che non è la nostra dice che è coerente.
