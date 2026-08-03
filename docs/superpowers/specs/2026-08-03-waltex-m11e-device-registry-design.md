# waltex — M11e: il registry dei dispositivi, e il polimorfismo che si sposta nel VFS

Data: 2026-08-03
Prerequisito: `2026-07-29-waltex-userland-design.md`, milestone M11a-M11d chiuse.

## Obiettivo

`ls /dev` non mostra `hda`, e la ragione non è una dimenticanza: è strutturale.

`struct device` di M8 porta `read(d, buf, n)` e `write(d, buf, n)`. Quindi nel
registro dei dispositivi «essere un dispositivo» e «avere una vista a byte» sono
la stessa proprietà. Un disco ha la prima e non la seconda — la sua granularità è
il settore, e una lettura corta su un disco è un guasto, non un esito normale —
quindi non può iscriversi, quindi non esiste come file.

M11e sposta il polimorfismo dal device layer al VFS, che è dove Linux lo tiene:

- il **registry** conosce identità e presenza — nome, tipo, `major`/`minor`, un
  puntatore opaco all'implementazione — e nessuna operazione di I/O;
- la **vista a byte** la fabbrica un adapter dentro le `inode_ops`, e per i
  dispositivi a blocchi è una traduzione byte → LBA con bounce buffer.

Il criterio di fine è che `ls /dev` elenchi `ttyS0` e `hda` accanto, che
`cat /dev/hda 600` attraversi il confine di settore e stampi i byte giusti, e che
`cat /dev/kbd` continui ad aspettare l'input — perché lì lo zero non è EOF.

**Non è una milestone del piano**, ed è dichiarato: il piano va da M11d a M12
(memoria). M11e è un refactor che non aggiunge concetti, chiude un difetto di M8
che M9b ha ereditato, e va prima di M12 per una ragione precisa — M12 tocca la
cache degli inode (lo sfratto, debito 1), e due cambiamenti insieme sulla stessa
superficie rendono ambiguo ogni guasto. È la stessa ragione per cui `unlink` è
stato tenuto fuori da M11b.

## Cosa la richiesta iniziale diceva di sbagliato

La proposta da cui questo spec nasce era scritta contro uno stato del repo
anteriore a M11a, e tre suoi punti sono da correggere. Stanno qui perché
riproporli sarebbe naturale.

1. **`vfs_mount("/dev", devfs_root())` ricostruirebbe un bug già pagato.** La
   radice di devfs ha una sola voce e si chiama `dev`: montare quella dà
   `/dev/dev/kbd`. Si monta `devfs_devdir()`, ed è `main.c:117` da M11c. Quattro
   self-check lo presero la prima volta.
2. **`devfs_root()` non è vestigiale e non si elimina.** È la radice di ripiego
   quando non c'è minix su hdb (`main.c:130`), e serve a tenere il kernel usabile
   con un motivo leggibile sulla seriale invece di una radice muta. Quindi devfs
   conserva **entrambi** gli accessori.
3. **«Il futuro filesystem di M10 potrà parlare con `struct blockdev` in LBA» è
   già vero.** `minixfs_init(ata_drive(1))` fa esattamente questo da M11a. Nel
   piano M10 è il driver ATA e M11 è minix, chiuse entrambe. Il criterio non è un
   obiettivo di M11e, è un invariante da non rompere — e il modo di romperlo
   sarebbe far passare minixfs dall'adapter.

E un quarto punto che non è un errore della proposta ma un vincolo
dell'ambiente: **`cat /dev/hda | hexdump` non può funzionare.** Le pipe non
esistono, perché `read()` su una pipe vuota *deve* bloccare e il blocking I/O è
fuori scope dichiarato. La prova a mano prende un'altra forma, ed è la sezione
«Verifica».

## Il bug che il refactor raccoglie

In `kernel/device.c`:

```c
int p = strpos(d->name, '\0');
if (p > DEV_NAME_MAX) {
    return 1;
}

size_t lname = strlen(d->name);
if (lname >= DEV_NAME_MAX || lname == 0) {
    return -1;
}
```

`strpos` ha un ramo esplicito `else if (a == '\0') r = -1;`
(`kernel/memory.c:63`), quindi cercando il terminatore ritorna **sempre** -1. La
guardia è codice morto: `-1 > 16` è falso. E se fosse raggiungibile ritornerebbe
`1`, che viola il contratto «0 se iscritto, -1 se rifiutato» dichiarato
nell'header — la classe di bug di M9b, dove tre valori di ritorno sbagliati
hanno prodotto tre guasti lontani dalla causa.

