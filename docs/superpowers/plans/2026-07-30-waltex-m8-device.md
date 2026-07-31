# M8 — Device layer: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** contengono
> interfaccia, test, concetti e tranelli, ma il codice lo scrive Walter
> (vedi `CLAUDE.md`). I task **[CLAUDE]** sono infrastruttura.
>
> **Companion:** `2026-07-30-waltex-m8-funzioni.md` descrive funzione per
> funzione cosa deve fare, cosa ritorna in ogni caso, cosa non deve fare e
> quale test la prende. Tienilo aperto mentre scrivi.

**Obiettivo:** la tastiera smette di essere un caso speciale. Un `struct device`
con puntatori a `read` e `write`, un registro a capacità fissa, e i tre driver
che esistono già — VGA, seriale, tastiera — che si iscrivono senza cambiare la
loro logica.

**Architettura:** un registro e tre adattatori. Il registro è `kernel/device.c`:
logica pura sopra un array statico, quindi interamente provabile sull'host.
Gli adattatori sono tre funzioni di poche righe, una per driver, che traducono
fra la firma uniforme del device layer e la funzione che il driver ha già.

Nessun hardware nuovo. Circa 150 righe.

**Perché è un perno e non un rifacimento.** Oggi `shell.c` chiama
`keyboard_getchar` e `kprintf` per nome: sa esattamente con cosa sta parlando.
Dopo M8 esiste un modo di dire «scrivi questi byte su quel dispositivo» senza
sapere quale sia — ed è il prerequisito di M9, dove `/dev/kbd` diventa un file e
`cat` funziona su di lui senza contenere un caso speciale.

`vga.c`, `serial.c` e `keyboard.c` **non cambiano la loro logica**: guadagnano un
adattatore e una riga di iscrizione. Il device layer è un registro, non una
riscrittura.

## Vincoli globali

Quelli dei blocchi precedenti, più:

- **Nessuna allocazione dinamica**: `MAX_DEVICES 16`, `DEV_NAME_MAX 16`, array
  statico. L'allocatore arriva in M12 e non prima.
- **Il registro copia, non punta.** Chi si iscrive può passare una struct sullo
  stack; il registro deve poter sopravvivere al chiamante.
- **Solo device a caratteri.** I dispositivi a blocchi arrivano in M10 con una
  interfaccia propria, perché la loro granularità è il settore e non il byte.
- **Un puntatore a operazione nullo significa «non supportata»**, non «errore».
  È la stessa convenzione di `exc_handlers[vec] == 0` in `idt.c`.
- **`errno` non esiste ancora**: si ritorna 0/-1 e puntatore/0. I codici veri
  arrivano in M14, e anticiparli violerebbe la disciplina delle milestone.
- **La regola del consumatore unico sopravvive.** `kbd_read` avvolge
  `keyboard_getchar`, che ammette un solo consumatore: dopo M8 quel consumatore
  è chiunque legga `/dev/kbd`, e deve restare uno.

## Struttura dei file al termine di M8

| File | Responsabilità | Chi |
|---|---|---|
| `include/device.h` | `struct device`, le sei funzioni del registro | CLAUDE |
| `kernel/device.c` | il registro: iscrizione e ricerca | **WALTER** |
| `kernel/serial.c` | adattatore + iscrizione — **l'esempio svolto** | CLAUDE |
| `kernel/vga.c` | adattatore + iscrizione | **WALTER** |
| `kernel/keyboard.c` | adattatore + iscrizione | **WALTER** |
| `kernel/shell.c` | il comando `devs` | **WALTER** |
| `kernel/main.c` | `device_init()` per primo | CLAUDE |
| `tests/host/test_device.c` | 29 controlli, senza QEMU | CLAUDE |
| `kernel/selftest.c` | 9 controlli sui tre device veri | CLAUDE |
| `tests/shell.sh` | un controllo su `devs` | CLAUDE |

**`serial.c` è l'esempio svolto, e non per caso:** è già un file mio, quindi lo
scrivo io per intero — adattatore e iscrizione — e tu fai gli altri due
guardando quello. Un driver fatto vale più di tre pagine di descrizione.

## L'interfaccia

