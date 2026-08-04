# M11e — registry dei dispositivi e adapter a blocchi: piano di implementazione

> **Per chi esegue:** questo piano NON si dà a un subagente. Vedi «Chi scrive
> cosa» qui sotto: i `.c` concettuali sono di Walter, e il piano gli fornisce
> contratti, test-bersaglio e trappole invece dell'implementazione. Gli step
> hanno le caselle (`- [ ]`) per il tracciamento.

**Goal:** spostare il polimorfismo dal device layer al VFS, così che un disco
possa esistere in `/dev` e si legga a byte, senza che `struct blockdev` smetta di
parlare in LBA a minixfs.

**Architettura:** un registry agnostico (`dev.c`) che conosce nome, `kind`,
`major`/`minor` e un `void *impl`; un ponte (`devio.c`) che è l'unico posto dove
`enum dev_kind` si apre in uno `switch`, e che tiene le due vtable `inode_ops`;
un `devfs.c` che è solo albero e inizializza gli inode in modo lazy.

**Tech stack:** C freestanding `-m32 -ffreestanding`, GNU as, QEMU i386. Nessuna
libc. Test host con `gcc -m32 -DWALTEX_HOSTED`, self-check dentro la VM, script
bash che pilotano il monitor di QEMU.

**Spec:** `docs/superpowers/specs/2026-08-03-waltex-m11e-device-registry-design.md`

## Global Constraints

- **Nessuna allocazione dinamica.** Array statici a capacità fissa: `DEV_MAX 16`,
  `DEV_NAME_MAX 16`. L'allocatore arriva in M12.
- **Non esiste la libc.** Niente `string.h`/`stdio.h` nel kernel. Le funzioni di
  stringa stanno in `kernel/memory.c`; se serve qualcosa che non c'è, si scrive.
- **Errori come ritorno negativo**, mai `-1` generico dove esiste un valore
  Linux. In `inode_ops` la convenzione è `-1`, e resta.
- **Un puntatore a operazione NULLO significa «non supportata», non «errore».**
  Ma il valore consegnato a chi ha chiesto di leggere è `-1`, non `0`.
- **`assert()` è sempre attiva** e chiama `panic()`. Non introdurre `NDEBUG`.
- **`kprintf` scrive su VGA e su COM1.** La seriale è ciò che leggono i test: non
  aggiungere diagnostica solo su VGA.
- **Niente `\t`** nell'output: `vga_putc` gestisce `>= 32` più `'\n'` e `'\b'`.
- **Dopo ogni modifica al kernel si esegue `make test`**, non solo `make`.
- **Ogni attesa e ogni ciclo di risoluzione vuole un tetto.** Regola di M10.
- **`SECTOR_SIZE` è 512** e sta in `include/blockdev.h`.

## Chi scrive cosa

