# M8 — Cosa deve fare ogni funzione

Documento companion di `2026-07-30-waltex-m8-device.md`. Il piano dice cosa
costruire e in che ordine; questo dice, per ogni singola funzione, che lavoro fa.

**Come si legge una scheda.** Sei voci, sempre le stesse:

| voce | a cosa risponde |
|---|---|
| **Compito** | cosa deve fare, in una frase |
| **Ritorna** | ogni caso, nessuno lasciato implicito |
| **Non deve** | cosa è tentante fare e va evitato |
| **Chi la chiama** | adesso, e in quale milestone futura — è il «dove stiamo andando» |
| **Come si sbaglia** | i modi concreti, non «attento agli errori» |
| **Test** | quale controllo la prende quando è sbagliata |

La voce **Chi la chiama** è quella che vale di più: una funzione senza chiamante
è una funzione di cui non si può giudicare il progetto.

---

## Indice

**`kernel/device.c`** — il registro, tutto logica pura

1. [`device_init`](#1-device_init)
2. [`device_register`](#2-device_register)
3. [`device_find`](#3-device_find)
4. [`device_by_id`](#4-device_by_id)
5. [`device_count`](#5-device_count)
6. [`device_at`](#6-device_at)

**gli adattatori dei driver** — poche righe ciascuno

7. [`serial_dev_write`](#7-serial_dev_write) — **la scrivo io, è l'esempio**
8. [`vga_dev_write`](#8-vga_dev_write)
9. [`kbd_dev_read`](#9-kbd_dev_read)
10. [l'iscrizione dentro `*_init`](#10-liscrizione-dentro-_init)

**la shell**

11. [`shell_devs`](#11-shell_devs)

---

## Lo stato del registro

Prima delle schede, perché sei funzioni su sei lo toccano.

Il registro è **un array e un contatore**:

```text
static struct device devs[MAX_DEVICES];
static int ndev;                          /* quanti iscritti */
```

Gli slot da `0` a `ndev-1` sono validi; da `ndev` in su non sono mai stati
scritti. Non serve un flag «libero/occupato» per slot, e non averlo è una
scelta: in M8 non esiste `device_unregister`, le iscrizioni sono solo in
aggiunta, e un flag sarebbe un secondo stato da tenere in sincrono con `ndev` —
cioè un modo in più di sbagliare.

È lo stesso ragionamento del ring buffer di M5, dove non c'è un contatore degli
elementi: meno stato duplicato, meno modi di divergere.

---

## 1. `device_init`

```c
void device_init(void);
```

**Compito.** Portare il registro allo stato «nessun dispositivo iscritto».
Con la rappresentazione scelta è una riga: `ndev = 0`.

**Ritorna.** Niente.

**Non deve.** Toccare hardware — non ne ha. Non deve azzerare l'array `devs`:
gli slot oltre `ndev` non vengono mai letti, quindi pulirli è lavoro inutile su
16 × 28 byte.

**Chi la chiama.** `kmain`, come **prima** riga, prima di `vga_init()`. Deve
stare davanti a tutte le `*_init()` dei driver perché sono loro a iscriversi.

**Come si sbaglia.** In un modo solo, ed è insidioso: **dimenticandola, oppure
chiamandola dopo i driver, e non accorgendosene.** `ndev` è `static`, quindi sta
in `.bss` e al boot vale già zero: il registro funziona *per fortuna* anche senza
`device_init`. La fortuna finisce il giorno che il registro guadagna un campo che
non parte da zero.

Chiamarla *dopo* i driver è peggio, perché azzera `ndev` e i dispositivi già
iscritti diventano invisibili pur essendo nell'array.

**Test.** Self-check in VM: dopo il boot `device_count()` deve dare **3**.
Prende la chiamata mancante solo indirettamente, l'ordine sbagliato subito.

---

## 2. `device_register`

```c
int device_register(const struct device *d);
```

La funzione più densa di M8. Tredici dei ventuno controlli host guardano lei.

**Compito.** Copiare il descrittore del chiamante nel primo slot libero, dopo
aver verificato che sia accettabile.

**Copiare, non puntare.** È il punto centrale. Chi si iscrive può passare una
struct che vive sul suo stack, e quando la sua funzione ritorna quella memoria
non è più sua. Il registro deve possedere i propri dati.

È anche la ragione per cui `name` nella struct è un **array** e non un
`const char *`: se fosse un puntatore, copiare la struct copierebbe l'indirizzo
di una stringa che appartiene a qualcun altro, e il problema si spostherebbe di
un livello senza risolversi.

**Ritorna.**

| caso | valore |
|---|---|
| iscritto | `0` |
| registro pieno (`ndev == MAX_DEVICES`) | `-1` |
| nome più lungo di `DEV_NAME_MAX - 1` | `-1` |
| nome vuoto | `-1` |
| nome già iscritto | `-1` |
| sia `read` sia `write` nulli | `-1` |

**Perché rifiutare e non aggiustare**, caso per caso:

- **nome troppo lungo → rifiuta, non troncare.** `console-primaria` e
  `console-secondaria` troncate a 15 caratteri diventano la stessa stringa, e
  `device_find` restituirebbe sempre la prima. Un troncamento silenzioso
  trasforma un errore del driver in un bug di ricerca, che si manifesta
  altrove.
- **nome vuoto → rifiuta.** `device_find("")` non ha significato, e nel `/dev` di
  M9 diventerebbe un file senza nome.
- **duplicato → rifiuta.** Con due `console` la ricerca è ambigua e vince la
  prima in silenzio.
- **entrambe le operazioni nulle → rifiuta.** Un dispositivo che non sa né
  leggere né scrivere non è utilizzabile: iscriverlo nasconde un driver che ha
  dimenticato di riempire la struct.

**Non deve.** Allocare niente. Toccare hardware. Modificare `*d` — è `const`, e
il `const` è una promessa al chiamante, non un ornamento.

**Chi la chiama.** Adesso: `serial_init`, `vga_init`, `keyboard_init`, una volta
ciascuna. In **M10**: `ata_init`, una volta per disco trovato — ed è lì che il
copiare conta davvero, perché due dischi si iscriveranno con la *stessa* funzione
`read` distinguendosi per `priv`.

**Come si sbaglia.**

- **conservare il puntatore** invece di copiare byte per byte. Funziona finché
  tutti si iscrivono con una `static`, e si rompe al primo che usa una locale;
- **copiare il nome senza limite**, sforando `DEV_NAME_MAX`. Il nome è il primo
  campo della struct, quindi sforare scrive sopra `major` — la stessa geometria
  per cui in M7 `buf[128]` cadeva dentro `len`;
- **copiare il nome senza il terminatore**, e allora `device_find` legge oltre;
- **verificare il duplicato dopo aver scritto lo slot**: il controllo trova se
  stesso e rifiuta sempre;
- **incrementare `ndev` anche quando si rifiuta**: lo slot resta vuoto e
  `device_at` restituisce spazzatura;
- **contare la lunghezza del nome con `strlen` su una stringa non terminata**.
  Se il chiamante passa un nome non terminato, `strlen` cammina fuori. Fidarsi
  è ragionevole — i chiamanti sono tre e sono nostri — ma va saputo.

**Test.** `test_device.c`, tredici controlli. Quello che prende il bug del
puntatore merita una nota: iscrive un dispositivo, **modifica la struct
sorgente**, e poi verifica che il registro non sia cambiato. È l'unico modo di
distinguere una copia da un alias dall'esterno.

---

## 3. `device_find`

```c
struct device *device_find(const char *name);
```

**Compito.** Scorrere i dispositivi iscritti e restituire quello il cui nome
coincide.

**Ritorna.**

| caso | valore |
|---|---|
| trovato | puntatore allo slot nel registro |
| non trovato | `0` |

Il puntatore è allo slot **dentro** l'array, non a una copia: il chiamante può
usare `d->write(d, ...)` e il `d` che passa è quello giusto.

**Non deve.** Accettare un prefisso. La corrispondenza è esatta, quindi serve
`strcmp` e non un confronto sui primi N caratteri. `cons` non deve trovare
`console`.

**Non deve** scorrere oltre `ndev`.

**Chi la chiama.** Adesso: `shell_devs` quando gli si passa un nome
(`devs console`). In **M9**: `devfs`, per risolvere `/dev/<nome>` nell'inode
corrispondente — ed è il suo lavoro vero, questo di M8 è un antipasto.

**Come si sbaglia.**

- **confrontare con `strpos` o per lunghezza** invece che con `strcmp`;
- **scorrere fino a `MAX_DEVICES`** invece che fino a `ndev`. Gli slot mai
  scritti contengono zeri, quindi il loro nome è la stringa vuota e nessuna
  ricerca la trova: **funziona per fortuna.** Diventa un bug il giorno che uno
  slot viene riusato o che l'array non è più azzerato;
- **restituire l'indice invece del puntatore.** L'indice obbligherebbe il
  chiamante a un secondo giro con `device_at`, e in M9 il VFS terrebbe un indice
  che cambia significato se il registro cambia.

**Test.** Cinque controlli: trova ognuno dei tre, non trova un nome assente, non
accetta un prefisso.

---

## 4. `device_by_id`

```c
struct device *device_by_id(uint16_t major, uint16_t minor);
```

**Compito.** Come `device_find`, ma la chiave è la coppia di numeri.

**Ritorna.** Puntatore, oppure `0` se la coppia non è iscritta.

**Non deve.** Confrontare solo il major. `console` è 5:1; se domani ci fosse un
5:2 sarebbe un altro dispositivo.

**Chi la chiama.** **Nessuno, in M8** — e va detto invece di nasconderlo. Il suo
primo chiamante è il VFS di **M9**: l'inode di un file di dispositivo memorizza
`major` e `minor`, non il nome, esattamente come su Unix. Quando `cat /dev/kbd`
arriverà all'inode, la chiave che avrà in mano sarà `13:64`.

È una tensione con YAGNI, e la risolvo così: la scriviamo adesso perché M9 è la
milestone immediatamente successiva, perché sta nello spec approvato, e perché
aggiungerla dopo vorrebbe dire toccare `device.h` quando tre driver la usano
già. Nel frattempo i test host sono ciò che la tiene onesta: senza chiamante,
sono la sua unica verifica.

Se preferisci rimandarla a M9 è una scelta legittima — dimmelo e la togliamo da
header e test.

**Come si sbaglia.** Confrontando un campo solo. Il test lo prende iscrivendo
due dispositivi con lo **stesso major** e minor diverso, e chiedendo il secondo.

**Test.** Tre controlli: trova per coppia, distingue lo stesso major, ritorna 0
per una coppia assente.

---

## 5. `device_count`

```c
int device_count(void);
```

**Compito.** Restituire `ndev`.

**Ritorna.** Quanti dispositivi sono iscritti, da 0 a `MAX_DEVICES`.

**Non deve.** Contare scorrendo l'array. Il numero c'è già; ricalcolarlo
introdurrebbe una seconda verità che può divergere dalla prima.

**Chi la chiama.** `shell_devs` per enumerare, i self-check per verificare che i
tre driver si siano iscritti, e i test host quasi tutti. In **M9** il `readdir`
di `/dev`, che elenca i dispositivi come voci di directory.

**Come si sbaglia.** Difficile. L'unico modo è farla mentire dopo un rifiuto,
cioè incrementando `ndev` in `device_register` prima di aver deciso.

**Test.** Compare in quasi tutti i controlli di iscrizione, come verifica
incrociata.

---

## 6. `device_at`

```c
struct device *device_at(int i);
```

**Compito.** Dare accesso allo slot `i` senza esporre l'array.

**Ritorna.**

| caso | valore |
|---|---|
| `0 <= i < device_count()` | puntatore allo slot |
| `i` negativo | `0` |
| `i >= device_count()` | `0` |

**Non deve.** Fidarsi dell'indice. Il chiamante è un ciclo, e un ciclo si
sbaglia.

**Perché esiste.** Per la stessa ragione di `task_slot` in M6a: l'array è
`static` dentro `device.c`, e serve un modo di enumerarlo da fuori senza
renderlo globale. Senza, `shell_devs` e i self-check non potrebbero fare niente.

**Chi la chiama.** `shell_devs`, i self-check, i test host. In **M9** il
`readdir` di `/dev`.

**Come si sbaglia.**

- **non controllare il negativo.** `devs[-1]` legge i byte prima dell'array —
  che in `.bss` sono un'altra variabile;
- **controllare contro `MAX_DEVICES` invece di `ndev`.** `device_at(5)` con tre
  iscritti restituirebbe un puntatore a uno slot mai scritto, e il chiamante
  chiamerebbe `d->write` su un puntatore a funzione nullo — cioè un salto a
  zero.

**Test.** Due controlli: i tre indici validi in ordine di iscrizione, e i due
estremi fuori intervallo.

---

## 7. `serial_dev_write`

**Questa la scrivo io** — `serial.c` è un file mio. È l'esempio svolto: la forma
di questa funzione è la forma delle due che scrivi tu.

```c
static int serial_dev_write(struct device *d, const void *buf, uint32_t n);
```

**Compito.** Tradurre fra la firma uniforme del device layer e la funzione che il
driver ha già: un ciclo su `n` byte, ognuno passato a `serial_putc`.

**Ritorna.** `n` — tutti i byte scritti. La seriale in polling non può fallire:
`serial_putc` aspetta che il registro sia libero e scrive.

**Non deve.** Interpretare i byte. Nessuna traduzione di `\n`, nessun filtro sui
non stampabili: l'interpretazione sta un livello sotto, in `serial_putc`, e
farla anche qui la farebbe accadere due volte.

**Il tranello dei tipi.** `buf` è `const void *`, e su un `void *` non si può
indicizzare né fare aritmetica: serve un cast a `const char *` (o
`const uint8_t *`) **prima** di scrivere `p[i]`. È lo stesso errore che in M1
faceva `(void *)VGA_MEM + VGA_COLS` avanzare di 80 byte invece di 80 celle.

**`d` non serve e va zittito.** `(void)d;` — la firma lo impone perché in M10
servirà a distinguere due dischi, e `-Wextra` protesta se lo si ignora in
silenzio.

**`n == 0`** è legittimo: non scrive nulla e ritorna 0. Un ciclo scritto come
`do … while` lo sbaglierebbe.

---

## 8. `vga_dev_write`

```c
static int vga_dev_write(struct device *d, const void *buf, uint32_t n);
```

**Compito.** Identico a `serial_dev_write`, con `vga_putc` al posto di
`serial_putc`. Guarda quella e ricopia la forma.

**Ritorna.** `n`. Il framebuffer è memoria: scrivere non può fallire.

**Non deve.** Toccare `cursor`, `color` o la sezione critica: sono di
`vga_putc`, che le gestisce già. Questa funzione è un ciclo e nient'altro.

**Chi la chiama.** Adesso nessuno direttamente — vive dentro la s seriale in polling non può fallire, perché
       serial_putc seriale in polling non può fallire, perché
       serial_putc aspetta che il registro sia libero e poi scrive. n == 0 è
       legittimo e ritorna 0 —  aspetta che il registro sia libero e poi scrive. n == 0 è
       legittimo e ritorna 0 — truct del
dispositivo `console`, e la chiamerà chiunque faccia `d->write(d, ...)`. In
**M9** sarà `vfs_write` su `/dev/console`, e in **M15** una `write(1, ...)` di un
processo utente: la stessa funzione, con tre livelli in mezzo che ancora non
esistono.

**Come si sbaglia.** Indicizzando il `void *`; dimenticando `(void)d`;
gestendo `n == 0` con un `do … while`.

**Test.** Self-check in VM: scrivere due byte attraverso il dispositivo e poi
**rileggere il framebuffer** per verificare che ci siano finiti. È la rilettura
di sempre — su hardware muto è l'unica conferma che esista.

---

## 9. `kbd_dev_read`

```c
static int kbd_dev_read(struct device *d, void *buf, uint32_t n);
```

La funzione con il contratto più delicato di M8.

**Compito.** Estrarre **fino a** `n` byte dal ring buffer della tastiera
chiamando `keyboard_getchar`, e fermarsi appena non ce n'è più.

**Ritorna.** Quanti byte ha copiato **davvero**, da `0` a `n`.

**Zero significa «adesso non c'è niente», non «finito».** È la distinzione che
regge M9: `cat /dev/kbd` farà spin su quello zero, e se zero volesse dire fine
del file uscirebbe subito invece di aspettare che digiti.

**Non deve aspettare.** Mai. Se `keyboard_getchar` dà `-1`, la funzione ritorna
con quello che ha raccolto — anche se è niente, anche se `n` era 64. Un ciclo che
insistesse fino a riempire il buffer sarebbe un'attesa attiva dentro una funzione
che ha promesso di tornare subito, e in M9 non tornerebbe mai.

Il blocking I/O manca per scelta: sta nello spec sotto «fuori scope», e il punto
di decisione è M9.

**Non deve** essere chiamata da un gestore di interrupt, e **non deve** avere un
secondo lettore. Il ring buffer ammette un consumatore solo: è la regola di M5, e
`kbd_dev_read` la eredita insieme al buffer. Dopo M8 il consumatore è chiunque
legga `/dev/kbd`, e deve restare uno.

**Chi la chiama.** Adesso: un self-check, per verificare che a buffer vuoto
ritorni 0. In **M9**: `vfs_read` su `/dev/kbd`, e da lì la shell — che smetterà
di chiamare `keyboard_getchar` per nome e leggerà da un descrittore, senza sapere
cosa ci sia dall'altra parte. È il momento in cui «tutto è un file» diventa vero.

**Come si sbaglia.**

- **ciclare finché non si hanno `n` byte.** L'errore principale, descritto sopra;
- **perdere il carattere estratto** quando il buffer di destinazione è pieno:
  `keyboard_getchar` **consuma**, quindi il valore letto va scritto o è perduto
  per sempre. Va controllato che ci sia posto *prima* di estrarre;
- **restituire `-1` invece di `0`** quando non c'è niente. `-1` vuol dire errore,
  e non c'è nessun errore: la tastiera semplicemente non ha battuto;
- indicizzare il `void *`;
- dimenticare `(void)d`.

**Test.** Self-check in VM: senza aver digitato niente, una `read` di 8 byte
deve ritornare **0** e non toccare il buffer di destinazione.

Il caso «ritorna più di zero» non è coperto dai self-check, perché al boot non è
stato digitato niente: lo copre indirettamente `tests/keyboard.sh`, che digita e
verifica l'eco attraverso la shell. Lo scrivo perché è una lacuna vera e va
saputa, non nascosta.

**Attenzione all'ordine in `kmain`.** Il self-check che legge la tastiera gira
*prima* di `task_create(shell_task)`, quindi in quel momento non esiste un
secondo consumatore. È al sicuro per l'ordine delle righe, non per costruzione:
spostare i self-check dopo la creazione della shell li farebbe rubare caratteri.

---

## 10. L'iscrizione dentro `*_init`

Non è una funzione a sé: sono cinque righe dentro `vga_init` e `keyboard_init`.

**Compito.** Riempire uno `struct device` e passarlo a `device_register`.

**La struct può essere locale**, ed è il punto: `device_register` copia, quindi
una struct sullo stack di `vga_init` va benissimo. Se ti viene il dubbio «e se
poi sparisce?», la risposta è che deve sparire — è così che si verifica che la
copia funzioni.

**Il puntatore dell'operazione che il dispositivo non supporta va a zero**, e
non a una funzione che ritorna errore:

- `console` → `read = 0`
- `kbd` → `write = 0`

Un puntatore nullo dice «non supportata» ed è visibile a chi enumera — è quello
che permette a `devs` di stampare `-w` e `r-`. Una funzione che ritorna `-1`
sarebbe indistinguibile da un dispositivo rotto.

È la stessa convenzione di `exc_handlers[vec] == 0` in `idt.c`: puntatore nullo
uguale «nessuno se ne occupa».

**Il valore di ritorno va verificato.** Un `device_register` che fallisce al boot
va rilevato subito: `assert` è sempre attiva in questo progetto e chiama `panic`,
quindi il fallimento diventa un messaggio leggibile invece di un dispositivo che
manca senza spiegazione.

**Come si sbaglia.**

- **dimenticare di azzerare il campo non supportato.** Una struct locale non
  inizializzata contiene spazzatura dello stack, quindi `read` conterrebbe un
  indirizzo casuale e `d->read(...)` salterebbe là. Inizializzala per intero,
  con un initializer designato o azzerandola prima;
- **ignorare il ritorno di `device_register`**, e scoprire in M9 che `/dev/kbd`
  non c'è;
- **iscriversi prima di `device_init`**, cioè mettere `device_init` nel posto
  sbagliato in `kmain`.

---

## 11. `shell_devs`

```c
static void shell_devs(int argc, char **argv);
```

**Compito.** Senza argomenti, elencare tutti i dispositivi. Con un argomento,
mostrare solo quello.

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

**Ritorna.** Niente — la firma della tabella dei comandi è `void`.

**Come si costruisce.** Senza argomenti: un ciclo da `0` a `device_count() - 1`
con `device_at`. Con un argomento: una `device_find` e il messaggio se dà 0.

**Le due lettere di capacità si leggono dai puntatori**, non da un campo: `r` se
`read` non è nullo, `-` altrimenti; uguale per `w`. È il guadagno visibile della
scelta di progetto — la tabella dei comandi non ha bisogno di sapere niente dei
dispositivi per descriverli.

**L'incolonnamento a mano.** `'\t'` non si può usare: vale 9, e `vga_putc` non ha
un caso per lui — sulla seriale lo interpreta il terminale, a schermo non succede
niente. È lo stesso motivo per cui `shell_help` conta i caratteri con `strlen` e
riempie di spazi.

**`major` e `minor` sono `uint16_t`** e `%d` li stampa: `kprintf` non ha un
`%u`, e il debito è annotato in `CLAUDE.md`. Sotto 65536 non fa differenza.

**Chi la chiama.** `shell_exec`, attraverso la tabella. È l'unico chiamante che
ci sarà mai: da M9 in poi l'informazione si guarda con `ls /dev`.

**Come si sbaglia.**

- **usare `'\t'`** e vedere le colonne solo sulla seriale;
- **non gestire `device_find` che ritorna 0**, e dereferenziarlo;
- **stampare `argc` voci invece di `device_count()`**, che è il genere di
  scambio che si fa quando due contatori sono in vista.

**Test.** `tests/shell.sh` guadagna un controllo: digita `devs` e cerca tutte e
tre le righe. Prende sia l'enumerazione sia il fatto che i tre driver si siano
iscritti davvero — è un test di M8 travestito da test della shell.

---

## Riepilogo: 11 funzioni, dove va ognuna

| funzione | file | righe circa | primo chiamante vero |
|---|---|---|---|
| `device_init` | `device.c` | 3 | `kmain`, adesso |
| `device_register` | `device.c` | 30 | i tre `*_init`, adesso |
| `device_find` | `device.c` | 10 | `devs <nome>` adesso, `devfs` in M9 |
| `device_by_id` | `device.c` | 10 | **il VFS in M9** |
| `device_count` | `device.c` | 3 | `devs` e i self-check, adesso |
| `device_at` | `device.c` | 6 | `devs` e i self-check, adesso |
| `serial_dev_write` | `serial.c` | 8 | *la scrivo io* |
| `vga_dev_write` | `vga.c` | 8 | `vfs_write` in M9 |
| `kbd_dev_read` | `keyboard.c` | 14 | `vfs_read` in M9 |
| iscrizioni | `vga.c`, `keyboard.c` | 5 × 2 | — |
| `shell_devs` | `shell.c` | 20 | `shell_exec`, adesso |

Centoventi righe circa, di cui quaranta nel registro e il resto adattatori.

**La cosa da portarsi dietro:** cinque di queste undici funzioni hanno il loro
chiamante vero in **M9**, non in M8. M8 costruisce l'interfaccia; M9 è la
milestone che la usa e che dimostra se era disegnata bene. Se qualcosa qui sembra
sovrabbondante, è perché lo è *oggi* — e il piano di M9 dirà se avevo ragione.