```c
/* ---- include/device.h ---- */

#define MAX_DEVICES  16
#define DEV_NAME_MAX 16      /* NUL compreso: 15 caratteri utili */

struct device {
    char     name[DEV_NAME_MAX];    /* un ARRAY, non un puntatore */
    uint16_t major, minor;
    int (*read )(struct device *d, void *buf, uint32_t n);
    int (*write)(struct device *d, const void *buf, uint32_t n);
    void *priv;
};

void device_init(void);
int  device_register(const struct device *d);       /* 0 ok, -1 rifiutato */
struct device *device_find(const char *name);       /* 0 se non c'e' */
struct device *device_by_id(uint16_t major, uint16_t minor);
int device_count(void);
struct device *device_at(int i);                    /* 0 fuori intervallo */
```

Tre decisioni dentro quella struct, che vale la pena vedere prima di scrivere.

**`name` è un array e non un `const char *`.** Conseguenza diretta del vincolo
«il registro copia»: se fosse un puntatore, il registro conserverebbe l'indirizzo
di una stringa che appartiene a qualcun altro. L'array costringe a copiare, e
copiare è ciò che rende il registro indipendente da chi si iscrive.

**`read` e `write` prendono `struct device *` come primo argomento.** Non serve a
niente in M8 — gli adattatori lo ignorano — e serve a tutto in M10, quando il
driver ATA registrerà due dischi con la *stessa* funzione `read` e dovrà sapere
quale dei due sta leggendo. È la ragione per cui esiste anche `priv`.

**`priv` non è usato in M8.** È lo spazio dove un driver mette il proprio stato
per-dispositivo. Lo dichiariamo adesso perché sta nello spec approvato e perché
aggiungerlo dopo vorrebbe dire toccare la struct quando tre driver la usano già.

## I tre dispositivi, con numeri veri

| nome | major, minor | read | write | perché quei numeri |
|---|---|---|---|---|
| `console` | 5, 1 | — | VGA | è il numero di `/dev/console` su Linux |
| `ttyS0` | 4, 64 | — | COM1 | è il numero di `/dev/ttyS0` su Linux |
| `kbd` | 13, 64 | tastiera | — | sono i numeri di `/dev/input/event0` su Linux |

Verificati sul sistema che gira adesso, non ricordati:

```text
$ ls -l /dev/console /dev/ttyS0 /dev/input/event0
crw-------  5,   1  /dev/console
crw-rw----  4,  64  /dev/ttyS0
crw-rw----  13, 64  /dev/input/event0
```

Usare i numeri veri costa zero e resta coerente col vincolo POSIX dello spec:
lo stesso ragionamento per cui in M14 i numeri di syscall saranno quelli di
Linux i386.

Nota che `console` e `ttyS0` sono **due dispositivi distinti**, uno per lo
schermo e uno per la porta seriale, e nessuno dei due scrive in entrambi i posti.
Chi vuole entrambi scrive due volte — ed è esattamente ciò che `kprintf` fa già,
con i suoi due sink.

## Il contratto di `read`, che è la cosa più importante di M8

`read` ritorna **quanti byte ha copiato davvero**, e quel numero può essere
**zero**.

Zero significa «adesso non c'è niente», **non** «fine del file».

È il contratto che regge tutto il resto, per due ragioni:

- `kbd_read` non può bloccare: manca il blocking I/O, sta nello spec sotto
  «fuori scope», e il punto di decisione è M9. Una `read` che aspettasse di
  riempire il buffer sarebbe un ciclo di attesa dentro una funzione che ha
  promesso di tornare subito;
- in M9 `cat /dev/kbd` farà spin proprio su quello zero, e la differenza fra
  «niente adesso» e «finito» è la differenza fra un `cat` che aspetta e un `cat`
  che esce.

## Ordine di inizializzazione, e il tranello

`device_init()` va chiamata **prima** di tutte le `*_init()` dei driver, perché
sono i driver a iscriversi. In `kernel/main.c` oggi `vga_init()` è la prima riga
di `kmain`: `device_init()` le passa davanti.