Ciò che protegge davvero è la `strlen` sotto, cioè **precisamente la scansione
illimitata contro cui `include/device.h` mette in guardia per venticinque
righe**: «una strlen normale su una stringa non terminata cammina fuori
dall'array».

I due test host passano, e vale la pena capire perché: passano perché `major` sta
all'offset `DEV_NAME_MAX`, subito dopo `name`, quindi la `strlen` si ferma dopo
uno o due byte fuori dall'array. La correttezza dipende dal layout della struct.
`dev_entry` mette `kind` in quella posizione, e `enum` è un `int` — quindi
l'accidente su cui la protezione si appoggia cambia proprio adesso.

**Cura:** una scansione limitata a `DEV_NAME_MAX` byte, e la `strlen` sparisce
insieme alla guardia morta.

## Scelte

### 1. Il registry non conosce l'I/O

**Scelta:** `struct dev_entry` ha nome, `kind`, `major`, `minor`, e un
`void *impl`. Nessun puntatore a funzione.

**Perché:** è l'intero refactor in una frase. Finché il registry conosce
`read`/`write` a byte, i due mondi non ci stanno dentro insieme, e l'unica
alternativa è che uno dei due mentisca. `include/blockdev.h` lo dice già da M10:
«su una tastiera *ho letto 3 byte su 64* è un esito normale; su un disco è un
guasto. Mettere i due sotto la stessa interfaccia costringerebbe uno dei due a
mentire».

**Alternativa scartata:** un `union` dentro `struct device` con i due insiemi di
operazioni. Funziona, ed è peggio per due ragioni: chi legge la struct deve
sapere quale ramo è valido prima di poterla capire, e il registry resta il posto
dove si aggiunge il terzo mondo il giorno che arriva — cioè non ha smesso di
essere il punto di accumulo.

**Conseguenza sul `const`:** `dev_get` può ritornare `const struct dev_entry *`,
cosa che `device_find` non poteva. Oggi il puntatore deve essere mutabile perché
chi lo riceve chiama `d->write(d, ...)`; ora l'oggetto mutabile è `impl`, che la
voce riferisce e non possiede.

### 2. L'adapter vive in un file suo

**Scelta:** `kernel/devio.c` tiene le due vtable, i due wrapper di
registrazione, i due lookup tipizzati, e le due funzioni che rispondono a
domande per `kind`. `kernel/devfs.c` resta il puro mappatore registry → albero.

**Perché:** l'adapter è l'unica **logica** del refactor — clamping, bounce
buffer, letture a cavallo, trasferimento parziale, read-modify-write. Tutto il
resto è mappatura e rename. In un file suo, l'aritmetica si prova sull'host con
un `struct blockdev` finto **senza costruire un albero devfs**, che è la stessa
leva che ha reso M11a la verifica più forte del progetto: due puntatori a
funzione, e sotto un file invece di un disco.

**Alternativa scartata:** tutto in `devfs.c`, come diceva la proposta. Porta
`devfs.c` da 177 a ~300 righe e mescola due nature, e per provare il clamping
bisogna passare da un albero montato — cioè il FAIL non dice quale dei due è
rotto. È lo stesso argomento per cui `vfs_resolve` sta nell'header invece di
essere provata attraverso `vfs_open`.

### 3. Lo `switch` su `enum dev_kind` esiste una volta sola

**Scelta:** `devfs.c` non include `chardev.h` né `blockdev.h`. Dove gli
servirebbe sapere cosa sia un disco, chiede a `devio`:

```c
int devio_fill_inode(const struct dev_entry *e, struct inode *in);
int devio_caps(const struct dev_entry *e);   /* maschera a due bit */
```

**Perché:** la proposta faceva fare a devfs `in->size = b->nsectors *
SECTOR_SIZE`, che obbliga devfs a conoscere `struct blockdev` per un campo. Con
`devio_fill_inode` devfs riceve un inode già riempito — `type`, `ops`, `priv`,
`size`, `major`, `minor` — e il ramo per `kind` sta nell'unico file che è **il**
ponte fra i mondi. Il secondo cliente della stessa regola è `devio_caps`, che
serve alla colonna `r-`/`-w` di `devs`: senza di lui `shell.c` dovrebbe includere
entrambi gli header per guardare due puntatori a funzione.

**Conseguenza:** le due vtable restano `static` dentro `devio.c`. Non servono
`extern`, perché l'unico che le assegna è `devio_fill_inode`.

### 4. Un settore per volta, sempre

**Scelta:** l'adapter non ha una via rapida per le letture allineate a settore.
`b->read` viene chiamata **in un punto solo**, sempre con `count == 1`, e ogni
byte passa dal bounce buffer.