Deroga NON concessa: Walter ha scelto «design + interfacce mie, implementa lui».
CLAUDE.md prevale sul template di questa skill, quindi per i file di Walter gli
step **non contengono l'implementazione**. Contengono invece le tre cose che la
sostituiscono senza toglierla di mano: il **contratto** (l'header, già scritto),
il **test-bersaglio** (codice reale, che deve passare), e le **trappole con
nome**.

| file | autore | perché |
|---|---|---|
| `include/*.h` | Claude | contratti d'interfaccia, regola di CLAUDE.md |
| `kernel/dev.c` | **Walter** | è `device.c` riscritto |
| `kernel/devio.c` | **Walter** | è la logica della milestone |
| `kernel/devfs.c` | **Walter** | in lista |
| `kernel/shell.c` | **Walter** | in lista |
| `kernel/vga.c`, `kernel/keyboard.c` | **Walter** | in lista |
| `kernel/main.c`, `kernel/serial.c`, `kernel/selftest.c`, `kernel/ata.c` | Claude | in lista «Claude scrive e mantiene» |
| `Makefile`, `tests/**` | Claude | infrastruttura |

**Eccezione al Task 1, concordata:** il rename è meccanico e non contiene
decisioni, quindi lo fa Claude per intero, `vga.c`/`keyboard.c`/`devfs.c`/`shell.c`
compresi. Cambia un nome di tipo e nient'altro.

## File structure

**Nuovi:**

| file | responsabilità |
|---|---|
| `include/dev.h` | `struct dev_entry`, il registry. Non conosce l'I/O |
| `include/chardev.h` | `struct chardev`. Ex `device.h`, senza nome e numeri |
| `include/devio.h` | il ponte: registrazione, lookup tipizzati, `fill_inode`, `caps` |
| `kernel/dev.c` | il registry: array, iscrizione con sei rifiuti, tre ricerche |
| `kernel/devio.c` | due wrapper, due lookup tipizzati, due vtable `static`, `fill_inode`, `caps` |
| `tests/host/test_dev.c` | il registry, ex `test_device.c` |
| `tests/host/test_devio.c` | l'aritmetica dell'adapter, con un disco finto in RAM |
| `tests/host/test_devfs.c` | l'albero, il lazy init, l'identità dei puntatori |

**Modificati:** `include/blockdev.h` (perde `BLK_NAME_MAX`), `include/vfs.h`
(`INODE_BLOCKDEV`), `kernel/devfs.c` (riscritto), `kernel/shell.c`,
`kernel/main.c`, `kernel/serial.c`, `kernel/vga.c`, `kernel/keyboard.c`,
`kernel/ata.c`, `kernel/selftest.c`, `Makefile`, `tests/host/Makefile`,
`tests/shell.sh`.

**Eliminati:** `include/device.h`, `kernel/device.c`, `tests/host/test_device.c`.

**Invariato e verificato tale:** `kernel/vfs.c`, `kernel/minixfs.c`,
`kernel/procfs.c`. `git diff --stat` su questi tre dev'essere **vuoto** a fine
milestone — è la misura binaria che ha dato senso a M11d.

---

## Task 1: Il rename, e niente altro

**Autore:** Claude (meccanico, eccezione concordata).

**Files:**
- Create: `include/chardev.h` (da `include/device.h`)
- Delete: `include/device.h`
- Modify: `kernel/device.c`, `kernel/serial.c`, `kernel/vga.c`,
  `kernel/keyboard.c`, `kernel/devfs.c`, `kernel/shell.c`, `kernel/main.c`,
  `kernel/selftest.c`, `kernel/ata.c` (solo un commento), `Makefile`
- Test: `tests/host/test_chardev.c` (da `test_device.c`),
  `tests/host/test_keyboard.c`, `tests/host/Makefile`

**Interfaces:**
- Consumes: niente.
- Produces: `struct chardev` con gli stessi campi di `struct device`
  (`name[DEV_NAME_MAX]`, `major`, `minor`, `read`, `write`, `priv`);
  `chardev_init()`, `chardev_register()`, `chardev_find()`, `chardev_by_id()`,
  `chardev_count()`, `chardev_at()`. `MAX_DEVICES` resta `MAX_DEVICES` in questo
  task — diventa `DEV_MAX` nel Task 2, quando `dev.h` esiste.

**Criterio di fine:** nessun cambiamento di comportamento. **Il numero dei test
che passano è identico prima e dopo**, e si misura.

- [ ] **Step 1: misurare la base, prima di toccare niente**

```bash
make -s clean >/dev/null && make -s >/dev/null 2>&1
make -C tests/host -s run 2>/dev/null | grep -cE "ok +--"
```

Annota il numero. Deve essere lo stesso all'ultimo step di questo task. Poi:

```bash
make test 2>&1 | tail -20
```

Deve essere verde. Se non lo è, **fermarsi**: il refactor non parte da un albero
rotto.

- [ ] **Step 2: `git mv` dell'header e del test**

```bash
git mv include/device.h include/chardev.h
git mv tests/host/test_device.c tests/host/test_chardev.c
```

`git mv` e non `cp` + `rm`: la storia del file si segue, e in un refactor di
rename è l'unico modo di leggere il diff.

- [ ] **Step 3: rinominare dentro `chardev.h`**

Sostituzioni, in quest'ordine (il primo è il più specifico):

| da | a |
|---|---|
| `WALTEX_DEVICE_H` | `WALTEX_CHARDEV_H` |
| `struct device` | `struct chardev` |
| `device_init` | `chardev_init` |
| `device_register` | `chardev_register` |
| `device_find` | `chardev_find` |
| `device_by_id` | `chardev_by_id` |
| `device_count` | `chardev_count` |
| `device_at` | `chardev_at` |

I commenti si aggiornano dove nominano `struct device` o `device_*`. **Non** si
riscrivono: la loro sostanza vale ancora, e il Task 2 è il posto dove cambiano di
significato.

- [ ] **Step 4: rinominare in tutti i consumatori**

```bash
for f in kernel/device.c kernel/serial.c kernel/vga.c kernel/keyboard.c \
         kernel/devfs.c kernel/shell.c kernel/main.c kernel/selftest.c \
         tests/host/test_chardev.c tests/host/test_keyboard.c; do
    sed -i 's/\bstruct device\b/struct chardev/g;
            s/\bdevice_init\b/chardev_init/g;
            s/\bdevice_register\b/chardev_register/g;
            s/\bdevice_find\b/chardev_find/g;
            s/\bdevice_by_id\b/chardev_by_id/g;
            s/\bdevice_count\b/chardev_count/g;
            s/\bdevice_at\b/chardev_at/g;
            s/"device\.h"/"chardev.h"/g' "$f"
done
git mv kernel/device.c kernel/chardev.c
```

Poi **rileggere a mano** i diff di `kernel/ata.c` e dei commenti: `sed` prende
anche le occorrenze dentro il testo, e in due punti il commento parla del
`device_at` di M8 come riferimento storico — lì il nome vecchio è corretto e va
ripristinato. `ata.c:360` è uno.

- [ ] **Step 5: aggiornare i due Makefile**

In `Makefile`, la lista degli oggetti: `kernel/device.c` → `kernel/chardev.c`.
In `tests/host/Makefile`:

```make
BINS := test_kprintf test_memory test_timer test_ring test_keyboard test_task \
        test_lineedit test_shell test_chardev test_vfs test_minixfs test_procfs

test_chardev: test_chardev.c ../../kernel/chardev.c ../../kernel/memory.c
	$(CC) $(CFLAGS) -o $@ $^
```

e nella regola di `test_shell`, `../../kernel/device.c` → `../../kernel/chardev.c`.

- [ ] **Step 6: compilare e misurare che nulla sia cambiato**

```bash
make -s clean >/dev/null && make -s 2>&1 | grep -i "warn\|error" ; echo "build: $?"
make -C tests/host -s run 2>/dev/null | grep -cE "ok +--"
```

Il conteggio deve essere **identico** allo Step 1. Un numero diverso in un task di
rename significa che si è cambiato un comportamento senza accorgersene.

- [ ] **Step 7: `make test`**

```bash
make test 2>&1 | tail -20
```

Verde. Poi il controllo che dice che nulla di concettuale si è mosso — e per
questo task **non** è il `git diff` sui tre filesystem, che era una previsione
sbagliata di questo piano:

```bash
git diff --stat HEAD | tail -3
```

Le inserzioni devono essere **esattamente** quante le cancellazioni. Un rename
puro non aggiunge e non toglie righe, e un numero asimmetrico è il segnale che
qualcosa di diverso da un nome è cambiato. Misurato: 180 e 180 su 18 file.

**Perché non il diff sui tre filesystem.** `kernel/minixfs.c` contiene due
riferimenti incrociati a `device_at` e `device_init` dentro i commenti, e un task
di rename non può lasciarli intatti *e* avere un diff vuoto lì. Si rinominano,
perché puntano a funzioni che esistono ancora sotto il nome nuovo, e un puntatore
che non risolve più è peggio di un nome storicamente impreciso.

**Il controllo binario alla M11d vale dai Task 2 in poi**, dove un diff in
`vfs.c`, `minixfs.c` o `procfs.c` è un vero segnale d'allarme: lì nessun nome
cambia, quindi qualunque modifica sarebbe funzionale.

- [ ] **Step 8: commit**

```bash
git add -A
git commit -m "M11e/1: struct device diventa struct chardev

Solo rename, nessun cambiamento di comportamento: il conteggio dei test
host e' identico prima e dopo. Prepara il registry agnostico del passo 2,
dove i dispositivi a blocchi si iscrivono accanto a quelli a caratteri.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Il registry agnostico, e le struct che diventano `static`

**Autore:** Claude gli header, il test e `main.c`/`serial.c`/`ata.c`/`selftest.c`.
**Walter** `kernel/dev.c`, i wrapper e i lookup in `kernel/devio.c`,
`kernel/vga.c`, `kernel/keyboard.c`, `kernel/shell.c`.

**Files:**
- Create: `include/dev.h`, `include/devio.h`, `kernel/dev.c`, `kernel/devio.c`,
  `tests/host/test_dev.c`
- Delete: `kernel/chardev.c`, `tests/host/test_chardev.c` (i contenuti si
  trasferiscono)
- Modify: `include/chardev.h`, `include/blockdev.h`, `kernel/serial.c`,
  `kernel/vga.c`, `kernel/keyboard.c`, `kernel/ata.c`, `kernel/main.c`,
  `kernel/shell.c`, `kernel/selftest.c`, `Makefile`, `tests/host/Makefile`

**Interfaces:**
- Consumes: `struct chardev` dal Task 1.
- Produces: tutto ciò che è negli Step 1-3, e su cui i Task 3 e 4 si appoggiano.

> **TRAPPOLA NUMERO UNO DELLA MILESTONE.** `impl` è un puntatore, e
> `chardev_register` **copiava**. Le `struct chardev` di `serial.c`, `vga.c` e
> `keyboard.c` sono locali di funzione — dopo questo task diventano tre puntatori
> penzolanti. Devono diventare `static` **in questo stesso commit**. Il kernel
> booterebbe comunque, `ls /dev` funzionerebbe, e
> `assert(chardev_register(...) == 0)` sarebbe verde: il guasto arriva al primo
> `read` dopo che quello stack è stato riusato, come un salto a un indirizzo
> arbitrario. `ata.c` è già a posto — tiene un array `static`.

- [ ] **Step 1: scrivere `include/dev.h`** (Claude)

```c
#ifndef WALTEX_DEV_H
#define WALTEX_DEV_H

#include "types.h"

#define DEV_MAX      16
#define DEV_NAME_MAX 16     /* NUL compreso: 15 caratteri utili */

/* DEV_NONE = 0 e non DEV_CHAR = 0, ed e' la convenzione di INODE_NONE e di
   "nessun inode vale zero" di procfs: uno slot mai riempito non deve poter
   passare per un dispositivo a caratteri valido. */
enum dev_kind { DEV_NONE = 0, DEV_CHAR, DEV_BLOCK };

/* Identita' e presenza, e NIENTE I/O — che e' l'intero refactor in una frase.
   Finche' il registro conosceva read/write a byte, "essere un dispositivo" e
   "avere una vista a byte" erano la stessa proprieta', e un disco ha la prima
   e non la seconda: la sua granularita' e' il settore, e una lettura corta su
   un disco e' un guasto, non un esito normale.

   name e' un ARRAY, e la ragione e' quella di M8: chi si iscrive riempie una
   struct e il registro COPIA, quindi conserva un nome che non appartiene a
   nessun altro.

   impl e' un PUNTATORE, e qui la convenzione di M8 si spezza: il nome si copia,
   le operazioni si RIFERISCONO. Quindi la struct puntata deve sopravvivere a chi
   la iscrive — static, non locale. Vedi il commento in serial_init. */
struct dev_entry {
    char          name[DEV_NAME_MAX];
    enum dev_kind kind;
    uint16_t      major, minor;   /* metadati: nessun lookup li usa ancora */
    void         *impl;           /* struct chardev * | struct blockdev * */
};

/* Porta il registro a "nessun dispositivo iscritto". Va chiamata da kmain PRIMA
   di ogni *_init() dei driver, perche' sono loro a iscriversi. */
void dev_init(void);

/* Copia la voce nel primo slot libero. 0 se iscritta, -1 se rifiutata, e i
   motivi del rifiuto sono SEI:

     - il registro e' pieno;
     - kind non e' DEV_CHAR ne' DEV_BLOCK;
     - name non contiene un NUL nei primi DEV_NAME_MAX byte. Va rilevato
       scandendo AL MASSIMO DEV_NAME_MAX byte: una strlen normale su una stringa
       non terminata cammina fuori dall'array, ed e' il bug che device_register
       aveva davvero — la guardia con strpos era codice morto, perche' strpos
       cercando '\0' ritorna sempre -1;
     - name e' vuoto;
     - un dispositivo con quel nome c'e' gia': la ricerca diventerebbe ambigua e
       vincerebbe il primo in silenzio;
     - la coppia (major, minor) e' gia' presa. Si prova con dev_by_id, cosi' la
       scansione per coppia esiste una volta sola.

   In tutti e sei si RIFIUTA, non si aggiusta: troncare un nome troppo lungo
   farebbe collidere due nomi distinti, cioe' trasformerebbe un errore del driver
   in un bug di ricerca che si manifesta da un'altra parte.

   NON controlla i puntatori a operazione, perche' non li conosce. Quel controllo
   vive nei due wrapper, dove diventa asimmetrico come deve essere: un disco da
   cui non si legge non ha senso, un disco su cui non si scrive e' read-only. */
int dev_register(const struct dev_entry *e);

/* L'INDICE della voce con quel nome, o -1. Un indice e non un puntatore, perche'
   devfs ci sceglie lo slot dell'inode nel proprio pool. Corrispondenza ESATTA:
   "cons" non trova "console". */
int dev_lookup_index(const char *name);

/* La voce i, o 0 se i e' negativo o >= dev_count(). Il controllo sul negativo
   non e' pedanteria: entries[-1] legge i byte prima dell'array.

   Ritorna const, cosa che device_find non poteva: allora il puntatore doveva
   essere mutabile perche' chi lo riceveva chiamava d->write(d, ...). Ora
   l'oggetto mutabile e' impl, che la voce riferisce e non possiede. */
const struct dev_entry *dev_get(int i);

/* La voce con quella coppia, o 0. E' anche l'implementazione del sesto rifiuto
   di dev_register, e i due si giustificano a vicenda: la ricerca per coppia e'
   ben definita PERCHE' la coppia e' unica per costruzione.

   Il chiamante che l'header di M8 prometteva — il VFS — non e' mai arrivato.
   Arrivera' con un nodo di dispositivo su minix, che memorizza major/minor e non
   il nome. */
const struct dev_entry *dev_by_id(uint16_t major, uint16_t minor);

/* Quanti sono iscritti. Il numero e' TENUTO, non ricalcolato: contare scorrendo
   introdurrebbe una seconda verita' che puo' divergere dalla prima. */
int dev_count(void);

#endif
```

- [ ] **Step 2: potare `include/chardev.h`** (Claude)

`struct chardev` perde `name`, `major` e `minor`, che vivono in `dev_entry`.
Restano `read`, `write`, `priv`, con le firme aggiornate a `struct chardev *`.
Sparisce `MAX_DEVICES` (ora `DEV_MAX` in `dev.h`) e spariscono le sei
dichiarazioni `chardev_*` del registro — `chardev_register` si ridichiara in
`devio.h` con una firma nuova, e le altre cinque non esistono più: al loro posto
ci sono `dev_lookup_index`/`dev_get`/`dev_by_id`/`dev_count` più `dev_chardev`.

I commenti su «un puntatore nullo significa non supportata» e su «`read` ritorna
quanti byte davvero, e zero non è EOF» **restano parola per parola**. Il commento
su `name` come array **si sposta** in `dev.h` (già fatto nello Step 1) e qui
viene rimpiazzato dall'avvertimento opposto:

```c
/* ATTENZIONE: questa struct NON viene piu' copiata dal registro. dev_entry.impl
   ne conserva l'INDIRIZZO, quindi deve sopravvivere a chi la iscrive: static o
   globale, mai una locale di funzione. Fino a M11e era il contrario, e i tre
   driver la riempivano sullo stack. */
```

- [ ] **Step 3: scrivere `include/devio.h`** (Claude)

```c
#ifndef WALTEX_DEVIO_H
#define WALTEX_DEVIO_H

#include "types.h"
#include "dev.h"

struct chardev;
struct blockdev;
struct inode;

/* Le capacita', come maschera. Serve a "devs" per la colonna r-/-w, e sta qui
   invece che in shell.c perche' lo switch su enum dev_kind deve esistere in UN
   posto solo — questo file. */
#define DEVIO_CAN_READ  1
#define DEVIO_CAN_WRITE 2

/* I due wrapper: compilano una dev_entry e la passano a dev_register. Ritornano
   quello che ritorna lui, piu' un rifiuto proprio ciascuno.

   L'asimmetria e' voluta e non si puo' esprimere con un controllo condiviso,
   perche' quello distingue solo "almeno uno":

                      read == 0                     write == 0
     chardev   lecito: console non si legge   lecito: kbd non si scrive
     blockdev  RIFIUTATO: un disco da cui     lecito: e' un disco read-only
               non si legge non ha senso

   c e b devono sopravvivere alla chiamata: il registro conserva il puntatore. */
int chardev_register (const char *name, uint16_t major, uint16_t minor,
                      struct chardev  *c);
int blockdev_register(const char *name, uint16_t major, uint16_t minor,
                      struct blockdev *b);

/* Lookup TIPIZZATI: controllano kind PRIMA di castare impl, e ritornano 0 se il
   nome non c'e' o se e' dell'altra specie. Il cast da void * che il chiamante
   non puo' verificare e' il punto dove un errore di registrazione diventa un
   salto in un indirizzo arbitrario. */
struct chardev  *dev_chardev (const char *name);
struct blockdev *dev_blockdev(const char *name);

/* Riempie *in come vista a FILE del dispositivo e: type, ops, priv, size,
   major, minor. 0 se ci riesce, -1 se e' nullo o se kind non e' uno dei due — e
   su -1 NON tocca *in, che e' la convenzione di lookup e di create.

   NON scrive ino: quello e' affare di devfs, e va scritto per ULTIMO. Il perche'
   sta nel commento di devfs_lookup.

   E' l'unico posto che assegna le due vtable, che per questo restano static
   dentro devio.c. */
int devio_fill_inode(const struct dev_entry *e, struct inode *in);

/* La maschera delle capacita', o 0 per una voce invalida. Zero e' anche il
   valore di un dispositivo che non sa fare niente, che i due wrapper rifiutano —
   quindi in pratica non si presenta, e non serve distinguerlo. */
int devio_caps(const struct dev_entry *e);

#endif
```

- [ ] **Step 4: togliere `BLK_NAME_MAX` da `include/blockdev.h`** (Claude)

Con un registry unico il limite del nome è uno, ed è `DEV_NAME_MAX`. Due costanti
con lo stesso valore sono due verità che possono divergere. `struct blockdev`
tiene ancora il proprio `name[]` — serve a `lsblk` per stampare e a `ata.c` per
distinguere i due dischi — e diventa `char name[DEV_NAME_MAX]`, con
`#include "dev.h"`.

La tabella nel commento in testa al file va aggiornata: la riga «chi consuma —
devfs, cioè il VFS / minixfs, in M11» diventa «devio, che ne fabbrica la vista a
byte / minixfs, che ci parla in LBA senza passare da lì». È la frase che dice
perché l'adapter non ha rotto M11a.

- [ ] **Step 5: scrivere il test del registry** (Claude)

`tests/host/test_dev.c`. Struttura e helper presi da `test_chardev.c`: `check`,
`same`, `failures`, `main` che ritorna 1 se qualcosa fallisce.

```c
/* Test del registry dei dispositivi, col gcc dell'host.

   Il registry e' logica pura sopra un array statico: nessuna porta I/O, nessun
   interrupt, nessuna allocazione. Nessuno stub — dev.c chiama soltanto strcmp,
   che arriva da memory.c. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "dev.h"

static int failures;

static void check(const char *name, int ok)
{
    printf(ok ? "ok   -- %s\n" : "FAIL -- %s\n", name);
    if (!ok)
        failures++;
}

static int same(const char *a, const char *b)
{
    int i;

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (a[i] != b[i])
            return 0;
    }

    return a[i] == b[i];
}

/* Un impl finto. Al registry interessa solo che il puntatore non sia nullo: non
   sa cosa sia, e questo test e' il posto dove quel fatto si vede. */
static int finto_impl;

static struct dev_entry fai(const char *nome, enum dev_kind kind,
                            uint16_t major, uint16_t minor)
{
    struct dev_entry e;
    int i;

    for (i = 0; i < DEV_NAME_MAX; i++)
        e.name[i] = '\0';
    for (i = 0; i < DEV_NAME_MAX - 1 && nome[i] != '\0'; i++)
        e.name[i] = nome[i];

    e.kind  = kind;
    e.major = major;
    e.minor = minor;
    e.impl  = &finto_impl;

    return e;
}

/* ---- iscrizione ---------------------------------------------------------- */

static void test_iscrizione_semplice(void)
{
    struct dev_entry e = fai("console", DEV_CHAR, 5, 1);

    dev_init();
    check("il registry parte vuoto", dev_count() == 0);
    check("dev_register accetta la prima voce", dev_register(&e) == 0);
    check("dev_count passa a 1", dev_count() == 1);
}

/* Il NOME si copia, e questo test sopravvive a M11e invariato nella sostanza. */
static void test_il_nome_si_copia(void)
{
    struct dev_entry e = fai("uno", DEV_CHAR, 1, 1);
    const struct dev_entry *reg;

    dev_init();
    dev_register(&e);

    e.name[0] = 'X';

    reg = dev_get(0);
    check("il nome nel registry non cambia se la sorgente cambia",
          reg != 0 && same(reg->name, "uno"));
}

/* Il ROVESCIO del precedente, ed e' il contratto NUOVO di M11e: impl e' un
   puntatore, quindi cio' che sta dall'altro capo NON e' copiato.

   Sta qui perche' e' la trappola numero uno della milestone. Fino a M11d il
   registro copiava l'intera struct e i tre driver la riempivano sullo stack;
   adesso quella memoria deve sopravvivere, e questo test e' il posto dove il
   nuovo contratto e' scritto in modo eseguibile. */
static void test_impl_si_riferisce(void)
{
    int oggetto = 7;
    struct dev_entry e = fai("due", DEV_CHAR, 2, 2);
    const struct dev_entry *reg;

    e.impl = &oggetto;

    dev_init();
    dev_register(&e);

    oggetto = 9;

    reg = dev_get(0);
    check("impl RIFERISCE e non copia: il cambiamento si vede",
          reg != 0 && reg->impl == &oggetto && *(int *)reg->impl == 9);
}

/* ---- i sei rifiuti ------------------------------------------------------- */

static void test_registry_pieno(void)
{
    struct dev_entry e;
    int i;

    dev_init();

    for (i = 0; i < DEV_MAX; i++) {
        char nome[4];

        nome[0] = 'a';
        nome[1] = (char)('0' + (i / 10));
        nome[2] = (char)('0' + (i % 10));
        nome[3] = '\0';

        e = fai(nome, DEV_CHAR, (uint16_t)(100 + i), 0);
        if (dev_register(&e) != 0)
            break;
    }

    check("dev_count arriva a DEV_MAX", dev_count() == DEV_MAX);

    e = fai("uno_di_troppo", DEV_CHAR, 200, 0);
    check("la voce oltre DEV_MAX e' rifiutata", dev_register(&e) == -1);
    check("e non e' entrata", dev_count() == DEV_MAX);
}

static void test_kind_invalido(void)
{
    struct dev_entry e = fai("boh", DEV_NONE, 9, 9);

    dev_init();
    check("DEV_NONE e' rifiutato", dev_register(&e) == -1);

    e = fai("boh", (enum dev_kind)99, 9, 9);
    check("un kind fuori intervallo e' rifiutato", dev_register(&e) == -1);
    check("nessuno dei due e' entrato", dev_count() == 0);
}

static void test_nome_duplicato(void)
{
    struct dev_entry a = fai("dup", DEV_CHAR, 1, 1);
    struct dev_entry b = fai("dup", DEV_BLOCK, 2, 2);

    dev_init();
    check("la prima con quel nome entra", dev_register(&a) == 0);
    check("la seconda con lo STESSO nome e' rifiutata", dev_register(&b) == -1);
    check("il registry ne contiene una sola", dev_count() == 1);

    /* Il rifiuto vale anche fra specie diverse: /dev ha un solo namespace, come
       su Unix, dove non possono coesistere un hda a caratteri e un hda a
       blocchi. */
    check("dev_lookup_index trova la prima", dev_lookup_index("dup") == 0);
    check("ed e' quella a caratteri",
          dev_get(0) != 0 && dev_get(0)->kind == DEV_CHAR);
}

/* Il rifiuto NUOVO di M11e. */
static void test_coppia_duplicata(void)
{
    struct dev_entry a = fai("uno", DEV_CHAR, 5, 1);
    struct dev_entry b = fai("due", DEV_CHAR, 5, 1);
    struct dev_entry c = fai("tre", DEV_CHAR, 5, 2);

    dev_init();
    check("la prima con 5:1 entra", dev_register(&a) == 0);
    check("una seconda con 5:1 e' rifiutata anche con un nome diverso",
          dev_register(&b) == -1);
    check("5:2 invece entra: e' il minor a distinguerle",
          dev_register(&c) == 0);
    check("il registry ne contiene due", dev_count() == 2);
}

/* I DUE casi di layout, ed e' la cura del bug di device_register.

   La guardia con strpos era codice morto — strpos cercando '\0' ritorna sempre
   -1, per un ramo esplicito in memory.c — e cio' che proteggeva davvero era la
   strlen sotto, cioe' precisamente la scansione illimitata contro cui l'header
   metteva in guardia. Funzionava per accidente di layout: major stava subito
   dopo name. In dev_entry al suo posto c'e' kind, che e' un int. */
static void test_nome_al_limite(void)
{
    struct dev_entry e;
    int i;

    /* Caso A: il byte dopo l'array NON e' zero, quindi una strlen illimitata
       risponde piu' di DEV_NAME_MAX. Passa anche con il controllo sbagliato. */
    dev_init();
    e = fai("", DEV_CHAR, 4, 4);
    for (i = 0; i < DEV_NAME_MAX; i++)
        e.name[i] = 'y';

    check("un nome non terminato e' rifiutato, non troncato",
          dev_register(&e) == -1);
    check("il nome non terminato non e' entrato", dev_count() == 0);

    /* Caso B, ed e' quello che conta: kind sta all'offset DEV_NAME_MAX, e
       DEV_CHAR vale 1 su una macchina little-endian — quindi il byte subito dopo
       l'array vale 1 e non zero.

       Per costruire il caso in cui una strlen illimitata risponde ESATTAMENTE
       DEV_NAME_MAX serve che quel byte sia zero, e con dev_entry si ottiene
       azzerando kind. Ma kind zero e' DEV_NONE, che viene rifiutato dal secondo
       controllo — quindi questo caso NON distingue piu' i due controlli come
       faceva in M8.

       Lo si costruisce allora con un nome di DEV_NAME_MAX byte esatti in cui il
       NUL manca ma la scansione limitata se ne accorge, e si verifica che
       l'ordine dei controlli sia quello giusto: il nome si valida PRIMA di
       guardare kind, altrimenti questo caso passerebbe per la ragione
       sbagliata. */
    dev_init();
    e = fai("", DEV_CHAR, 0, 0);
    for (i = 0; i < DEV_NAME_MAX; i++)
        e.name[i] = 'y';

    check("un nome di DEV_NAME_MAX byte esatti e' rifiutato",
          dev_register(&e) == -1);
    check("il nome di DEV_NAME_MAX byte non e' entrato", dev_count() == 0);
}

static void test_nome_vuoto(void)
{
    struct dev_entry e = fai("", DEV_CHAR, 6, 6);

    dev_init();
    check("un nome vuoto e' rifiutato", dev_register(&e) == -1);
    check("e non e' entrato", dev_count() == 0);
}

/* ---- le tre ricerche ----------------------------------------------------- */

static void test_lookup_index(void)
{
    struct dev_entry a = fai("console", DEV_CHAR, 5, 1);
    struct dev_entry b = fai("ttyS0",   DEV_CHAR, 4, 64);
    struct dev_entry c = fai("hda",     DEV_BLOCK, 3, 0);

    dev_init();
    dev_register(&a);
    dev_register(&b);
    dev_register(&c);

    check("dev_lookup_index rende l'indice di iscrizione",
          dev_lookup_index("console") == 0 &&
          dev_lookup_index("ttyS0")   == 1 &&
          dev_lookup_index("hda")     == 2);

    check("dev_lookup_index su un nome assente rende -1",
          dev_lookup_index("pippo") == -1);

    /* La corrispondenza e' ESATTA, e in /dev sarebbe la differenza fra aprire
       un file e aprirne un altro. */
    check("dev_lookup_index non accetta un prefisso",
          dev_lookup_index("cons") == -1);

    check("dev_lookup_index non accetta la stringa vuota",
          dev_lookup_index("") == -1);
}

static void test_get(void)
{
    struct dev_entry a = fai("uno", DEV_CHAR,  1, 1);
    struct dev_entry b = fai("due", DEV_BLOCK, 2, 2);

    dev_init();
    dev_register(&a);
    dev_register(&b);

    check("dev_get rende le voci in ordine di iscrizione",
          dev_get(0) != 0 && same(dev_get(0)->name, "uno") &&
          dev_get(1) != 0 && same(dev_get(1)->name, "due"));

    check("dev_get conserva kind, major e minor",
          dev_get(1)->kind  == DEV_BLOCK &&
          dev_get(1)->major == 2 && dev_get(1)->minor == 2);

    /* Sotto zero legge i byte prima dell'array; sopra il conteggio legge uno
       slot mai scritto, e chi ne usasse impl salterebbe in un indirizzo
       arbitrario. */
    check("dev_get fuori intervallo rende 0",
          dev_get(-1) == 0 && dev_get(dev_count()) == 0 &&
          dev_get(DEV_MAX) == 0);
}

static void test_by_id(void)
{
    struct dev_entry a = fai("uno", DEV_CHAR, 5, 1);
    struct dev_entry b = fai("due", DEV_CHAR, 5, 2);

    dev_init();
    dev_register(&a);
    dev_register(&b);

    check("dev_by_id trova per coppia",
          dev_by_id(5, 1) != 0 && same(dev_by_id(5, 1)->name, "uno"));
    check("dev_by_id distingue due minor sotto lo stesso major",
          dev_by_id(5, 2) != 0 && same(dev_by_id(5, 2)->name, "due"));
    check("dev_by_id su una coppia assente rende 0", dev_by_id(5, 3) == 0);
}

int main(void)
{
    test_iscrizione_semplice();
    test_il_nome_si_copia();
    test_impl_si_riferisce();
    test_registry_pieno();
    test_kind_invalido();
    test_nome_duplicato();
    test_coppia_duplicata();
    test_nome_al_limite();
    test_nome_vuoto();
    test_lookup_index();
    test_get();
    test_by_id();

    if (failures == 0) {
        printf("tutti i test del registry dei dispositivi passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
```

- [ ] **Step 6: aggiungere la regola nel `tests/host/Makefile`** (Claude)

```make
# memory.c per strcmp, che dev_lookup_index usa per il confronto esatto dei nomi.
# Nessuno stub: il registry e' logica pura, e non conosce nemmeno cosa sia un
# dispositivo — impl e' un void *.
test_dev: test_dev.c ../../kernel/dev.c ../../kernel/memory.c
	$(CC) $(CFLAGS) -o $@ $^
```

e in `BINS`, `test_chardev` → `test_dev`. Poi `git rm tests/host/test_chardev.c`
e `git rm kernel/chardev.c`.

- [ ] **Step 7: verificare che il test FALLISCA per il motivo giusto** (Claude)

```bash
make -C tests/host test_dev 2>&1 | tail -5
```

Atteso: errore di link, `undefined reference to 'dev_init'` e compagnia —
`kernel/dev.c` non esiste ancora. **Non** un errore di compilazione in
`test_dev.c`: quello vorrebbe dire che l'header è sbagliato, non che
l'implementazione manca.

- [ ] **Step 8: `kernel/dev.c`** (WALTER)

Contratto: `include/dev.h`, Step 1. Test-bersaglio: `test_dev.c`, Step 5 — 30
controlli, e passano tutti solo con i sei rifiuti implementati.

Le trappole, con nome:

1. **La scansione del nome è limitata a `DEV_NAME_MAX` byte.** Non `strlen`, che
   è il bug che si sta chiudendo. Non `strpos` cercando `'\0'`, che ritorna
   sempre -1.
2. **L'ordine dei controlli conta**, e `test_nome_al_limite` lo verifica: il nome
   si valida **prima** di guardare `kind`, altrimenti il caso B passerebbe per la
   ragione sbagliata.
3. **`ndev` è tenuto, non ricalcolato.** Contare scorrendo è la seconda verità.
4. **Il duplicato di coppia si prova con `dev_by_id`**, non con un secondo ciclo.
5. **Nessun `cli`.** L'iscrizione avviene al boot, prima della prima `sti`.

Verifica:

```bash
make -C tests/host -s test_dev && ./tests/host/test_dev | tail -3
```

Atteso: `tutti i test del registry dei dispositivi passano`.

- [ ] **Step 9: i wrapper e i lookup tipizzati in `kernel/devio.c`** (WALTER)

Solo le quattro funzioni degli Step 3 — `chardev_register`, `blockdev_register`,
`dev_chardev`, `dev_blockdev`. **Non** `devio_fill_inode` né `devio_caps`: quelle
sono il Task 4 e il Task 2 rispettivamente, e anticiparle rende ambigua ogni
diagnosi.

Trappole:

1. **L'asimmetria dei rifiuti** è nella tabella di `devio.h`. `blockdev_register`
   rifiuta `read == 0`; `chardev_register` no.
2. **Il nome si copia nella `dev_entry` locale**, e `dev_register` la copia
   ancora. Serve una copia limitata a `DEV_NAME_MAX` byte — e va scritta a mano,
   perché `strncpy` non esiste. Attenzione a terminare: un nome di 16 caratteri
   dal chiamante deve produrre un rifiuto, non un array senza NUL.
3. **`kind` prima del cast**, nei due lookup.

- [ ] **Step 10: `devio_caps` in `kernel/devio.c`** (WALTER)

Serve a `shell_devs` nello Step 13. `DEVIO_CAN_READ | DEVIO_CAN_WRITE` secondo
quale dei due puntatori dell'`impl` è non nullo, con lo `switch` su `kind` per
sapere di che specie sia l'`impl`. Zero per una voce invalida.

- [ ] **Step 11: le tre struct diventano `static`** (Claude `serial.c`, WALTER `vga.c` e `keyboard.c`)

`serial.c`, `vga.c`, `keyboard.c`: `struct chardev dev = {...}` →
`static struct chardev dev = {...}`, e la chiamata diventa
`chardev_register("ttyS0", 4, 64, &dev)`.

**Il commento va riscritto dicendo l'opposto di quello che dice adesso.** In
`serial.c` oggi c'è:

> La struct è LOCALE, e non è una distrazione: `device_register` copia, quindi
> questa memoria può sparire appena `serial_init` ritorna.

Diventa:

```c
    /* La struct e' STATIC, e non e' una distrazione: da M11e il registry
       conserva il PUNTATORE invece di copiare — dev_entry.impl — quindi questa
       memoria deve sopravvivere a serial_init. Con una locale il kernel
       booterebbe comunque e ls /dev funzionerebbe: il guasto arriva al primo
       read dopo che quello stack e' stato riusato, come un salto in un indirizzo
       arbitrario.

       Il NOME invece si copia ancora, e sta nella dev_entry: la convenzione di
       M8 si e' spezzata in due, ed e' scritto in dev.h.

       L'inizializzatore designato azzera per intero cio' che non nomina, quindi
       read e priv finiscono a zero senza scriverlo. Conta anche da static: read
       conterrebbe un indirizzo su cui qualcuno prima o poi salterebbe. */
```

L'equivalente nei due file di Walter, con le sue parole.

- [ ] **Step 12: `ata_init` si iscrive** (Claude)

In `kernel/ata.c`, dopo che `IDENTIFY` ha popolato `drives[i]`:

```c
        /* I numeri sono quelli veri di Linux per il canale IDE primario: hda e'
           3:0 e hdb 3:64, verificati con ls -l /dev/hd*. Costa zero e sta nella
           stessa direzione del vincolo POSIX.

           Il ritorno si verifica: un'iscrizione che fallisce al boot deve essere
           rumorosa, e assert chiama panic. drives e' un array static, quindi il
           puntatore che il registry conserva resta valido — vedi il commento in
           serial_init. */
        assert(blockdev_register(drives[i].name, 3, (uint16_t)(i * 64),
                                 &drives[i]) == 0);
```

**Non** si tocca `ata_drive()`: sopravvive come accessore interno del driver, e
`minixfs` continua a riceverne un `struct blockdev *`. È l'invariante che dice
che l'adapter non ha rotto M11a.

- [ ] **Step 12b: `devfs.c` filtra `DEV_CHAR`, e il perché è un invariante del VFS** (WALTER)

**Questo step non era nel piano, e la sua assenza era un errore.** `devfs.c` usa
`chardev_at(i)->name` e `MAX_DEVICES`, che non esistono più: non può restare
intatto fino al Task 3. E la previsione dello Step 15 — «`cat /dev/hda` deve
fallire in modo pulito, l'inode non ha ancora `ops`» — era **falsa**:

```c
    if (f->inode->ops->read == 0) {     /* vfs.c, vfs_read */
```

`vfs_read` dereferenzia `ops` senza controllare che non sia nullo, e `vfs_readdir`
fa lo stesso. **Un inode ha SEMPRE `ops`** è un invariante del VFS, non una
cortesia — e un inode con `ops` nullo non fallisce, fa una tripla fault.

E dare a `hda` la vtable dei chardev è peggio: `chardev->read` su un
`struct blockdev *` legge un puntatore a funzione dall'offset sbagliato, e ci
salta.

Quindi in questo task, e nel Task 3, **`devfs` serve solo le voci con
`kind == DEV_CHAR`** — in `lookup` E in `readdir`, che devono restare d'accordo:
è la lezione di M11a, e se comparisse solo nella prima `cat /dev/hda` funzionerebbe
e `ls /dev` non lo mostrerebbe.

Le modifiche minime, mantenendo l'inizializzazione eager di M11d:
`chardev_at(i)` → `dev_get(i)`, `MAX_DEVICES` → `DEV_MAX`, nome e numeri dalla
voce, `priv = e->impl`, e il filtro sul `kind`.

**Guadagno inatteso:** `hda` sta nel registry e non in `/dev`, quindi
`dev_count()` vale 5 e `/dev` ha 3 voci. Quella discrepanza **è una prova**: dice
che il registry e l'albero sono due cose separate, cosa che fino a M11d non era
osservabile perché coincidevano sempre. Nel Task 4 riconvergono, e la
riconvergenza è la milestone.

Conseguenza sul mio `selftest.c`: `check_devfs_readdir` confronta `n` con
`dev_count()`, e nei Task 2-3 deve confrontarlo col numero di voci `DEV_CHAR`.
Torna a `dev_count()` nel Task 4.

- [ ] **Step 13: `shell.c` passa al registry** (WALTER)

- `shell_devs`: itera `dev_get(i)` per `i < dev_count()`, e stampa nome,
  `kind`, `major:minor`, e la colonna `r-`/`-w` da `devio_caps`. La colonna
  guadagna significato: `r-` su un disco vuol dire read-only, che prima non era
  esprimibile.
- `shell_lsblk`: itera il registry filtrando `kind == DEV_BLOCK`, e prende
  `nsectors` da `(struct blockdev *)e->impl`. Spariscono `ata_drive_count()` e
  `ata_drive()` da questo file. I cast a `int` in `kprintf` **restano**, col
  commento sul debito di `put_uint`.
- `disco_per_nome` si **elimina** e diventa `dev_blockdev(name)`.
  `disco_da_argv` la chiama. Vale per `rdsect` **e** `wrsect`: se solo `lsblk`
  passasse al registry resterebbero due modi di dire quali dischi esistono, e la
  divergenza qui sarebbe che `wrsect` scrive sul disco sbagliato.
- Il messaggio «nessun disco sul canale primario» diventa «nessun disco»: il
  registry non sa da quale canale vengano.

`shell.c` include già `blockdev.h`; aggiunge `dev.h` e `devio.h` e **non**
`chardev.h`, perché `devio_caps` gli risparmia di guardare i puntatori a
operazione.

- [ ] **Step 14: `main.c` e `selftest.c`** (Claude)

In `main.c`: `chardev_init()` → `dev_init()`;
`minixfs_init(ata_drive(1))` → `minixfs_init(dev_blockdev("hdb"))`, per nome e
non per indice, per la ragione già scritta in `shell.c` a proposito di `rdsect`;
`kprintf("waltex: /dev con %d dispositivi", dev_count())`.

In `selftest.c`: `check_device_count` diventa `report("i cinque driver si sono
iscritti", dev_count() == 5)`, e i tre `chardev_find("...")` diventano
`dev_chardev("...")`. Si aggiungono:

```c
static void check_dev_dischi_iscritti(void)
{
    report("hda e' nel registry", dev_blockdev("hda") != 0);
    report("hdb e' nel registry", dev_blockdev("hdb") != 0);

    /* La specie si rispetta in entrambi i versi, ed e' il controllo che dice che
       i due lookup guardano kind invece di castare e sperare. */
    report("hda non e' un dispositivo a caratteri", dev_chardev("hda") == 0);
    report("kbd non e' un dispositivo a blocchi", dev_blockdev("kbd") == 0);
}
```

- [ ] **Step 15: `make test`, e le tre misure** (Claude)

```bash
make test 2>&1 | tail -20
```

Verde. Poi:

```bash
# 1. i tre file che non devono essere cambiati
git diff --stat HEAD~1 -- kernel/vfs.c kernel/minixfs.c kernel/procfs.c

# 2. cinque voci in /dev
grep -c "waltex: /dev con 5 dispositivi" <(bash tests/smoke.sh 2>&1) || true

# 3. il conteggio, misurato e non stimato
make -C tests/host -s run 2>/dev/null | grep -cE "ok +--"
```

La prima dev'essere vuota. E il controllo a mano che vale più dei tre:

```bash
make run
```

e al prompt `devs`, `lsblk`, `ls /dev`. `hda` e `hdb` compaiono in `ls /dev`, e
`cat /dev/hda` **deve fallire in modo pulito** — l'inode non ha ancora `ops`,
perché `devio_fill_inode` arriva nel Task 4. Se salta o resetta la VM, `devfs`
sta consegnando un inode con `ops` nullo senza che nessuno lo controlli.

- [ ] **Step 16: commit**

```bash
git add -A
git commit -m "M11e/2: registry agnostico, e i dischi si iscrivono

dev_entry conosce nome, kind, major/minor e un void *impl, e nessuna
operazione di I/O: e' cio' che permette a un disco di stare nel registry
accanto a una tastiera senza che uno dei due debba mentire sulla propria
granularita'.

impl e' un PUNTATORE mentre device_register copiava, quindi le struct dei
tre driver a caratteri diventano static nello stesso commit: con una
locale il kernel booterebbe e il guasto arriverebbe al primo read dopo il
riuso dello stack. Il test che lo copriva e' stato rovesciato.

Chiude un bug latente di device_register: la guardia sul nome non
terminato era codice morto, perche' strpos cercando il terminatore
ritorna sempre -1, e cio' che proteggeva era la strlen illimitata contro
cui l'header metteva in guardia.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: `devfs` diventa solo un albero, e inizializza lazy

**Autore:** Claude il test e `INODE_BLOCKDEV`. **Walter** `kernel/devfs.c`.

**Files:**
- Create: `tests/host/test_devfs.c`
- Modify: `include/vfs.h`, `kernel/devfs.c`, `kernel/selftest.c`, `Makefile`,
  `tests/host/Makefile`

**Interfaces:**
- Consumes: `dev_lookup_index`, `dev_get`, `dev_count` dal Task 2;
  `devio_fill_inode` dal Task 4 — **che non esiste ancora**. In questo task
  `devfs.c` la chiama comunque, e `devio.c` ne fornisce una versione che gestisce
  **solo** `DEV_CHAR` e ritorna -1 su `DEV_BLOCK`. Il ramo a blocchi è il Task 4.
- Produces: `devfs_init()`, `devfs_root()`, `devfs_devdir()` — firme invariate.

- [ ] **Step 1: `INODE_BLOCKDEV` in `include/vfs.h`** (Claude)

```c
enum inode_type { INODE_NONE = 0, INODE_FILE, INODE_DIR, INODE_CHARDEV,
                  INODE_BLOCKDEV };
```

Si aggiunge **in coda**: `enum inode_type` non è serializzato da nessuna parte —
minix usa i propri bit di `i_mode` — ma cambiare i valori esistenti per ordine
alfabetico è il genere di modifica che non costa niente e non serve a niente.

E il commento su `major`/`minor` in `struct inode`:

```c
    uint16_t         major, minor;   /* validi se INODE_CHARDEV o INODE_BLOCKDEV */
```

**Non si tocca** la nota su `st_dev`: resta un problema diverso e resta di M14.

- [ ] **Step 2: scrivere il test dell'albero** (Claude)

`tests/host/test_devfs.c`. Deve poter iscrivere dispositivi finti nel registry
vero, quindi linka `dev.c`, `devio.c`, `devfs.c`, `memory.c`.

```c
/* Test dell'albero di devfs, col gcc dell'host.

   devfs non ha hardware sotto: legge il registry, che e' un array in .bss, e
   riempie inode. Quindi si prova interamente qui — ed e' il quinto modulo del
   progetto provato fuori da QEMU, dopo kprintf, il VFS, minixfs e procfs.

   Il dispositivo finto e' un struct chardev static: da M11e il registry ne
   conserva il puntatore, quindi una locale sarebbe il bug che test_dev
   documenta. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "dev.h"
#include "chardev.h"
#include "devio.h"
#include "devfs.h"
#include "vfs.h"

static int failures;

static void check(const char *name, int ok)
{
    printf(ok ? "ok   -- %s\n" : "FAIL -- %s\n", name);
    if (!ok)
        failures++;
}

static int same(const char *a, const char *b)
{
    int i;

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (a[i] != b[i])
            return 0;
    }

    return a[i] == b[i];
}

static int finta_read(struct chardev *c, void *buf, uint32_t n)
{
    (void)c; (void)buf;
    return (int)n;
}

/* STATIC, e non e' pigrizia: il registry ne conserva l'indirizzo. */
static struct chardev uno = { .read = finta_read };
static struct chardev due = { .read = finta_read };

static void test_radice_e_devdir(void)
{
    struct inode *root, *devdir, *out;

    dev_init();
    devfs_init();

    root   = devfs_root();
    devdir = devfs_devdir();

    check("devfs_root non e' nullo", root != 0);
    check("devfs_devdir non e' nullo", devdir != 0);
    check("sono due inode DISTINTI", root != devdir);
    check("la radice e' una directory",
          root != 0 && root->type == INODE_DIR);

    /* La radice ha UNA voce e si chiama dev. E' la ragione per cui kmain monta
       devfs_devdir() e non devfs_root(): innestando la radice si otterrebbe
       /dev/dev/kbd, e quattro self-check lo presero in M11a. */
    check("la radice contiene solo dev",
          root != 0 && root->ops != 0 && root->ops->lookup != 0 &&
          root->ops->lookup(root, "dev", &out) == 0 && out == devdir);

    check("la radice non contiene altro",
          root->ops->lookup(root, "pippo", &out) == -1);

    /* Nessun inode vale zero: e' la lezione di procfs, e lo zero significa
       "nessun inode" — il valore con cui una voce di directory minix dice
       "cancellata". */
    check("radice e devdir hanno ino non nullo",
          root->ino != 0 && devdir->ino != 0);
}

/* LA lezione di M11a: lookup rende un puntatore che deve sopravvivere alla
   chiamata, e due lookup dello stesso file devono rendere lo STESSO puntatore.
   Altrimenti due size possono divergere, e in M16 due refcount. */
static void test_lookup_e_idempotente(void)
{
    struct inode *devdir, *a, *b;

    dev_init();
    chardev_register("uno", 5, 1, &uno);
    devfs_init();

    devdir = devfs_devdir();

    check("lookup trova il dispositivo",
          devdir->ops->lookup(devdir, "uno", &a) == 0);
    check("un secondo lookup rende lo STESSO puntatore",
          devdir->ops->lookup(devdir, "uno", &b) == 0 && a == b);

    check("l'inode e' un dispositivo a caratteri", a->type == INODE_CHARDEV);
    check("major e minor arrivano dalla voce del registry",
          a->major == 5 && a->minor == 1);
    check("priv punta all'impl", a->priv == &uno);
    check("ops e' stato riempito", a->ops != 0 && a->ops->read != 0);
    check("ino non e' zero", a->ino != 0);

    check("lookup su un nome assente rende -1",
          devdir->ops->lookup(devdir, "pippo", &b) == -1);
}

/* IL test che giustifica il lazy init, e che il devfs di M11d non passava.

   Fino a M11d devfs_init leggeva il registry e fotografava cio' che c'era:
   un driver che si iscriveva DOPO non compariva in /dev, e non c'era nessun
   errore da nessuna parte. */
static void test_iscrizione_dopo_devfs_init(void)
{
    struct inode *devdir, *out;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;

    dev_init();
    chardev_register("uno", 5, 1, &uno);
    devfs_init();

    /* DOPO devfs_init. */
    check("l'iscrizione tardiva riesce",
          chardev_register("due", 5, 2, &due) == 0);

    devdir = devfs_devdir();

    check("il dispositivo iscritto dopo devfs_init si trova con lookup",
          devdir->ops->lookup(devdir, "due", &out) == 0 &&
          out->priv == &due);

    check("e compare anche in readdir",
          devdir->ops->readdir(devdir, 1, nome, &ino) == 1 &&
          same(nome, "due"));
}

static void test_readdir(void)
{
    struct inode *devdir;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;

    dev_init();
    chardev_register("uno", 5, 1, &uno);
    chardev_register("due", 5, 2, &due);
    devfs_init();

    devdir = devfs_devdir();

    check("readdir rende la prima voce",
          devdir->ops->readdir(devdir, 0, nome, &ino) == 1 &&
          same(nome, "uno"));
    check("readdir rende la seconda",
          devdir->ops->readdir(devdir, 1, nome, &ino) == 1 &&
          same(nome, "due"));

    /* Zero e -1 sono distinti apposta: zero e' "le voci sono finite", -1 e' "la
       domanda non aveva senso". Un ciclo che si fermasse su entrambi sembrerebbe
       funzionare fino al giorno in cui readdir comincia a fallire davvero. */
    check("readdir oltre l'ultima rende 0",
          devdir->ops->readdir(devdir, 2, nome, &ino) == 0);
    check("readdir con un indice negativo rende 0 o -1, non 1",
          devdir->ops->readdir(devdir, -1, nome, &ino) <= 0);

    /* I numeri di readdir devono essere quelli degli inode che lookup rende:
       se divergessero, ls mostrerebbe numeri che nessun open ritrova. */
    {
        struct inode *out;

        devdir->ops->readdir(devdir, 0, nome, &ino);
        devdir->ops->lookup(devdir, "uno", &out);
        check("il numero di readdir e' quello dell'inode di lookup",
              ino == out->ino);
    }
}

/* /dev VUOTA e' uno stato legittimo, e nel kernel esiste: i self-check girano
   dopo i driver, ma un boot senza dischi ne ha tre invece di cinque. */
static void test_registry_vuoto(void)
{
    struct inode *devdir, *out;
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;

    dev_init();
    devfs_init();

    devdir = devfs_devdir();

    check("con il registry vuoto /dev non ha voci",
          devdir->ops->readdir(devdir, 0, nome, &ino) == 0);
    check("e lookup non trova niente",
          devdir->ops->lookup(devdir, "uno", &out) == -1);
}

int main(void)
{
    test_radice_e_devdir();
    test_lookup_e_idempotente();
    test_iscrizione_dopo_devfs_init();
    test_readdir();
    test_registry_vuoto();

    if (failures == 0) {
        printf("tutti i test dell'albero di devfs passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
```

- [ ] **Step 3: la regola nel `tests/host/Makefile`** (Claude)

```make
# devfs legge il registry e riempie inode, e devio.c e' cio' che sa come si
# riempiono: si linkano tutti e tre veri, perche' e' logica pura. vfs.c NON
# serve — questo test chiama le inode_ops direttamente, che e' il modo di
# provare l'albero senza dipendere dal risolutore.
test_devfs: test_devfs.c ../../kernel/devfs.c ../../kernel/devio.c \
            ../../kernel/dev.c ../../kernel/memory.c
	$(CC) $(CFLAGS) -o $@ $^
```

e `test_devfs` in `BINS`.

- [ ] **Step 4: verificare che fallisca** (Claude)

```bash
make -C tests/host test_devfs 2>&1 | tail -5
```

Atteso: link fallito su `devio_fill_inode` — non esiste ancora nemmeno nella
versione a solo `DEV_CHAR`. È il primo lavoro dello Step 5.

- [ ] **Step 5: `devio_fill_inode`, ramo `DEV_CHAR` soltanto** (WALTER)

Contratto: `include/devio.h`, Task 2 Step 3. Su `DEV_BLOCK` ritorna **-1** in
questo task: il ramo arriva nel Task 4, e anticiparlo rende ambigua la diagnosi
se qualcosa va storto.

Riempie `type`, `ops` (la vtable `static` dei chardev), `priv`, `major`, `minor`,
e `size = 0`. **Non** `ino`.

- [ ] **Step 6: riscrivere `kernel/devfs.c`** (WALTER)

Test-bersaglio: `test_devfs.c`, Step 2 — 24 controlli.

Struttura: `ino_root`, `ino_dev`, e `struct inode dev_inodes[DEV_MAX]`
indicizzato 1:1 col registry. Radice 1, `/dev` 2, dispositivo *i* → `3 + i`.

`devfs_init` riempie **solo** radice e `/dev`, e non tocca il registry.

Le trappole, con nome:

1. **`ino` si scrive per ULTIMO nel lazy init**, ed è la trappola vera di questo
   task. Due task prelazionati cento volte al secondo possono entrare su lo
   stesso slot: con `ino` per ultimo la corsa è benigna — valori identici, e chi
   arriva secondo vede ancora zero e rifà il lavoro. Con `ino` **prima**, il
   secondo vede `ino != 0` e riceve un inode con `ops` ancora nullo, cioè un
   salto attraverso un puntatore nullo al primo `read`. **Nessuna sezione critica
   serve**, ed è la disciplina del ring buffer di M5: la struttura sostituisce il
   `cli`. Non aggiungerne una credendo di sistemare qualcosa.
2. **`ino == 0` è il marcatore di «slot non inizializzato»**, quindi non serve un
   flag per slot. Ragionamento di `struct file` senza flag di occupazione.
3. **`devfs_readdir` legge il registry VIVO**, non una fotografia. `idx < 0` o
   `idx >= dev_count()` → 0.
4. **`readdir` copia al massimo `VFS_NAME_MAX` byte e termina.** `DEV_NAME_MAX`
   è 16 e `VFS_NAME_MAX` è 14: un nome di 15 caratteri nel registry non entra in
   un nome di directory, e va troncato o rifiutato — **non** copiato oltre il
   buffer del chiamante, che vuole `VFS_NAME_MAX + 1` byte.
5. **Il numero che `readdir` rende deve essere quello che `lookup` mette in
   `ino`.** Se divergessero, `ls` mostrerebbe numeri che nessun `open` ritrova.
   Un controllo del test lo verifica.
6. **`root_lookup` rende -1 quando non trova**, non 1. È il primo dei tre bug di
   M9b: `vfs_resolve` controlla `< 0`, quindi con 1 crede di aver trovato e
   cammina su un puntatore mai inizializzato — e il sintomo è diverso a ogni
   boot.
7. **`devfs_root()` NON si elimina.** È la radice di ripiego di `main.c:130`,
   quando non c'è minix su hdb.
8. **`devfs_init` deve AZZERARE `dev_inodes[]`**, ed è la trappola che si scopre
   solo eseguendo il test. Nel kernel `devfs_init` è chiamata una volta sola,
   quindi ometterlo non rompe niente — è esattamente la nota di `device_init` in
   M8: «dimenticare questa chiamata non rompe niente OGGI». Ma `test_devfs.c` la
   chiama cinque volte, e senza l'azzeramento uno slot con `ino != 0` rimasto dal
   test precedente fa saltare il lazy init: `lookup` consegna l'inode vecchio, con
   il `priv` che punta al dispositivo di prima. Oggi i test passerebbero comunque
   perché registrano gli stessi nomi negli stessi indici — cioè è un guasto che
   aspetta il primo test scritto in un ordine diverso.

   È anche la convenzione del progetto: **ogni `*_init()` stabilisce uno stato
   noto**, non «aggiunge a quello che c'era».

Verifica:

```bash
make -C tests/host -s test_devfs && ./tests/host/test_devfs | tail -3
```

- [ ] **Step 7: il self-check di ordine** (Claude)

In `selftest.c`, il controllo che è il migliore possibile sul lazy init — i
self-check girano **dopo** i driver, quindi `/dev` deve essere piena mentre
`devfs_init` non ha letto niente:

```c
/* Un devfs_init che si fosse memorizzato il registry passerebbe ogni altro
   controllo e cadrebbe su questo — o meglio, cadrebbe sul test host che iscrive
   un dispositivo DOPO devfs_init, che qui non si puo' costruire perche' i driver
   veri si iscrivono tutti prima. Qui si verifica la meta' osservabile: il
   conteggio di /dev coincide con quello del registry, letti in due modi diversi. */
static void check_devfs_conta_come_il_registry(void)
{
    struct inode *devdir = devfs_devdir();
    char nome[VFS_NAME_MAX + 1];
    uint32_t ino;
    int n = 0;

    while (devdir->ops->readdir(devdir, n, nome, &ino) == 1)
        n++;

    report("le voci di /dev sono quante le voci del registry",
           n == dev_count());
}
```

- [ ] **Step 8: `make test` e il controllo a mano** (Claude)

```bash
make test 2>&1 | tail -20
git diff --stat HEAD~2 -- kernel/vfs.c kernel/minixfs.c kernel/procfs.c
```

Poi `make run`, e al prompt: `ls /dev` con cinque voci, `cat /dev/kbd` che
aspetta l'input e si chiude su Invio, `ls /dev/hda` che dice ancora `file 0 byte`
— l'adapter è il Task 4.

- [ ] **Step 9: commit**

```bash
git add -A
git commit -m "M11e/3: devfs e' solo un albero, e inizializza lazy

Gli inode del pool si riempiono al primo lookup invece di essere
fotografati da devfs_init, quindi un driver che si iscrive dopo compare
in /dev — cosa che fino a M11d si perdeva in silenzio, senza nessun
errore da nessuna parte.

ino si scrive per ULTIMO: due task prelazionati sullo stesso slot
scrivono valori identici e la corsa e' benigna, mentre con ino scritto
prima il secondo riceve un inode con ops ancora nullo. Nessuna sezione
critica serve, ed e' la disciplina del ring buffer di M5.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: L'adapter a blocchi

**Autore:** Claude il test, il self-check, `shell.sh`. **Walter**
`kernel/devio.c` (il ramo a blocchi) e `kernel/shell.c`.

**Files:**
- Create: `tests/host/test_devio.c`
- Modify: `kernel/devio.c`, `kernel/shell.c`, `kernel/selftest.c`,
  `tests/shell.sh`, `tests/host/Makefile`

**Interfaces:**
- Consumes: tutto dai Task 2 e 3.
- Produces: `blk_inode_ops` (`static`, raggiungibile solo attraverso
  `devio_fill_inode`), e il ramo `DEV_BLOCK` di `devio_fill_inode` che pone
  `type = INODE_BLOCKDEV` e `size = b->nsectors * SECTOR_SIZE`.

- [ ] **Step 1: scrivere il test dell'aritmetica** (Claude)

`tests/host/test_devio.c`. Il disco finto è **in RAM e non su file**, e la
ragione è che l'aritmetica va provata anche sui **fallimenti**: iniettare un
errore al secondo settore è cosa che `fread` non sa fare.

```c
/* Test dell'adapter byte<->LBA, col gcc dell'host.

   E' il cuore di M11e: clamping, bounce buffer, letture a cavallo di piu'
   settori, trasferimenti parziali, read-modify-write. Tutto il resto della
   milestone e' mappatura e rename.

   Il disco finto sta in RAM e non su file, a differenza di test_minixfs: qui
   serve iniettare errori a un settore preciso, e fread non sa fallire su
   richiesta. Il pattern e' lo stesso generatore di tools/mkdisk.sh —
   byte[i] = (i * 7 + 3) & 0xFF — perche' e' deterministico e NON costante: un
   memset di un valore solo passerebbe anche con un adapter che legge sempre lo
   stesso settore, o che riempie mezzo buffer e lascia il resto a caso.

   Le vtable sono static dentro devio.c, quindi l'unica strada per arrivarci e'
   devio_fill_inode — ed e' un vantaggio: il test esercita il percorso vero. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "dev.h"
#include "blockdev.h"
#include "devio.h"
#include "vfs.h"

#define NSECT 4
#define DISCO (NSECT * SECTOR_SIZE)

static int failures;

static void check(const char *name, int ok)
{
    printf(ok ? "ok   -- %s\n" : "FAIL -- %s\n", name);
    if (!ok)
        failures++;
}

static uint8_t disco[DISCO];

/* -1 = mai. Altrimenti l'LBA da cui in poi read e write falliscono. */
static int fallisci_da;

/* Quante chiamate ha ricevuto il driver. E' il controllo che dice che l'adapter
   va un settore per volta, cioe' che la cache di M12 avra' un solo punto in cui
   infilarsi. */
static int chiamate;

static uint8_t atteso(uint32_t i)
{
    return (uint8_t)((i * 7 + 3) & 0xFF);
}

static void riempi_disco(void)
{
    uint32_t i;

    for (i = 0; i < DISCO; i++)
        disco[i] = atteso(i);

    fallisci_da = -1;
    chiamate    = 0;
}

static int finto_read(struct blockdev *b, uint32_t lba, void *buf,
                      uint32_t count)
{
    uint8_t *d = (uint8_t *)buf;
    uint32_t i;

    (void)b;
    chiamate++;

    if (fallisci_da >= 0 && lba >= (uint32_t)fallisci_da)
        return -1;

    /* Per sottrazione, non con lba + count: e' la regola di M10, e vale anche
       in un driver finto. */
    if (lba >= NSECT || count > (uint32_t)NSECT - lba)
        return -1;

    for (i = 0; i < count * SECTOR_SIZE; i++)
        d[i] = disco[lba * SECTOR_SIZE + i];

    return (int)count;
}

static int finto_write(struct blockdev *b, uint32_t lba, const void *buf,
                       uint32_t count)
{
    const uint8_t *s = (const uint8_t *)buf;
    uint32_t i;

    (void)b;
    chiamate++;

    if (fallisci_da >= 0 && lba >= (uint32_t)fallisci_da)
        return -1;

    if (lba >= NSECT || count > (uint32_t)NSECT - lba)
        return -1;

    for (i = 0; i < count * SECTOR_SIZE; i++)
        disco[lba * SECTOR_SIZE + i] = s[i];

    return (int)count;
}

static struct blockdev bd_rw = {
    .name = "hda", .nsectors = NSECT,
    .read = finto_read, .write = finto_write
};

static struct blockdev bd_ro = {
    .name = "hdb", .nsectors = NSECT,
    .read = finto_read            /* .write resta 0: disco read-only LEGITTIMO */
};

static struct blockdev bd_muto = {
    .name = "hdc", .nsectors = NSECT
    /* ne' read ne' write: blockdev_register lo rifiuta, ma l'adapter deve
       comunque non saltare se ci arriva */
};

/* Costruisce l'inode passando da devio_fill_inode, che e' l'unica strada verso
   la vtable static. */
static int prepara(struct inode *in, struct blockdev *b)
{
    struct dev_entry e;
    int i;

    for (i = 0; i < DEV_NAME_MAX; i++)
        e.name[i] = '\0';
    e.name[0] = 'h'; e.name[1] = 'd'; e.name[2] = 'a';

    e.kind  = DEV_BLOCK;
    e.major = 3;
    e.minor = 0;
    e.impl  = b;

    return devio_fill_inode(&e, in);
}

/* ---- la vista a file ----------------------------------------------------- */

static void test_fill_inode(void)
{
    struct inode in;

    riempi_disco();
    check("devio_fill_inode riesce su un blockdev", prepara(&in, &bd_rw) == 0);
    check("il tipo e' INODE_BLOCKDEV", in.type == INODE_BLOCKDEV);
    check("size e' nsectors * SECTOR_SIZE", in.size == DISCO);
    check("priv punta al blockdev", in.priv == &bd_rw);
    check("major e minor arrivano dalla voce", in.major == 3 && in.minor == 0);
    check("read e write ci sono",
          in.ops != 0 && in.ops->read != 0 && in.ops->write != 0);

    /* Su -1 *in NON viene toccato, che e' la convenzione di lookup e create. */
    {
        struct dev_entry e;
        struct inode intatto;
        int i;

        for (i = 0; i < DEV_NAME_MAX; i++) e.name[i] = '\0';
        e.name[0] = 'x';
        e.kind = DEV_NONE; e.major = 0; e.minor = 0; e.impl = &bd_rw;

        intatto.type = INODE_FILE;
        check("su kind invalido rende -1", devio_fill_inode(&e, &intatto) == -1);
        check("e non ha toccato *in", intatto.type == INODE_FILE);
    }
}

/* ---- lettura ------------------------------------------------------------- */

static int contenuto_giusto(const uint8_t *buf, uint32_t off, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (buf[i] != atteso(off + i))
            return 0;
    }

    return 1;
}

static void test_lettura_allineata(void)
{
    struct inode in;
    uint8_t buf[64];

    riempi_disco();
    prepara(&in, &bd_rw);

    check("read di 16 byte dall'inizio rende 16",
          in.ops->read(&in, 0, buf, 16) == 16);
    check("e il contenuto e' quello del disco", contenuto_giusto(buf, 0, 16));
    check("ed e' bastato UN settore", chiamate == 1);
}

static void test_lettura_non_allineata(void)
{
    struct inode in;
    uint8_t buf[64];

    riempi_disco();
    prepara(&in, &bd_rw);

    /* skip = 5: il bounce buffer serve proprio a questo. */
    check("read di 10 byte dall'offset 5 rende 10",
          in.ops->read(&in, 5, buf, 10) == 10);
    check("e parte dal byte 5, non dallo 0", contenuto_giusto(buf, 5, 10));
    check("un solo settore letto", chiamate == 1);
}

/* IL caso difficile: 500 + 100 attraversa il confine fra il settore 0 e l'1.
   Esercita in un colpo l'offset non allineato, la doppia iterazione, e lo skip
   che torna a zero al secondo giro. */
static void test_lettura_a_cavallo(void)
{
    struct inode in;
    uint8_t buf[128];

    riempi_disco();
    prepara(&in, &bd_rw);

    check("read di 100 byte dall'offset 500 rende 100",
          in.ops->read(&in, 500, buf, 100) == 100);
    check("il contenuto e' continuo attraverso il confine",
          contenuto_giusto(buf, 500, 100));
    check("sono servite DUE letture di settore", chiamate == 2);
}

static void test_lettura_su_tre_settori(void)
{
    struct inode in;
    static uint8_t buf[2048];

    riempi_disco();
    prepara(&in, &bd_rw);

    /* 100 .. 1599: settori 0, 1, 2 e 3. */
    check("read di 1500 byte dall'offset 100 rende 1500",
          in.ops->read(&in, 100, buf, 1500) == 1500);
    check("il contenuto e' continuo su quattro settori",
          contenuto_giusto(buf, 100, 1500));
    check("una lettura per settore, non una sola grande", chiamate == 4);
}

static void test_clamp_e_eof(void)
{
    struct inode in;
    uint8_t buf[128];

    riempi_disco();
    prepara(&in, &bd_rw);

    /* Il clamp: 2040 + 100 sfora, e si riducono a 8. */
    check("read che sfora la fine rende solo i byte che ci sono",
          in.ops->read(&in, DISCO - 8, buf, 100) == 8);
    check("e sono gli ultimi 8 del disco",
          contenuto_giusto(buf, DISCO - 8, 8));

    /* EOF VERO, e qui lo zero e' onesto: su un chardev significherebbe "adesso
       niente", qui significa "finito". E' la ragione per cui shell_cat non va
       toccato: il ramo file si ferma sullo zero. */
    check("read esattamente alla fine rende 0",
          in.ops->read(&in, DISCO, buf, 10) == 0);
    check("read oltre la fine rende 0",
          in.ops->read(&in, DISCO + 5000, buf, 10) == 0);

    /* off + n gira se si somma: off vicino a 2^32 piu' n positivo torna
       piccolo, e un adapter che confronta la somma crederebbe di essere dentro.
       Si sottrae. */
    check("un offset enorme rende 0 invece di girare",
          in.ops->read(&in, 0xFFFFFFF0u, buf, 64) == 0);

    check("n == 0 rende 0 senza toccare il disco",
          in.ops->read(&in, 0, buf, 0) == 0);
}

static void test_operazione_assente(void)
{
    struct inode in;
    uint8_t buf[64];

    riempi_disco();
    prepara(&in, &bd_muto);

    /* "Non supportata" rende -1 e NON 0. La convenzione del puntatore nullo
       descrive il DISPOSITIVO; il valore consegnato a chi ha chiesto di leggere
       deve essere -1, perche' uno zero direbbe EOF. E' il return 1 di
       chardev_read che in M9b faceva avanzare l'offset a cat su un buffer che
       nessuno aveva riempito. */
    check("read con b->read nullo rende -1",
          in.ops->read(&in, 0, buf, 16) == -1);
    check("write con b->write nullo rende -1",
          in.ops->write(&in, 0, buf, 16) == -1);

    /* E un disco read-only e' legittimo: si legge, non si scrive. */
    riempi_disco();
    prepara(&in, &bd_ro);
    check("un disco read-only si legge", in.ops->read(&in, 0, buf, 16) == 16);
    check("e non si scrive", in.ops->write(&in, 0, buf, 16) == -1);
}

/* I due rami dell'errore, e la distinzione e' la convenzione di read portata
   dentro: chi ha ricevuto 12 byte buoni deve saperlo, e dirgli -1 glieli fa
   buttare. Chi non ne ha ricevuto nessuno non ha nulla da salvare, e uno zero
   gli direbbe EOF. */
static void test_errore_a_meta(void)
{
    struct inode in;
    uint8_t buf[128];

    riempi_disco();
    prepara(&in, &bd_rw);
    fallisci_da = 0;

    check("errore al PRIMO settore rende -1",
          in.ops->read(&in, 0, buf, 64) == -1);

    riempi_disco();
    prepara(&in, &bd_rw);
    fallisci_da = 1;

    /* 500 + 100: i primi 12 byte vengono dal settore 0, che si legge; il resto
       dal settore 1, che fallisce. */
    check("errore al SECONDO settore rende i byte gia' copiati",
          in.ops->read(&in, 500, buf, 100) == 12);
    check("e quei byte sono giusti", contenuto_giusto(buf, 500, 12));
}

/* ---- scrittura ----------------------------------------------------------- */

/* LA trappola della scrittura: parziale vuole read-modify-write. Senza la
   lettura, i 502 byte intorno diventano spazzatura — e non spazzatura casuale,
   sono dati veri di qualcun altro, quindi hanno l'aria di essere giusti. E' la
   zona non azzerata di M11b. */
static void test_scrittura_parziale_preserva(void)
{
    struct inode in;
    uint8_t buf[8];
    int i;

    riempi_disco();
    prepara(&in, &bd_rw);

    for (i = 0; i < 5; i++)
        buf[i] = 0xAA;

    check("write di 5 byte all'offset 5 rende 5",
          in.ops->write(&in, 5, buf, 5) == 5);

    check("i 5 byte sono cambiati",
          disco[5] == 0xAA && disco[6] == 0xAA && disco[7] == 0xAA &&
          disco[8] == 0xAA && disco[9] == 0xAA);

    check("il byte PRIMA e' intatto", disco[4] == atteso(4));
    check("il byte DOPO e' intatto",  disco[10] == atteso(10));
    check("la fine del settore e' intatta",
          disco[SECTOR_SIZE - 1] == atteso(SECTOR_SIZE - 1));
    check("il settore successivo e' intatto",
          disco[SECTOR_SIZE] == atteso(SECTOR_SIZE));
}

static void test_scrittura_a_cavallo(void)
{
    struct inode in;
    static uint8_t buf[128];
    int i;

    riempi_disco();
    prepara(&in, &bd_rw);

    for (i = 0; i < 100; i++)
        buf[i] = 0x55;

    check("write di 100 byte dall'offset 500 rende 100",
          in.ops->write(&in, 500, buf, 100) == 100);
    check("gli ultimi byte del settore 0 sono cambiati",
          disco[500] == 0x55 && disco[SECTOR_SIZE - 1] == 0x55);
    check("i primi del settore 1 pure", disco[SECTOR_SIZE] == 0x55);
    check("il byte prima e' intatto", disco[499] == atteso(499));
    check("il byte dopo e' intatto",  disco[600] == atteso(600));
}

static void test_scrittura_clamp_e_eof(void)
{
    struct inode in;
    uint8_t buf[64];
    int i;

    riempi_disco();
    prepara(&in, &bd_rw);

    for (i = 0; i < 64; i++)
        buf[i] = 0x11;

    /* Un disco NON cresce. */
    check("write che sfora rende solo i byte che ci stanno",
          in.ops->write(&in, DISCO - 8, buf, 64) == 8);
    check("write esattamente alla fine rende 0",
          in.ops->write(&in, DISCO, buf, 8) == 0);
    check("write oltre la fine rende 0",
          in.ops->write(&in, DISCO + 5000, buf, 8) == 0);
}

/* ---- caps --------------------------------------------------------------- */

static void test_caps(void)
{
    struct dev_entry e;
    int i;

    for (i = 0; i < DEV_NAME_MAX; i++)
        e.name[i] = '\0';
    e.name[0] = 'h';

    e.kind = DEV_BLOCK; e.major = 3; e.minor = 0;

    e.impl = &bd_rw;
    check("un disco rw ha entrambe le capacita'",
          devio_caps(&e) == (DEVIO_CAN_READ | DEVIO_CAN_WRITE));

    e.impl = &bd_ro;
    check("un disco read-only ha solo la lettura",
          devio_caps(&e) == DEVIO_CAN_READ);

    e.kind = DEV_NONE;
    check("una voce invalida rende 0", devio_caps(&e) == 0);
}

int main(void)
{
    test_fill_inode();
    test_lettura_allineata();
    test_lettura_non_allineata();
    test_lettura_a_cavallo();
    test_lettura_su_tre_settori();
    test_clamp_e_eof();
    test_operazione_assente();
    test_errore_a_meta();
    test_scrittura_parziale_preserva();
    test_scrittura_a_cavallo();
    test_scrittura_clamp_e_eof();
    test_caps();

    if (failures == 0) {
        printf("tutti i test dell'adapter byte<->LBA passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
```

- [ ] **Step 2: la regola nel `tests/host/Makefile`** (Claude)

```make
# dev.c serve perche' devio.c ci si appoggia per i wrapper di registrazione,
# anche se questo test non registra niente: costruisce la dev_entry a mano e
# chiama devio_fill_inode, che e' l'unica strada verso la vtable static.
test_devio: test_devio.c ../../kernel/devio.c ../../kernel/dev.c \
            ../../kernel/memory.c
	$(CC) $(CFLAGS) -o $@ $^
```

e `test_devio` in `BINS`.

- [ ] **Step 3: verificare che fallisca per il motivo giusto** (Claude)

```bash
make -C tests/host -s test_devio && ./tests/host/test_devio 2>&1 | grep -c FAIL
```

Atteso: compila e linka (le funzioni esistono dal Task 3), e **fallisce sui
controlli a blocchi** — `devio_fill_inode` ritorna -1 su `DEV_BLOCK`. Se non
compila, l'header è sbagliato; se passa tutto, qualcuno ha anticipato il Task 4.

- [ ] **Step 4: il ramo `DEV_BLOCK` e `blk_inode_ops`** (WALTER)

Contratto: `include/devio.h`. Test-bersaglio: `test_devio.c`, ~45 controlli.

L'aritmetica, in ordine — è la sezione «`blk_inode_ops`, in lettura» dello spec:

```text
1.  off >= in->size          → 0            EOF VERO
2.  n > in->size - off       → n = in->size - off       clamp
      NON «off + n > in->size»: quella somma GIRA.
      Si SOTTRAE, ed e' lecito perche' il punto 1 garantisce off < in->size.
3.  b->read == 0             → -1
4.  finche' restano byte:
      lba   = off / SECTOR_SIZE
      skip  = off % SECTOR_SIZE
      chunk = min(SECTOR_SIZE - skip, restanti)
      b->read(b, lba, bounce, 1)        ← L'UNICA chiamata, e sempre count 1
      memcpy(dst, bounce + skip, chunk)
5.  errore con byte gia' copiati → rende i byte copiati
    errore al primo settore      → -1
```

Le trappole, con nome:

1. **Nessuna via rapida per le letture allineate.** `b->read` in **un punto
   solo**, sempre `count == 1`. È la riga in cui la cache di M12 si infila, e con
   due punti ce ne sarebbero due da sostituire. `test_lettura_su_tre_settori`
   verifica `chiamate == 4` proprio per questo.
2. **Il bounce buffer è LOCALE**, 512 byte sullo stack. Statico farebbe
   mescolare due letture prelazionate a metà — la lezione di procfs. Il costo è
   dichiarato: un ottavo dello stack di un task.
3. **`off + n` gira.** Si sottrae. `test_clamp_e_eof` ha il controllo con
   `0xFFFFFFF0`.
4. **In scrittura, parziale = read-modify-write.** Se `skip != 0` o
   `chunk != SECTOR_SIZE`, si legge il settore, si ritoccano `chunk` byte, si
   riscrive. Saltare la lettura distrugge i byte intorno, e sono dati veri di
   qualcun altro.
5. **Un disco non cresce.** `off >= size` → 0, clamp come in lettura.
6. **`size = nsectors * SECTOR_SIZE` gira a 4 GiB.** Non si sistema qui: vuole
   una dimensione a 64 bit in `struct inode`, che è M14. Va annotato nel commento
   e nell'elenco dei debiti.

Verifica:

```bash
make -C tests/host -s test_devio && ./tests/host/test_devio | tail -3
```

- [ ] **Step 5: `shell_ls` e `cat [n]`** (WALTER)

`shell_ls`, il terzo ramo — oggi `if (type == INODE_CHARDEV) … else "file %d
byte"`, quindi un blockdev si annuncia come file:

```text
  6 hda  blockdev 3:0  2048 byte
```

`shell_cat`, il limite opzionale. **La logica non si tocca**: `dispositivo =
(ino->type == INODE_CHARDEV)` resta, e un `INODE_BLOCKDEV` prende il ramo
«file», cioè si ferma sullo zero. Il criterio «`cat /dev/hda` termina all'EOF» si
soddisfa **non** modificando la condizione d'uscita — ed è la prova che il taglio
è nel posto giusto.

Si aggiunge: `argc == 3` → `n` massimo di byte, e il ciclo si ferma anche quando
li ha stampati. `argc > 3` → messaggio d'uso. Senza `n` il comportamento non
cambia di una virgola, quindi i test esistenti coprono già quel ramo.

Attenzione: il limite conta i byte **stampati**, non le chiamate a `read`. Con un
buffer da 64 e `n = 15` si legge una volta e si stampano 15.

- [ ] **Step 6: il self-check bidirezionale** (Claude)

In `selftest.c`. È il controllo che dà senso alla milestone: due strade allo
stesso dato, come l'orologio CMOS di M4.

```c
/* L'adapter contro il driver, e l'intervallo attraversa il confine di settore:
   500..599 prende gli ultimi 12 byte del settore 0 e i primi 88 dell'1, cioe'
   esercita l'offset non allineato, la doppia iterazione e lo skip che torna a
   zero al secondo giro.

   Non confronta contro un pattern atteso ma contro l'ALTRA strada: un disco non
   puo' verificare se stesso, e un adapter che sbagliasse l'aritmetica nello
   stesso modo in cui la sbaglia il test passerebbe qualunque controllo
   interno. */
static void check_blk_adapter_contro_lba(void)
{
    struct blockdev *b = dev_blockdev("hda");
    static uint8_t via_lba[2 * SECTOR_SIZE];
    uint8_t via_vfs[100];
    int fd, r, i, uguali;

    if (b == 0) {
        report("hda c'e' per il confronto bidirezionale", 0);
        return;
    }

    /* La strada bassa: due settori in LBA, ricuciti a mano. */
    if (b->read(b, 0, via_lba, 1) != 1 ||
        b->read(b, 1, via_lba + SECTOR_SIZE, 1) != 1) {
        report("i due settori si leggono in LBA", 0);
        return;
    }

    /* La strada alta: open, lseek, read. */
    fd = vfs_open("/dev/hda", O_RDONLY);
    report("/dev/hda si apre", fd >= 0);
    if (fd < 0)
        return;

    report("lseek a 500 riesce", vfs_lseek(fd, 500, SEEK_SET) == 500);

    r = vfs_read(fd, via_vfs, 100);
    report("read di 100 byte a cavallo del confine rende 100", r == 100);

    uguali = (r == 100);
    for (i = 0; i < r; i++) {
        if (via_vfs[i] != via_lba[500 + i])
            uguali = 0;
    }
    report("i byte del VFS coincidono con quelli letti in LBA", uguali);

    /* L'EOF, sull'altro estremo. */
    report("lseek a size - 10 riesce",
           vfs_lseek(fd, (int32_t)(b->nsectors * SECTOR_SIZE - 10), SEEK_SET)
               == (int)(b->nsectors * SECTOR_SIZE - 10));
    report("read di 100 in fondo rende 10", vfs_read(fd, via_vfs, 100) == 10);
    report("la read successiva rende 0, che qui e' EOF VERO",
           vfs_read(fd, via_vfs, 100) == 0);

    vfs_close(fd);
}

/* La dimensione, che e' l'altra meta' di cio' che rende il file un file. */
static void check_blk_size(void)
{
    struct inode *in;
    struct blockdev *b = dev_blockdev("hda");

    report("/dev/hda si risolve", vfs_resolve("/dev/hda", &in) == 0 && b != 0);
    if (b == 0)
        return;

    report("il tipo e' INODE_BLOCKDEV",
           vfs_resolve("/dev/hda", &in) == 0 && in->type == INODE_BLOCKDEV);
    report("size e' nsectors * 512", in->size == b->nsectors * SECTOR_SIZE);
    report("major e minor sono 3:0", in->major == 3 && in->minor == 0);
}
```

Registrarle in `selftest_run()` **dopo** i controlli di `devfs`, e chiamarle solo
se il mount di `/dev` è riuscito — senza, `vfs_open("/dev/hda")` fallisce per una
ragione che non c'entra con l'adapter.

- [ ] **Step 7: `tests/shell.sh`** (Claude)

`shell.sh` non ha helper `manda`/`attendi`: si digita con `sendkeys.py` un tasto
per argomento, e si verifica con `fra_prompt "<comando>"` che isola l'output di un
comando fra due prompt. Tre modifiche, e **due sono di una parola**.

Nella sezione dei tasti, dopo il `cat /dev/kbd` esistente:

```bash
# M11e: la vista a byte su un disco. La firma la scrive tools/mkdisk.sh nel
# settore 0, e il kernel la ritrova per una strada NUOVA — open, read, EOF —
# invece di rdsect. Un disco non puo' verificare se stesso.
#
# Quindici byte e non seicento: 600 attraverserebbe il confine di settore, ma
# stamperebbe 15 byte di firma, 497 NUL — il settore 0 e' azzerato dopo la firma
# — e poi 88 byte di binario. Illeggibile, e inutile come bersaglio di grep. Il
# confine lo prova check_blk_adapter_contro_lba, dove un confronto byte a byte si
# puo' fare davvero.
python3 tests/sendkeys.py "$MON" c a t spc slash d e v slash h d a spc 1 5 ret
```

Nel ciclo che verifica `ls /dev`, **una parola**:

```bash
for nome in console ttyS0 kbd hda hdb; do
```

È la forma migliore di verifica che ci sia: le tre righe di prima **non
cambiano** e continuano a passare, come in M11c quando `ls /` passò dal devfs al
disco senza che il test si accorgesse. Un test che non cambia mentre sotto cambia
tutto è la conferma che il taglio è nel punto giusto.

Nel ciclo di `devs`, anch'esso una riga:

```bash
for atteso in "console.*5:1" "ttyS0.*4:64" "kbd.*13:64" "hda.*3:0" "hdb.*3:64"; do
```

E l'asserzione nuova, accanto a quella di `cat /dev/kbd`:

```bash
# La catena: open → vfs_read → blk_inode_ops → bounce buffer → ata_read → il
# disco. E il numero 15 prova anche il limite di byte di cat, che senza questo
# comando non sarebbe esercitato da nessuna parte.
if fra_prompt "cat /dev/hda 15" | grep -q "waltex-disk-v1"; then
    echo "ok   -- cat /dev/hda legge il disco a BYTE e si ferma a 15"
else
    echo "FAIL -- cat /dev/hda non ha restituito la firma di mkdisk.sh"
    FALLITI=1
fi
```

- [ ] **Step 8: `make test` e le misure finali** (Claude)

```bash
make test 2>&1 | tail -20
```

Poi i tre controlli che chiudono la milestone:

```bash
# 1. il VFS non ha saputo niente di tutto questo
git diff --stat HEAD~3 -- kernel/vfs.c kernel/minixfs.c kernel/procfs.c

# 2. i conteggi, MISURATI
make -C tests/host -s run 2>/dev/null | grep -cE "ok +--"
bash tests/smoke.sh 2>&1 | grep -cE "selftest: ok"

# 3. nessun avviso di compilazione
make -s clean >/dev/null && make -s 2>&1 | grep -ci warn
```

Il primo **vuoto**, il terzo **zero**. I due conteggi si scrivono in `CLAUDE.md`.

- [ ] **Step 9: aggiornare `CLAUDE.md`** (Claude)

- «Stato corrente»: M11e chiusa, con la tabella delle milestone aggiornata e la
  riga `M11e refactor` fra M11d e M12.
- La sezione di M11e: la trappola di `impl` puntatore contro registro che
  copiava, `ino` scritto per ultimo, l'unica chiamata a `b->read`, il bug di
  `device_register` chiuso.
- «Debiti tecnici»: si aggiunge `size` a 32 bit sui dischi sopra 4 GiB, sotto
  «Mordono in M12-M13» no — sotto **M14**, con `struct stat`. E si aggiunge il
  bounce buffer sullo stack sotto «Mordono quando qualcuno tocca quel file».
- «Saldati»: la protezione del nome non terminato in `device_register`.
- I conteggi dei test, misurati allo Step 8.

- [ ] **Step 10: commit**

```bash
git add -A
git commit -m "M11e/4: l'adapter byte<->LBA, e hda si legge come un file

blk_inode_ops traduce offset in LBA con un bounce buffer da un settore:
clamp per sottrazione perche' off + n gira, EOF vero sullo zero, i byte
gia' copiati su un errore a meta', e read-modify-write in scrittura
parziale perche' altrimenti i 502 byte intorno diventano dati di
qualcun altro.

b->read viene chiamata in UN punto solo e sempre con count 1: e' la riga
in cui la buffer cache si infilera' in M12, e con una via rapida per le
letture allineate ce ne sarebbero due.

shell_cat non e' stato toccato nella logica — un INODE_BLOCKDEV prende
il ramo file e si ferma sullo zero — ed e' la prova che il taglio e' nel
posto giusto.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Self-review

**Copertura dello spec.** Ogni sezione ha un task:

| sezione dello spec | task |
|---|---|
| Il bug di `device_register` | 2, Step 1 (header) e 8 (`test_nome_al_limite`) |
| Scelta 1: registry senza I/O | 2 |
| Scelta 2: adapter in un file suo | 2 (wrapper), 4 (vtable) |
| Scelta 3: uno `switch` solo | 2 Step 10 (`caps`), 3 Step 5 e 4 Step 4 (`fill_inode`) |
| Scelta 4: un settore per volta | 4 Step 4, trappola 1, misurata da `chiamate` |
| Scelta 5: bounce locale | 4 Step 4, trappola 2 |
| Scelta 6: `dev_by_id` resta | 2 Step 1 e `test_by_id` |
| `include/dev.h`, i sei rifiuti | 2 Step 1, coperti uno a uno da `test_dev.c` |
| La trappola di `impl` puntatore | 2, riquadro + Step 11 + `test_impl_si_riferisce` |
| `chardev.h`, `blockdev.h`, `devio.h`, `vfs.h` | 2 Step 2-4, 3 Step 1 |
| L'albero, `ino` per ultimo | 3 Step 6, trappola 1 |
| `chr_inode_ops` | 3 Step 5 |
| `blk_inode_ops` lettura e scrittura | 4 Step 4 |
| Il limite a 4 GiB | 4 Step 4 trappola 6, Step 9 |
| `shell.c` | 2 Step 13, 4 Step 5 |
| `main.c` | 2 Step 14 |
| Verifica: 3 test host, self-check, `shell.sh` | 2 Step 5, 3 Step 2, 4 Step 1/6/7 |
| Ordine di lavoro a quattro passi | i quattro task |

**Nessun buco trovato.** Un'aggiunta rispetto allo spec: `devio_caps` è nel Task
2 e non nel 4, perché `shell_devs` lo usa già lì — senza, il Task 2 lascerebbe
`devs` senza la colonna per un commit.

**Coerenza dei tipi.** `dev_lookup_index` rende `int`, `dev_get`/`dev_by_id`
rendono `const struct dev_entry *`, `devio_fill_inode(const struct dev_entry *,
struct inode *)` rende `int`. Usati con queste firme nei Task 3 e 4.
`chardev_register(name, major, minor, c)` con quattro argomenti in `serial.c`,
`vga.c`, `keyboard.c` e in `test_devfs.c`; `blockdev_register` uguale in `ata.c`.
`SECTOR_SIZE` da `blockdev.h` in `test_devio.c` e nel self-check.

**Una cosa che il piano NON risolve e che va decisa eseguendo:** lo Step 8 del
Task 2 dice che `test_nome_al_limite` caso B «non distingue più i due controlli
come faceva in M8», perché con `dev_entry` il byte dopo `name` è `kind` e
azzerarlo significa `DEV_NONE`, che viene rifiutato comunque. Il test resta utile
— verifica l'ordine dei controlli — ma **ha perso potere diagnostico** rispetto
alla versione di M8. Se eseguendo si trova un modo di ricostruire il caso forte,
va fatto; altrimenti va annotato nel test, perché un controllo che sembra forte e
non lo è è peggio di uno assente.

---

## Handoff

Il piano non va a un subagente: i `.c` concettuali sono di Walter. La ripartizione
per task:

| task | Claude | Walter |
|---|---|---|
| 1 | tutto (rename meccanico) | — |
| 2 | `dev.h`, `chardev.h`, `devio.h`, `blockdev.h`, `test_dev.c`, Makefile, `serial.c`, `ata.c`, `main.c`, `selftest.c` | `dev.c`, i wrapper e i lookup e `caps` in `devio.c`, `vga.c`, `keyboard.c`, `shell.c` |
| 3 | `vfs.h`, `test_devfs.c`, Makefile, `selftest.c` | `devfs.c`, `devio_fill_inode` ramo char |
| 4 | `test_devio.c`, `selftest.c`, `shell.sh`, `CLAUDE.md` | `blk_inode_ops`, `fill_inode` ramo block, `shell.c` |

Ordine consigliato: Claude fa il Task 1 e gli step di header/test del Task 2,
Walter implementa, e ci si scambia il controllo a ogni «verifica» del piano. Il
Task 1 può partire subito — non contiene decisioni.