Il tranello è che **funzionerebbe anche sbagliando**. Il registro tiene un
contatore `static`, che al boot è già zero perché sta in `.bss`: quindi
dimenticare `device_init()` non rompe niente *oggi*, e rompe tutto il giorno che
il registro guadagna un campo che non parte da zero. Un self-check verifica il
conteggio dopo il boot, e prende sia l'ordine sbagliato sia la chiamata
mancante.

## I task

### Task 1 [CLAUDE]: header, test host

Scrivo `include/device.h` come sopra, `tests/host/test_device.c` con 29
controlli, e la regola nel `tests/host/Makefile`.

Il kernel continua a compilare e ad avviarsi **invariato**: nessuno chiama ancora
niente. Il solo rosso è il test host, che non linka finché `device.c` non esiste.

I 29 controlli, e ognuno c'è per un modo preciso di sbagliare:

*Iscrizione*

- il primo `device_register` ritorna 0
- `device_count` passa da 0 a 1
- **il registro ha copiato, non puntato**: modificando la struct sorgente dopo
  l'iscrizione, il registro non cambia
- tre iscrizioni riescono, `device_count` dà 3
- l'iscrizione numero `MAX_DEVICES + 1` è rifiutata
- un'iscrizione rifiutata **non** incrementa il conteggio
- un nome duplicato è rifiutato
- un nome duplicato non sovrascrive quello esistente
- un nome di `DEV_NAME_MAX - 1` caratteri è accettato
- un nome di `DEV_NAME_MAX` caratteri è rifiutato, **non troncato**
- il nome accettato al limite è terminato da NUL
- un nome vuoto è rifiutato
- un device con `read` e `write` entrambi nulli è rifiutato

*Ricerca*

- `device_find` trova ognuno dei tre (tre controlli)
- `device_find` su un nome assente ritorna 0
- `device_find` **non** accetta un prefisso: `"cons"` non trova `"console"`
- `device_by_id` trova per coppia major/minor
- `device_by_id` distingue due device con lo **stesso major** e minor diverso
- `device_by_id` su una coppia assente ritorna 0

*Enumerazione*

- `device_at(0)` … `device_at(2)` restituiscono i tre in ordine di iscrizione
- `device_at(-1)` e `device_at(device_count())` ritornano 0

**Verifica:** `make` compila e il kernel si avvia con 43 self-check verdi;
`make -C tests/host test_device` non linka. Nessun commit: è un quinto di
milestone.

---

### Task 2 [WALTER]: `kernel/device.c`

Le sei funzioni del registro. Logica pura: nessun hardware, nessuna
allocazione, nessuna `kprintf`.

Il dettaglio di ognuna sta nel documento companion. Qui le due decisioni di
struttura da prendere prima di scrivere:

**Come si rappresenta uno slot libero.** In M8 non esiste `device_unregister`:
le iscrizioni sono solo in aggiunta, e nessuno si cancella. Quindi non serve un
flag per slot — basta un contatore di quanti sono iscritti, e gli slot da 0 a
`n-1` sono quelli validi. Un flag per slot sarebbe un secondo stato da tenere in
sincrono con il primo, cioè un modo in più di sbagliare.

**Dove si ferma la scansione.** A `device_count()`, non a `MAX_DEVICES`.
Scandire tutto l'array confronterebbe anche gli slot mai scritti: oggi sono
zeri, quindi non troverebbe niente per fortuna, ma è fortuna e non correttezza.

**Verifica:** `make -C tests/host test_device` e i 29 controlli passano.

---

### Task 3 [CLAUDE]: `serial.c` si iscrive — l'esempio svolto

Scrivo per intero `serial_dev_write` e l'iscrizione di `serial.c`, aggiungo
`device_init()` in cima a `kmain`, e i tre self-check sul dispositivo `ttyS0`.

Alla fine di questo task **un** dispositivo è iscritto, il kernel si avvia, e tu
hai davanti un driver completo da cui copiare la forma.

**Verifica:** `make test` verde, `device_count()` dà 1.

---

### Task 4 [WALTER]: `vga.c` e `keyboard.c` si iscrivono

Due adattatori e due iscrizioni, sulla forma di `serial.c`.

- `vga.c` guadagna `vga_dev_write`, che scrive `n` byte chiamando `vga_putc`, e
  l'iscrizione di `console` con `read` a **zero**;