**Perché:** è la risposta al requisito «lascia l'adapter strutturato in modo che
la cache si possa infilare al posto del bounce buffer». Con una via rapida che
legge direttamente nel buffer del chiamante ci sarebbero **due** punti da
sostituire quando la cache arriva; così c'è una riga, e diventa
`cache_get(b, lba)`. La via rapida inoltre non si eserciterebbe: `cat` legge a
blocchi di 64 byte, quindi sarebbe codice mai eseguito — la stessa ragione per cui
il doppio indiretto si rifiuta in scrittura in M11b.

### 5. Il bounce buffer è locale

**Scelta:** 512 byte sullo stack, non `static`.

**Perché:** è la lezione di procfs. Statico costerebbe 512 byte una volta sola,
ma fra il «leggo il settore» e il «copio la fetta» ci sta un tick del timer, e
due letture prelazionate si mescolerebbero. Sullo stack non è condiviso con
nessuno e non serve nessuna sezione critica.

**Il costo, dichiarato:** gli stack dei task sono 4096 byte, quindi il buffer è
**un ottavo dello stack**, dentro la catena `shell_cat → vfs_read → blk_read`. È
il candidato numero uno a spostarsi quando M12 porta `kmalloc`, e insieme alla
riga della cache è il motivo per cui questo file va riletto in M12.

### 6. `dev_by_id` resta, e diventa l'implementazione di un rifiuto

**Scelta:** `dev_by_id(major, minor)` sopravvive al refactor, e
`dev_register` rifiuta una coppia `(major, minor)` duplicata chiamandola.

**Perché:** l'header di M8 prometteva che il primo chiamante sarebbe stato il VFS
di M9, e non è successo in quattro milestone — di solito è il momento di tagliare.
Qui no: il nuovo rifiuto ha bisogno esattamente di quella scansione, e tenerne
una sola evita due implementazioni della stessa domanda. I due si giustificano a
vicenda — la ricerca per coppia è ben definita *perché* la coppia è unica per
costruzione, e la coppia è unica *perché* qualcuno cerca per coppia.

Il chiamante promesso arriverà con un nodo di dispositivo su minix, che memorizza
`major`/`minor` e non il nome. Più tardi del previsto, ma è quello.

## Interfacce

### `include/dev.h` — il registry

```c
#define DEV_MAX      16
#define DEV_NAME_MAX 16          /* NUL compreso: 15 caratteri utili */

enum dev_kind { DEV_NONE = 0, DEV_CHAR, DEV_BLOCK };

struct dev_entry {
    char          name[DEV_NAME_MAX];
    enum dev_kind kind;
    uint16_t      major, minor;
    void         *impl;          /* struct chardev * | struct blockdev * */
};

void dev_init(void);
int  dev_register(const struct dev_entry *e);
int  dev_lookup_index(const char *name);                 /* l'indice, o -1 */
const struct dev_entry *dev_get(int i);                  /* la voce i, o 0 */
const struct dev_entry *dev_by_id(uint16_t major, uint16_t minor);
int  dev_count(void);
```

`DEV_NONE = 0` e non `DEV_CHAR = 0`: è la convenzione di `INODE_NONE` e di
«nessun inode vale zero» di procfs. Uno slot mai riempito non deve poter passare
per un chardev valido.

`dev_lookup_index` ritorna un **indice** e non un puntatore, e serve così: devfs
usa quell'indice per scegliere lo slot dell'inode nel pool.

`BLK_NAME_MAX` sparisce da `blockdev.h`: con un registry unico il limite del nome
è uno, ed è `DEV_NAME_MAX`. Due costanti con lo stesso valore sono due verità che
possono divergere.

**I rifiuti di `dev_register`,** e in tutti si rifiuta invece di aggiustare —
troncare un nome troppo lungo trasformerebbe un errore del driver in un bug di
ricerca che si manifesta altrove:

| rifiuto | nota |
|---|---|
| registry pieno | |
| `kind` non è `DEV_CHAR` né `DEV_BLOCK` | nuovo: `DEV_NONE` e i valori fuori range |
| nome non terminato entro `DEV_NAME_MAX` byte | scansione **limitata**: è la cura del bug |
| nome vuoto | `dev_lookup_index("")` non ha significato, e in `/dev` sarebbe un file senza nome |
| nome duplicato | la ricerca diventerebbe ambigua e vincerebbe il primo in silenzio |
| coppia `(major, minor)` duplicata | nuovo, via `dev_by_id` |

Verificato che i cinque device attuali non collidano: console 5:1, ttyS0 4:64,
kbd 13:64, hda 3:0, hdb 3:64.