- `keyboard.c` guadagna `kbd_dev_read`, che estrae **fino a** `n` byte con
  `keyboard_getchar`, e l'iscrizione di `kbd` con `write` a **zero**.

Entrambe `static`: nessuno le chiama per nome, vivono dentro la struct del
dispositivo. Le schede stanno nel documento companion, ai punti 8 e 9.

Nessuna delle due tocca la logica esistente: `vga_putc` e `keyboard_getchar`
restano come sono.

**Verifica:** `make test` verde, i 9 self-check sui device passano,
`device_count()` dà 3.

---

### Task 5 [WALTER]: il comando `devs`

Una voce in più nella tabella di `shell.c` — `shell_devs`, sulla forma delle
altre otto — in due modi d'uso.

Senza argomenti enumera con `device_count` e `device_at`, stampando nome,
`major:minor` e quali operazioni il dispositivo supporta — leggendo la capacità
dalla **nullità dei puntatori**, che è il guadagno visibile di quella scelta di
progetto.

Con un argomento mostra un solo dispositivo, con `device_find`. Non è
decorazione: è ciò che dà a `device_find` un chiamante vero in M8 invece di
farla aspettare fino a M9.

```text
waltex> devs
  console   5:1    -w
  ttyS0     4:64   -w
  kbd      13:64  r-
waltex> devs kbd
  kbd      13:64  r-
waltex> devs pippo
devs: pippo: nessun dispositivo con questo nome
```

Le prime tre righe sono quelle che `tests/shell.sh` cercherà.

**Verifica:** `make test` verde, e `devs` elenca tre dispositivi.

---

### Task 6 [CLAUDE]: chiusura

Aggiungo il controllo su `devs` a `tests/shell.sh`, aggiorno `README.md` e
`CLAUDE.md` con lo stato e i numeri, e **propongo** il commit —
`M8: device layer, i driver si iscrivono a un registro` — eseguendolo solo se
confermi.

## Come si sbaglia

**Conservare il puntatore invece di copiare.** Il modo classico, e il più
insidioso: funziona finché chi si è iscritto ha usato una `static`, e si rompe
il giorno che qualcuno passa una struct locale. Il test che lo prende modifica la
sorgente dopo l'iscrizione.

**Troncare un nome troppo lungo invece di rifiutarlo.** `console-primaria` e
`console-secondaria` troncate a 15 caratteri diventano la stessa cosa, e
`device_find` restituisce sempre la prima.

**Scandire fino a `MAX_DEVICES`.** Confronta anche slot mai scritti. Oggi
innocuo, domani no.

**Incrementare il contatore anche quando si rifiuta.** Lo slot resta vuoto e
`device_at` restituisce spazzatura.

**`kbd_read` che cicla finché non ha `n` byte.** Diventa un'attesa attiva dentro
una funzione che ha promesso di non aspettare, e in M9 `cat /dev/kbd` non
tornerebbe mai.

**Indicizzare un `void *`.** `buf` è `const void *`: `buf[i]` non compila, e
`(char *)buf + i` non è la stessa cosa di `(char *)(buf + i)`. È l'errore di M1
con `(void *)VGA_MEM + VGA_COLS`, che avanzava di 80 **byte** invece di 80 celle.

**Chiamare `kbd_read` da un self-check dopo che la shell è partita.** Sarebbero
due consumatori dello stesso ring buffer. I self-check girano prima di
`task_create(shell_task)`, quindi sono al sicuro — ma è al sicuro per l'ordine
delle righe in `kmain`, non per costruzione.

**Un adattatore che interpreta i byte.** `console_write` non deve tradurre `\n`,
filtrare i non stampabili o fare altro: scrive quello che riceve. L'interpretazione
è di `vga_putc`, un livello sotto, e duplicarla qui la farebbe accadere due volte.

## Lettura di accompagnamento

`include/linux/fs.h` di Linux 0.01 per vedere `struct file_operations`: è la
stessa idea — una struct di puntatori a funzione che rende i dispositivi
interscambiabili — con vent'anni di meno addosso.

E **xv6**, `file.c` e `devsw` in `console.c`: la sua tabella `devsw[]` indicizzata
per major è esattamente il nostro `device_by_id`, in venti righe.