Se ne va «`read` e `write` entrambi nulli», perché il registry non conosce i
metodi — **e perderlo è un guadagno**, perché nei due wrapper diventa asimmetrico
come deve essere:

| caso | `chardev_register` | `blockdev_register` |
|---|---|---|
| `read == 0` | lecito: console non si legge | **rifiutato**: un disco da cui non si legge non ha senso |
| `write == 0` | lecito: kbd non si scrive | lecito: è un disco read-only |
| entrambi nulli | rifiutato | rifiutato |

Un controllo condiviso non potrebbe esprimerlo: distingue solo «almeno uno».

### `include/chardev.h` — ex `device.h`

`struct device` → `struct chardev`, `device_register` → `chardev_register`,
`MAX_DEVICES` → `DEV_MAX` (in `dev.h`). La struct perde `name`, `major` e
`minor`, che ora vivono nella voce del registry: le resta la coppia di
operazioni e `priv`.

`device.h` si **elimina**, non si conserva come header di compatibilità che
includa `chardev.h`. Dieci file da aggiornare è meccanico; uno shim è una seconda
verità.

Delle tre convenzioni di M8, due restano parola per parola — «un puntatore a
operazione nullo significa non supportata, non errore», e «`read` ritorna quanti
byte ha copiato davvero, e zero significa adesso non c'è niente, NON fine del
file».

La terza si **spezza in due**, ed è la sezione che segue.

### La trappola numero uno: `impl` è un puntatore, e il registro COPIAVA

Questa è la cosa più pericolosa di tutto M11e, e non si vede provando.

Oggi `device_register` copia l'intera struct, quindi i driver la riempiono
**sullo stack**. `serial.c:53` lo dice esplicitamente:

> La struct è LOCALE, e non è una distrazione: `device_register` copia, quindi
> questa memoria può sparire appena `serial_init` ritorna. **Se il registro
> conservasse il puntatore invece di copiare, il guasto si manifesterebbe
> esattamente qui** — ed è il caso che `test_device` costruisce modificando la
> sorgente dopo l'iscrizione.

Il refactor fa esattamente quello che quel commento descrive come guasto. La
convenzione di M8 si divide:

| cosa | prima | dopo |
|---|---|---|
| `name` | copiato nell'array della struct | copiato nell'array di `dev_entry`. **Invariato** |
| operazioni, `priv` | copiate | **riferite** attraverso `impl` |

Quindi le `struct chardev` dei tre driver devono diventare **`static`**, e i tre
commenti che spiegano perché sono locali vanno riscritti dicendo l'opposto. Vale
per `serial.c`, `vga.c` e `keyboard.c`. `ata.c` è già a posto: tiene un array
`static` di due `struct blockdev`, perché `ata_drive(i)` ne ritorna i puntatori.

**Perché è la trappola numero uno:** una struct locale non azzerata dopo il
ritorno resta leggibile per un po', quindi in QEMU il kernel booterebbe, `ls
/dev` funzionerebbe, e `assert(chardev_register(...) == 0)` sarebbe verde. Il
guasto arriva al primo `read` dopo che quello stack è stato riusato — cioè un
salto a un indirizzo arbitrario, lontano dalla causa. È la stessa specie del
frame falsificato di `task_create` e della zona non azzerata di M11b: memoria che
ha l'aria di essere giusta.

**Il test che lo prende esiste già e va ROVESCIATO.** `test_device.c` modifica la
sorgente dopo l'iscrizione per dimostrare che il registro ha copiato. Diventa due
test con due assunti opposti, ed è la forma migliore di documentazione del nuovo
contratto:

- modificare il `name` della sorgente dopo l'iscrizione **non** si vede nel
  registry (l'array copia, invariato);
- modificare le operazioni della `struct chardev` dopo l'iscrizione **si** vede
  (il puntatore riferisce, nuovo).

### `include/blockdev.h`

Invariata nella struct — è il punto: `minixfs` continua a parlarle in LBA senza
sapere che esista un adapter. Perde `BLK_NAME_MAX`.

### `include/devio.h` — il ponte

```c
#define DEVIO_CAN_READ  1
#define DEVIO_CAN_WRITE 2

int chardev_register (const char *name, uint16_t major, uint16_t minor,
                      struct chardev  *c);
int blockdev_register(const char *name, uint16_t major, uint16_t minor,
                      struct blockdev *b);

struct chardev  *dev_chardev (const char *name);   /* 0 se assente o è a blocchi */
struct blockdev *dev_blockdev(const char *name);   /* 0 se assente o è a caratteri */

int devio_fill_inode(const struct dev_entry *e, struct inode *in);
int devio_caps(const struct dev_entry *e);
```

I due lookup tipizzati controllano `kind` **prima** di castare `impl`: un cast da
`void *` che il chiamante non può verificare è il punto dove un errore di
registrazione diventa un salto in un indirizzo arbitrario.

`devio_fill_inode` ritorna 0 se ha riempito, -1 se `e` è nullo o se `kind` non è
uno dei due — e su -1 **non tocca `*in`**, che è la convenzione di `lookup` e di
`create`. Non scrive `ino`: quello è affare di devfs, e il perché sta nella
sezione sull'albero.

`devio_caps` ritorna una maschera di `DEVIO_CAN_READ`/`DEVIO_CAN_WRITE`, e zero
per una voce invalida. Zero è anche il valore di un dispositivo che non sa fare
niente, che i due wrapper rifiutano — quindi in pratica non si presenta, e non
serve distinguerlo.

### `include/vfs.h`

`enum inode_type` guadagna `INODE_BLOCKDEV`. È l'unica modifica al VFS, e
`kernel/vfs.c` **non cambia di una riga** — verificato che non includa già né
`device.h` né altro del device layer: include `vfs.h`, `task.h`, `memory.h`,
`irq.h`. È la stessa misura binaria che ha dato senso a M11d:
`git diff --stat kernel/vfs.c` dev'essere vuoto.

Il commento su `major`/`minor` va aggiornato: dice «validi se `INODE_CHARDEV`», e
diventano validi per due tipi. **Non tocca** la nota su `st_dev`, che resta un
problema diverso e resta di M14.

## L'albero: `kernel/devfs.c`

Pool `struct inode dev_inodes[DEV_MAX]`, indicizzato 1:1 col registry. Radice 1,
`/dev` 2, dispositivo *i* → `3 + i`.

Due `open` sullo stesso dispositivo devono dare lo **stesso** puntatore a inode —
è la lezione di M11a («`lookup` restituisce un puntatore che deve sopravvivere
alla chiamata») e la ragione per cui il pool non può essere una locale.

**`ino == 0` è il marcatore di «slot non inizializzato»**, quindi non serve un
flag per slot: è il ragionamento di `struct file` senza flag di occupazione, del
ring buffer senza contatore, e del registry senza campo di capacità.

```c
static int devfs_lookup(struct inode *dir, const char *name, struct inode **out)
{
    int i = dev_lookup_index(name);
    if (i < 0) return -1;

    if (dev_inodes[i].ino == 0) {
        devio_fill_inode(dev_get(i), &dev_inodes[i]);
        dev_inodes[i].ino = 3 + i;      /* ULTIMA. Vedi sotto. */
    }

    *out = &dev_inodes[i];
    return 0;
}
```

**`ino` si scrive per ultimo, e non è estetica.** Due task prelazionati cento
volte al secondo possono entrare qui sullo stesso slot:

- con `ino` per ultimo la corsa è **benigna**: scrivono valori identici, e chi
  arriva secondo vede ancora `ino == 0`, rifà il lavoro, e ottiene lo stesso
  risultato;
- con `ino` **prima**, il secondo vede `ino != 0` e riceve un inode mezzo
  riempito, con `ops` ancora nullo — cioè un salto attraverso un puntatore nullo
  al primo `read`.

Nessuna sezione critica serve, e va detto **perché nessuno la aggiunga** credendo
di sistemare qualcosa: è la disciplina del ring buffer di M5, dove la struttura
sostituisce il `cli`.

`devfs_readdir` legge il registry vivo, `idx >= dev_count()` → 0. `devfs_init`
riempie solo radice e `/dev`, e **non tocca più il registry**.

**Il guadagno del lazy init non è quello che sembra.** Un driver che si iscrive
*prima* di `devfs_init` funziona già oggi: `serial_init()` è a `main.c:39` e
`devfs_init()` a `main.c:88`. Ciò che oggi si perde in **silenzio** è l'opposto —
un driver che si iscrive **dopo** `devfs_init` non compare in `/dev`, e non c'è
nessun errore da nessuna parte. Il lazy init lo rende impossibile.

Conseguenza da non dimenticare: il commento di `main.c:80-87` — «`devfs_init`
LEGGE il registro dei dispositivi, quindi va dopo tutte le `*_init()` dei
driver» — **diventa falso e va riscritto**. Resta vero il vincolo opposto,
`dev_init()` prima di tutti i driver, perché sono loro a iscriversi.

## L'adapter: `kernel/devio.c`

### `chr_inode_ops`

Passthrough verso `chardev->read`/`write`, e **ignora `off`**: un dispositivo a
caratteri non ha una posizione, quindi una `lseek` su `/dev/kbd` non significa
niente e va ignorata, non rifiutata.

**«Non supportata» ritorna -1, non 0** — e questa riga vale i tre bug di M9b da
sola. La convenzione «puntatore nullo = non supportata, non errore» descrive il
**dispositivo**; il valore consegnato a chi ha chiesto di leggere deve essere -1,
perché uno zero significherebbe «adesso niente» su un chardev e «EOF» su un
file. Sono entrambe bugie. È esattamente il `return 1` di `chardev_read` che
faceva avanzare l'offset a `cat` su un buffer che nessuno aveva riempito.

### `blk_inode_ops`, in lettura

```text
1.  off >= in->size          → 0            EOF VERO, e qui lo zero è onesto
2.  n > in->size - off       → n = in->size - off       clamp
      NON «off + n > in->size»: quella somma GIRA.
      Si SOTTRAE, ed è lecito perché il punto 1 garantisce off < in->size.
      È la regola di M10: «lba + count > nsectors è il controllo SBAGLIATO».
3.  b->read == 0             → -1
4.  finché restano byte:
      lba   = off / SECTOR_SIZE
      skip  = off % SECTOR_SIZE
      chunk = min(SECTOR_SIZE - skip, restanti)
      b->read(b, lba, bounce, 1)        ← L'UNICA chiamata, e sempre count 1
      memcpy(dst, bounce + skip, chunk)
5.  errore con byte già copiati → ritorna i byte copiati
    errore al primo settore     → -1
```

Il punto 5 è la convenzione di `read` portata dentro: chi ha ricevuto 300 byte
buoni deve saperlo, e dirgli -1 glieli fa buttare. Chi non ne ha ricevuto nessuno
non ha nulla da salvare, e uno zero gli direbbe EOF.

### `blk_inode_ops`, in scrittura

Simmetrica, con una trappola in più: **una scrittura parziale è
read-modify-write.** Scrivere 10 byte all'offset 5 vuole leggere il settore,
ritoccarne 10 byte, e riscriverlo. Saltare la lettura distrugge i 502 byte
intorno — ed è la stessa specie di guasto della zona non azzerata di M11b, dove i
resti di qualcun altro hanno l'aria di essere giusti perché sono dati veri.

Un disco non cresce: `off >= size` → 0, e il clamp è quello della lettura.
`write == 0` → -1, e la registrazione con `write` nullo resta legittima: è un
disco read-only, non un driver incompleto.

### Un limite dichiarato

`size = nsectors * SECTOR_SIZE` gira in `uint32_t` a 4 GiB, cioè 8388608
settori, e LBA28 arriva a 128 GiB — quindi è raggiungibile in principio. Sui
nostri dischi da 2048 e 512 settori non si vede. **Non si sistema in M11e:**
vorrebbe una dimensione a 64 bit in `struct inode`, che è un problema di
`struct stat` in M14. Va annotato nell'elenco dei debiti.

## `kernel/shell.c`

- **`shell_cat` non si tocca nella logica**, e sta qui perché è la prova che il
  taglio è nel posto giusto. La condizione d'uscita è
  `dispositivo = (ino->type == INODE_CHARDEV)`: un `INODE_BLOCKDEV` prende il
  ramo «file», cioè si ferma sullo zero. Il criterio «`cat /dev/hda` termina
  all'EOF» si soddisfa **non** modificando `cat`.
- **`cat` guadagna un limite opzionale**: `cat <path> [n]`. Senza `n` il
  comportamento non cambia di una virgola, quindi i test esistenti coprono già
  quel ramo. Con `n` si fermano i byte a `n`, e serve a due cose: rende possibile
  la prova a mano su un disco da 1 MB, e impedisce di inondare la seriale, che è
  il log che i test leggono con `grep`.
- **`shell_ls` guadagna il terzo ramo**: oggi ha `if (type == INODE_CHARDEV) …
  else "file %d byte"`, quindi un blockdev si annuncerebbe come file. Diventa
  `blockdev 3:0  1048576 byte`.
- **`shell_devs`** itera il registry e stampa `kind`, `major:minor`, e la colonna
  `r-`/`-w` via `devio_caps`. La colonna guadagna significato: `r-` su un disco
  ora vuol dire «read-only», stato legittimo che prima non era esprimibile.
- **`shell_lsblk`** filtra `kind == DEV_BLOCK` sul registry. Spariscono
  `ata_drive_count()` e la lista parallela.
- **`disco_per_nome` diventa `dev_blockdev(name)`** e si elimina, e `disco_da_argv`
  la chiama. Vale anche per `rdsect` e `wrsect`: se solo `lsblk` passasse al
  registry resterebbero due modi di dire quali dischi esistono, che è «una
  seconda verità che può divergere dalla prima» — le parole di `device.h:104`. La
  divergenza qui sarebbe che `wrsect` scrive sul disco sbagliato.

`ata_drive()` sopravvive come accessore interno del driver, ma fuori da `ata.c`
non lo chiama più nessuno.

## `kernel/main.c`

- `device_init()` → `dev_init()`, sempre prima di tutti i driver;
- `minixfs_init(ata_drive(1))` → `minixfs_init(dev_blockdev("hdb"))`. Per nome e
  non per indice, per la ragione già scritta in `shell.c` a proposito di
  `rdsect`: «`rdsect hdb 7` si legge, `rdsect 1 7` bisogna ricordarselo». E
  perché montare per nome è la forma verso cui il blocco va;
- il commento sull'ordine di `devfs_init` va riscritto (vedi sopra);
- `kprintf("waltex: /dev con %d dispositivi", dev_count())` passa da 3 a 5.

## Verifica

### Test host

| file | cosa |
|---|---|
| `test_dev.c` | rimpiazza `test_device.c`. Duplicati per nome **e** per `(major, minor)`; nome non terminato **nei due casi di layout**, riscritti perché `dev_entry` ha `kind` dopo `name`; registry pieno; `kind` invalido; `dev_by_id`; `dev_get` fuori intervallo |
| `test_devio.c` | **il valore della milestone.** `struct blockdev` finto su un file con `fread`/`fseek`, come `test_minixfs.c`: offset non allineato, a cavallo di due settori, di tre, clamp all'ultimo byte, `off == size`, `off > size`, `n == 0`, `read == 0` → -1, errore al secondo settore → i byte copiati, errore al primo → -1, write parziale che preserva i byte intorno, `devio_caps` sui quattro casi |
| `test_devfs.c` | due `lookup` danno lo **stesso** puntatore; `readdir` enumera; un device iscritto **dopo** `devfs_init` compare; nessun `ino` a zero |

`test_devio.c` funziona per la ragione che ha reso M11a la verifica più forte del
progetto: `struct blockdev` ha due puntatori a funzione, e sull'host diventano
`fread`/`fseek` su un file. È la quarta volta che questa leva paga — il sink di
`kprintf`, l'albero finto di `test_vfs.c`, il disco finto di `test_minixfs.c`.

### Self-check in QEMU

Il controllo che dà senso alla milestone: **i byte di `hda` letti attraverso il
VFS coincidono con gli stessi letti in LBA da `ata_drive(0)`.** Due strade allo
stesso dato — è la disciplina dell'orologio CMOS di M4 e della verifica
bidirezionale di M10, applicata all'adapter. Non può passare se l'aritmetica è
sbagliata, e non dipende da cosa c'è sul disco.

L'intervallo si sceglie apposta perché **attraversi il confine di settore**: `lseek(500)`
più `read(100)` prende gli ultimi 12 byte del settore 0 e i primi 88 del settore
1, cioè esercita in un colpo l'offset non allineato, la doppia iterazione e lo
`skip` che torna a zero al secondo giro. È il caso difficile, e va confrontato
con i due settori letti in LBA e ricuciti a mano.

Più: `/dev/hda` esiste e ha `size == nsectors * 512`; `lseek(size - 10)` seguito
da `read(100)` dà 10 e poi 0.

E il controllo di ordine, che è il migliore che si possa fare sul lazy init: i
self-check girano **prima** di `task_init`, ma **dopo** i driver — quindi in
quell'istante `/dev` deve essere piena, mentre `devfs_init` non ha letto niente.
Un `devfs_init` che si fosse memorizzato il registry passerebbe questo e
cadrebbe sul test host che iscrive un device dopo.

### Script

`tests/shell.sh` guadagna due righe e **non perde nessuna delle esistenti**:

- `ls /dev` deve trovare `hda` accanto ai tre di prima;
- `cat /dev/hda 15` deve stampare esattamente `waltex-disk-v1`.

Quella firma la scrive `tools/mkdisk.sh` nel settore 0, e usarla come bersaglio
di `grep` chiude il cerchio di M10: il riferimento lo genera un programma che non
è il kernel, e il kernel lo ritrova attraverso una strada **nuova** — `open`,
`read`, EOF — invece di `rdsect`. Un disco non può verificare se stesso.

**La prima versione di questo spec sbagliava il numero**, e vale la pena tenerlo
scritto: diceva `cat /dev/hda 600`, perché 600 attraversa il confine di settore.
Attraversarlo lo fa, ma stampa 15 byte di firma, poi **497 NUL** — il settore 0 è
azzerato dopo la firma — e poi 88 byte di pattern binario. Illeggibile a schermo
e inutile come bersaglio di `grep`. Il confine lo prova il self-check
bidirezionale, che è il posto dove un confronto byte a byte si può fare davvero.

Il conteggio dei test si **misura** a fine milestone, non si stima:
`make -C tests/host -s run | grep -cE "ok +--"` e la stessa cosa sul log seriale.
`check_device_count` passa da 3 a 5.

## Ordine di lavoro

Quattro passi, e ognuno compila e passa i test da solo. È la richiesta di Walter
ed è anche la disciplina delle milestone applicata dentro una milestone: quando
qualcosa si rompe, la superficie di sospetto è di poche decine di righe.

1. **Il rename, e niente altro.** `device.h` → `chardev.h`, `struct device` →
   `struct chardev`, `device_register` → `chardev_register`, i tre driver,
   `selftest.c`, `test_device.c` → `test_chardev.c`. Nessun cambiamento di
   comportamento: **tutti i test passano invariati nel numero**, ed è il criterio
   di fine del passo.
2. **Il registry.** `dev.c`/`dev.h`, i due wrapper in `devio.c`, i lookup
   tipizzati. `ata_init` si iscrive. `devs` e `lsblk` passano al registry, e
   `ls /dev` mostra `hda` — che a questo punto è un inode senza `ops`, quindi
   `cat` su di lui deve fallire in modo pulito, non saltare in un puntatore
   nullo.

   **È il passo in cui le `struct chardev` dei tre driver diventano `static`**, e
   va fatto nello STESSO commit in cui `impl` diventa un puntatore: separarli
   lascia un commit che boota, passa i test, e ha tre puntatori penzolanti. Il
   test rovesciato sulle operazioni è il criterio che dice che è stato fatto.

   Criterio: `test_dev.c` verde, `ls /dev` con cinque voci.
3. **devfs.** Lazy init, `devio_fill_inode` limitata al ramo chardev,
   `ino` per ultimo. Criterio: `test_devfs.c` verde, i self-check di M9b verdi
   rinominati.
4. **L'adapter a blocchi.** `blk_inode_ops`, `INODE_BLOCKDEV`, il ramo blockdev
   di `devio_fill_inode`, `shell_ls`, `cat [n]`. Criterio: `test_devio.c` verde,
   il self-check bidirezionale verde, `cat /dev/hda 15` che stampa la firma.

## Fuori scope

Non sono cose fatte male, sono cose non fatte — e stanno qui perché la domanda
«perché manca?» venga risposta una volta.

- **La buffer cache.** L'adapter è strutturato perché ci si infili — una riga,
  `b->read(b, lba, bounce, 1)` — e non c'è. Senza `kmalloc` sarebbe un array
  statico di settori, cioè una decisione di dimensionamento presa nel posto
  sbagliato: la si prende in M12, quando c'è un allocatore.
- **Il blocking I/O.** Invariato: `cat /dev/kbd` fa ancora spin sullo zero, ed è
  la ragione per cui quello zero non può significare EOF.
- **`mknod`, e i nodi di dispositivo persistenti su minix.** `major`/`minor`
  restano metadati che nessun lookup usa. Il registry li conserva perché il
  giorno che un inode minix dirà «sono il dispositivo 3:0», `dev_by_id` è la
  funzione che risponde.
- **La dimensione a 64 bit** per i dischi sopra 4 GiB. Vedi il limite dichiarato
  sopra: è M14.
- **`umount`**, che vuole il refcount sull'inode. Invariato.

## Debiti che questa milestone tocca

**Ne chiude uno che non era nell'elenco:** la protezione del nome non terminato
in `device_register`, che era codice morto più una `strlen` illimitata.

**Ne apre uno:** `size` a 32 bit sui dischi sopra 4 GiB. Morde in M14, con
`struct stat`.

**Non tocca** i debiti 1 e 2 — il puntatore in `mounts[].punto` e il refcount
mancante — e vale la pena dirlo, perché il pool `dev_inodes[]` è statico e non
sfratta, quindi il rischio dello sfratto resta interamente dentro la cache di
minixfs, dove è annotato.

**Il debito 5** (`put_uint` tratta la base 10 come con segno) si ripresenta:
`in->size` di un disco da 1 MB è 1048576, che sta in un `int`, ma i cast a `int`
in `lsblk` e in `shell_ls` restano necessari e vanno mantenuti con il commento
che dice perché.
