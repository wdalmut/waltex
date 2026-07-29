# M7 — Shell: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** contengono
> interfaccia, test, concetti e tranelli, ma il codice lo scrive Walter
> (vedi `CLAUDE.md`). I task **[CLAUDE]** sono infrastruttura.

**Obiettivo:** un prompt. Il kernel legge una riga dalla tastiera, la spezza in
parole, cerca la prima in una tabella di comandi e la esegue. Alla fine si può
scrivere `echo ciao`, `ticks`, `ps`, `peek 0xb8000` e ottenere una risposta.

**Architettura:** due moduli separati, con lo stesso criterio di M5 — la logica
pura sta da un lato e si prova sull'host, l'hardware dall'altro. `lineedit.c` è
l'editor di riga: accumula caratteri, gestisce il backspace, dice quando la riga
è finita. Non stampa niente da sé: riceve un **sink di eco** come puntatore a
funzione, che è ciò che lo rende testabile fuori da QEMU e che è lo stesso
espediente dei due sink di `kprintf`. `shell.c` è la tabella dei comandi più il
task che pompa i caratteri dalla tastiera nell'editor.

**Perché prima e non ultima.** La shell è lo strumento di debug delle nove
milestone che seguono. Oggi per guardare un indirizzo si aggiunge una `kprintf`
e si ricompila; da domani si scrive `peek`. In M13, quando le tabelle delle
pagine andranno ispezionate mentre si scrive il paging, questo prompt sarà la
differenza fra vedere e indovinare.

## Vincoli globali

Quelli dei blocchi precedenti, più:

- **Il consumatore della tastiera si sposta, non si aggiunge.** Il ring buffer
  ammette **un solo** consumatore. Oggi è il ciclo di idle di `kmain`; alla fine
  di M7 è la shell, e `kmain` non deve più chiamare `keyboard_getchar`.
- **Niente allocazione dinamica**, come sempre. `LINE_MAX 128`,
  `SHELL_MAX_ARGS 8`, array statici.
- **La shell è un task come gli altri**, creato con `task_create` e soggetto a
  prelazione. Non ha privilegi e non disabilita gli interrupt.
- **`lineedit.c` non deve conoscere né `kprintf` né la VGA.** Se lo include, il
  test host smette di funzionare e il modulo perde il suo scopo.
- Nessun blocking I/O: la shell fa spin su `keyboard_getchar`. È deliberato, sta
  nello spec, e il punto di decisione è M9.

## Struttura dei file al termine di M7

| File | Responsabilità | Chi |
|---|---|---|
| `include/lineedit.h` | l'editor di riga e il suo sink di eco | CLAUDE |
| `kernel/lineedit.c` | accumulo, backspace, fine riga | **WALTER** |
| `include/shell.h` | tabella comandi, split, parse_hex | CLAUDE |
| `kernel/shell.c` | i comandi e il task del prompt | **WALTER** |
| `kernel/vga.c` | impara `\b` (aggiunta di poche righe) | **WALTER** |
| `include/demo.h`, `kernel/demo.c` | i due task rumorosi, accesi da `spin` | CLAUDE |
| `kernel/main.c` | crea il task shell, smette di leggere la tastiera | CLAUDE |
| `tests/host/test_lineedit.c` | 22 controlli, senza QEMU | CLAUDE |
| `tests/host/test_shell.c` | 22 controlli su split e parse_hex | CLAUDE |
| `tests/shell.sh` | digita `echo ciao` e verifica la risposta | CLAUDE |
| `tests/tasks.sh` | manda `spin` prima di misurare | CLAUDE |
| `tests/keyboard.sh` | perde il filtro `tr -d 'AB'` | CLAUDE |
| `kernel/selftest.c` | 3 controlli sul `\b` della VGA | CLAUDE |

**Interfacce prodotte da M7** — i due header che scrivo io nel Task 1, e contro
cui Walter compila:

```c
/* ---- include/lineedit.h ---- */

#define LINE_MAX 128

/* Il sink di eco e' un puntatore a funzione per la stessa ragione per cui
   kprintf ha due sink: senza, questo modulo dovrebbe chiamare kprintf, e il
   test host non potrebbe verificare COSA e' stato echeggiato. Con il sink, il
   test passa una funzione che accoda in un buffer e controlla byte per byte —
   compresa la sequenza "\b \b" del backspace, che altrimenti si potrebbe solo
   guardare a schermo e sperare. */
struct lineedit {
    char buf[LINE_MAX];
    int  len;
    void (*echo)(char c);        /* 0 = nessun eco, lecito */
};

void lineedit_init(struct lineedit *le, void (*echo)(char c));

/* Consuma un carattere. Ritorna 1 se la riga e' completa — e allora buf e'
   terminato da NUL e len e' la sua lunghezza — 0 altrimenti. */
int lineedit_putc(struct lineedit *le, char c);

/* Azzera la riga tenendo il sink. Da chiamare dopo aver eseguito il comando. */
void lineedit_reset(struct lineedit *le);


/* ---- include/shell.h ---- */

#define SHELL_MAX_ARGS 8
#define SHELL_PROMPT   "waltex> "

struct shell_cmd {
    const char *name;
    void (*fn)(int argc, char **argv);
    const char *help;
};

/* Spezza line in parole scrivendo dei NUL dentro line, e riempie argv con
   puntatori dentro line stessa. Ritorna argc, al massimo max.
   Pura: nessuno stato, nessun hardware, nessuna copia. */
int shell_split(char *line, char **argv, int max);

/* Legge un numero esadecimale, con "0x" facoltativo. Ritorna 1 se ok e scrive
   *out, 0 se malformato lasciando *out intatto.
   Convenzione 1/0 come ring_push, non -EINVAL: errno.h arriva in M14 e
   anticiparlo violerebbe la disciplina delle milestone. */
int shell_parse_hex(const char *s, uint32_t *out);

/* Esegue una riga: split, ricerca nella tabella, chiamata. */
void shell_exec(char *line);

void shell_init(void);

/* Il task del prompt. Non ritorna. */
void shell_task(void);


/* ---- include/demo.h ---- */

void demo_tasks_init(void);      /* crea i due task, silenziosi */
void demo_tasks_start(void);     /* accende la stampa di A e B */
```

---

## Due scoperte fatte leggendo il codice, che cambiano il piano

### 1. Il backspace arriva, ma la VGA non lo sa cancellare

`kernel/keyboard.c` ha `[0x0E] = '\b'` in **entrambe** le tabelle, quindi il
backspace attraversa il ring buffer e arriva alla shell. Buona notizia.

Ma `vga_putc` in `kernel/vga.c:58-65` ha due soli casi: `c >= 32` scrive nella
cella, `c == '\n'` va a capo. `'\b'` vale 8, quindi **non fa niente**: il cursore
non torna indietro e il carattere non si cancella.

Sulla **seriale** funziona comunque, perché il `\b` lo interpreta il terminale
all'altro capo. Su **VGA** no. Quindi cancellare a schermo richiede una piccola
aggiunta a `vga.c`, ed è il Task 3.

La sequenza classica per cancellare è **tre caratteri**: `\b` per tornare
indietro, `' '` per sovrascrivere, `\b` per tornare di nuovo. Con `\b` che
decrementa il cursore e lo spazio che lo incrementa, il conto torna: netto,
il cursore arretra di uno e la cella è vuota.

### 2. Lo shift si spegne dopo ogni tasto, non al rilascio

Osservazione, non un compito: in `keyboard_handler` la riga `shift_pressed = 0`
sta nel ramo `else`, che viene eseguito per **qualunque** tasto normale. Quindi
tenendo premuto shift e digitando `AB` si ottiene `Ab`: il primo tasto consuma il
flag.

Non blocca M7 — i comandi sono minuscoli e `tests/keyboard.sh` digita `walter` —
ed è tuo il file, quindi lo segnalo e non lo tocco. Il debito in `CLAUDE.md` è
scritto in modo più mite di quanto la realtà sia; se vuoi sistemarlo è una riga,
spostando l'azzeramento nel ramo che riconosce il **break code** dello shift
(`0xAA` e `0xB6`) invece che in quello dei tasti normali.

---

## Task 1 [CLAUDE]: header, test host, self-check, harness

Scrivo `include/lineedit.h`, `include/shell.h`, `include/demo.h` esattamente come
sopra; `tests/host/test_lineedit.c` e `tests/host/test_shell.c`; la regola nel
`tests/host/Makefile`; i tre self-check sul `\b`; e `tests/shell.sh`.

I test non linkeranno finché i moduli di Walter non esistono: è lo stato rosso di
partenza, come in M5.

**`tests/host/test_lineedit.c` — 22 controlli.** Il sink di eco è una funzione
che accoda in un buffer statico, così l'eco si verifica byte per byte.

- `'a'` su riga nuova ritorna 0
- `"abc\n"` ritorna 1 sulla `'\n'`
- dopo la riga completa `buf` è `"abc"`
- `len` è 3
- l'eco ricevuto è esattamente `"abc\n"`
- riga vuota più `'\n'`: ritorna 1, `buf` è `""`, `len` è 0
- backspace su riga vuota ritorna 0 e lascia `len` a 0
- backspace su riga vuota **non produce eco** — se lo producesse si mangerebbe
  il prompt, ed è il caso che si nota solo guardando lo schermo
- `"ab"` più `'\b'`: `len` è 1
- `"ab"` più `'\b'`: `buf` è `"a"`
- `"ab"` più `'\b'`: l'eco è `"ab\b \b"`
- due backspace consecutivi su `"ab"` portano `len` a 0
- `LINE_MAX - 1` caratteri vengono accettati
- il carattere successivo è scartato: `len` resta `LINE_MAX - 1`
- il carattere scartato non produce eco
- a buffer pieno un `'\n'` chiude comunque la riga
- `'\t'` è scartato, `len` invariato
- `'\t'` non produce eco
- ESC (27) è scartato
- `lineedit_reset` azzera `len`
- `lineedit_reset` conserva il sink
- un sink nullo non fa crashare `lineedit_putc`

**`tests/host/test_shell.c` — 22 controlli.** Servono stub inerti per ciò che la
tabella dei comandi chiama e che sull'host non esiste (`kprintf`, `timer_ticks`,
`task_slot`, `task_current`, `vga_clear`, `panic`, `demo_tasks_start`,
`keyboard_getchar`): è lo stesso espediente di `test_timer.c`, che stubba
`irq_register` e `pic_mask`. Sotto test c'è solo logica pura.

`shell_split`:

- `"echo ciao"` dà argc 2
- `argv[0]` è `"echo"`
- `argv[1]` è `"ciao"`
- `""` dà argc 0
- `"   "` dà argc 0
- `"  echo   ciao  "` dà argc 2 con gli argomenti giusti (due controlli)
- dieci parole con max 8 danno argc 8
- una parola sola dà argc 1
- la riga è modificata in place: c'è un NUL dopo `"echo"`

`shell_parse_hex`:

- `"1000"` dà `0x1000` — la base è **sempre** 16, il prefisso è facoltativo
- `"0x1000"` dà `0x1000`
- `"0X1000"` dà `0x1000`
- `"ff"` dà `0xFF`
- `"FF"` dà `0xFF`
- `"0"` dà 0 e ritorna 1
- `""` fallisce
- `"0x"` fallisce
- `"xyz"` fallisce
- `"12g4"` fallisce
- nove cifre falliscono (non ci starebbero in 32 bit)
- `"ffffffff"` dà `0xFFFFFFFF`
- in caso di fallimento `*out` non è toccato

**Self-check in `kernel/selftest.c` — 3 controlli**, perché il `\b` della VGA
esiste solo davanti al framebuffer:

- `vga_putc('\b')` con il cursore a 0 non fa danni e lo lascia a 0
- `'X'`, `'\b'`, `'Y'` lasciano `'Y'` nella cella 0
- dopo `'X'` e `'\b'` il cursore **hardware** è tornato a 0, riletto dai
  registri `0x0E`/`0x0F` del CRTC

**`tests/shell.sh`.** Modellato su `keyboard.sh`, che è già la macchina che
serve. Digita `echo` spazio `ciao` Invio e cerca `ciao` come riga a sé.

I nomi dei tasti sono **verificati contro QEMU**, non ricordati: `ret` per
Invio, `spc` per lo spazio, `backspace` per il backspace. `enter` e `space`
vengono **rifiutati** dal monitor.

```text
python3 tests/sendkeys.py "$MON" e c h o spc c i a o ret
```

Il marker che i test aspettano prima di digitare diventa `waltex: M7 ok`, **non**
il prompt: il prompt non ha un ritorno a capo in fondo, quindi cercarlo con
`grep` su un file che sta crescendo è una corsa. Il marker è una riga intera e
significa già «tutto quello che precede ha funzionato».

**Verifica:** `make` compila (gli header nuovi non rompono nulla), i test host
nuovi non linkano. Nessun commit: è metà di una milestone.

---

## Task 2 [WALTER]: `kernel/lineedit.c`

**Cosa deve fare.** Tenere un buffer e una lunghezza, e classificare ogni
carattere che arriva in **quattro** casi. Non ce ne sono altri, e riconoscerlo
prima di scrivere è metà del lavoro:

1. **`'\n'`** — la riga è finita. Termina `buf` con un NUL, echeggia il ritorno
   a capo, ritorna 1. Vale anche a riga vuota: premere Invio senza scrivere
   niente è legittimo e produce un prompt nuovo.
2. **`'\b'`** — cancella. **Solo se c'è qualcosa da cancellare.** Con `len` a
   zero non si fa niente e soprattutto non si echeggia: l'eco arretrerebbe il
   cursore sopra il prompt e se lo mangerebbe.
3. **carattere stampabile** — accoda ed echeggia, **se c'è posto**. Serve spazio
   per il NUL finale, quindi la capacità utile è `LINE_MAX - 1`. Se non c'è
   posto: né accodare né echeggiare, così chi digita vede che non entra più.
4. **tutto il resto** — `'\t'`, ESC, e ogni altro carattere di controllo:
   scartare in silenzio. Accodare un tab renderebbe `shell_split` imprevedibile,
   accodare un ESC scriverebbe spazzatura a schermo.

**La sequenza del backspace è tre caratteri**, non uno: `\b`, spazio, `\b`.
Il primo arretra, lo spazio sovrascrive e avanza, il secondo arretra di nuovo.
Mandare solo `\b` sposta il cursore lasciando il carattere visibile.

**Il sink va chiamato attraverso il puntatore**, e va controllato che non sia
nullo prima di usarlo. Non chiamare `kprintf` da questo file: se lo fai il test
host non linka più e il modulo ha perso la ragione di esistere.

**Verifica:** `make -C tests/host` e i 22 controlli di `test_lineedit` passano.
Nessun commit ancora.

---

## Task 3 [WALTER]: `vga_putc` impara `\b`

**Cosa deve fare.** Un terzo caso in `vga_putc`, accanto a `c >= 32` e
`c == '\n'`: quando il carattere è `'\b'`, il cursore torna indietro di uno —
**se non è già a zero**.

Non si scrive niente nella cella: cancellare è compito di chi manda lo spazio.
Questa aggiunta sposta soltanto il cursore.

**Dove va messa** conta: dentro la sezione critica aperta da `irq_save`, con gli
altri due casi, perché tocca `cursor` come loro. Fuori, si riapre esattamente la
corsa che quella sezione critica esiste per chiudere.

**Il caso limite che il self-check controlla** è il cursore a zero. Un
decremento non protetto su un `uint16_t` a zero dà 65535, il controllo
`cursor >= VGA_COLS*VGA_ROWS` subito sotto scatta, e il risultato è uno scroll
spurio al primo backspace su riga vuota — cioè lo schermo che scorre quando
premi backspace di troppo. Sintomo strano, causa banale.

**Verifica:** `make test`, i tre self-check nuovi passano, e i 40 vecchi
continuano a passare. Nessun commit ancora.

---

## Task 4 [WALTER]: `shell_split` e `shell_parse_hex`

Due funzioni pure, il pezzo di `shell.c` che si prova sull'host.

**`shell_split(char *line, char **argv, int max)`.** Cammina la riga con due
stati: *dentro una parola* e *fra le parole*. Alla prima transizione
fuori→dentro registra il puntatore in `argv`; alla prima dentro→fuori scrive un
NUL. Ritorna quante parole ha registrato, fermandosi a `max`.

**Non copia niente.** Scrive i NUL dentro `line` e mette in `argv` puntatori
dentro `line` stessa. È il modo Unix di farlo, e ha una conseguenza da tenere a
mente: dopo lo split la riga originale non esiste più come stringa unica. Se un
comando volesse la riga intera dovrebbe salvarsela prima.

**I casi che contano** sono le stringhe vuote e gli spazi di troppo: `""`,
`"   "`, `"  echo   ciao  "`. Spazi iniziali, finali e multipli non devono
produrre parole vuote. Un ciclo che assume «uno spazio separa due parole»
inciampa su tutti e tre.

**`shell_parse_hex(const char *s, uint32_t *out)`.** Salta un eventuale `0x` o
`0X`, poi accumula: `val = val * 16 + cifra`. Le cifre sono `0-9`, `a-f`, `A-F`;
qualunque altro carattere è un fallimento, non una fine.

**Tre fallimenti da non dimenticare**, ognuno con un suo test:

- la stringa **vuota**, e `"0x"` da solo: nessuna cifra letta. Un accumulatore
  inizializzato a zero che non conta le cifre restituisce 0 e sembra riuscito.
  Serve contare quante cifre si sono viste.
- **più di otto cifre**: non ci starebbero in 32 bit e il valore si
  troncherebbe in silenzio.
- in caso di fallimento **`*out` non va toccato**. Chi chiama deve poter tenere
  il valore che aveva.

**Verifica:** i 22 controlli di `test_shell` passano. Nessun commit ancora.

---

## Task 5 [WALTER]: la tabella dei comandi e il task del prompt

**La tabella.** Un array statico di `struct shell_cmd`, terminato da una voce
con `name` a 0 così `shell_exec` sa dove fermarsi senza una costante da tenere
in sincrono. Otto comandi:

| Comando | Cosa fa | Cosa usa |
|---|---|---|
| `help` | elenca nome e descrizione di ogni voce | la tabella stessa |
| `echo` | stampa `argv[1..]` separati da spazio | — |
| `ticks` | i tick dal boot | `timer_ticks()` |
| `ps` | per ogni task: indice, stato, `esp`, e chi sta girando | `task_slot()`, `task_current()` |
| `peek` | `peek <indirizzo> [n]`, default 64 byte, in esadecimale a 16 per riga | `shell_parse_hex` |
| `spin` | accende i due task rumorosi | `demo_tasks_start()` |
| `clear` | pulisce lo schermo | `vga_clear()` |
| `panic` | provoca un panic deliberato | `assert(0)` o `panic()` |

`help` che si stampa dalla tabella invece che da una stringa scritta a mano è
l'unico modo perché non menta mai: aggiungere un comando aggiorna `help` da sé.

`peek` è il comando che ripaga la milestone. Prima del paging ogni indirizzo è
leggibile e non può faultare, quindi non serve nessuna protezione: la
segmentazione è piatta su 4 GiB e senza paging non esistono pagine non mappate.
Da M13 la storia cambia, e sarà `peek` stesso a mostrarlo.

**`shell_exec(char *line)`.** Split, e se `argc` è 0 non fare niente — riga
vuota, si ristampa solo il prompt. Altrimenti scorrere la tabella confrontando
`argv[0]` con `name`. Trovato: chiamare `fn(argc, argv)`. Non trovato: dire
quale comando non esiste, perché un messaggio muto costa una ricompilazione per
capire se hai sbagliato a digitare o se il comando non c'è.

Serve un confronto fra stringhe, e **non esiste `strcmp`**: questo è codice
freestanding. Scrivilo, come `memcpy` in M1. Va in `kernel/memory.c` accanto agli
altri, non in `shell.c`: dalla M9 servirà al VFS per confrontare i nomi dei file,
e la sua casa naturale è quella. La dichiarazione in `include/memory.h` la
aggiungo io nel Task 1, così compili contro un contratto già fissato.

`memory.c` non compariva in nessuna delle due liste di `CLAUDE.md` — l'ho
aggiunto alla tua, perché è tuo dal M1 e l'omissione era una svista mia.

**Il sink di eco, che è dove i due moduli si incontrano.** `lineedit.c` non sa
stampare: qualcuno deve dargli una funzione. Serve quindi in `shell.c` una
funzioncina — `static void shell_echo(char c)` — che non fa altro che
`kprintf("%c", c)`, e serve collegarla con `lineedit_init(&le, shell_echo)`.

Va fatto **una volta**, in `shell_init` o all'inizio di `shell_task`, mai dentro
il ciclo: `lineedit_init` azzera anche `len`, quindi rifarlo a ogni carattere
cancellerebbe la riga mentre la stai scrivendo. Per ricominciare dopo un comando
esiste `lineedit_reset`, che tiene il sink.

Lo `struct lineedit` è uno solo e vive come `static` in `shell.c`, non sullo
stack di `shell_task`: 128 byte più i campi su uno stack da 4 KB stanno, ma la
disciplina del progetto è array statici a capacità fissa, e in M14 questo stack
diventerà quello di un processo utente.

**`shell_task(void)`.** Il ciclo, e non ritorna:

1. stampa il prompt
2. cicla su `keyboard_getchar()` finché non dà un carattere
3. lo passa a `lineedit_putc`
4. quando quello ritorna 1: `shell_exec(le.buf)`, `lineedit_reset(&le)`, e
   torna al punto 1

**Sul `hlt` nel ciclo di attesa: non usarlo.** In `kmain` c'era e andava bene,
perché `kmain` era l'idle. Qui no: `hlt` ferma la CPU fino al prossimo
interrupt, e il timer scatta cento volte al secondo, quindi *funzionerebbe* — ma
`hlt` è privilegiata, e in M14 questo stesso ciclo finirà in ring 3, dove
`hlt` prende un #GP. Scriverlo adesso senza `hlt` significa non riscriverlo
allora.

**Lo spin è deliberato**, non una svista: manca il blocking I/O, sta nello spec
sotto «fuori scope», e il punto di decisione è M9. Con la prelazione gli altri
task girano comunque.

**Verifica:** `make test`. `tests/shell.sh` passa. Nessun commit: chiude il
Task 6.

---

## Task 6 [CLAUDE]: rifilo, test aggiornati, chiusura

**`kernel/demo.c` e `include/demo.h`.** Tiro `task_a` e `task_b` fuori da
`main.c`, con un flag `volatile` che parte spento: stampano solo dopo `spin`.
`main.c` torna a essere la sola sequenza di boot, che è quello che deve essere.

**`kernel/main.c`.** Togliere il ciclo di eco delle righe 107-114 — è il punto
in cui il consumatore della tastiera **si sposta**, e leggere da due posti
violerebbe la regola del ring buffer. `kmain` diventa: `shell_init()`,
`task_create(shell_task)`, `demo_tasks_init()`, e poi il ciclo di idle che
dorme in `hlt` senza leggere niente. Il marker finale diventa `waltex: M7 ok`.

**`tests/tasks.sh`.** Manda `spin` con `sendkeys.py` prima di misurare. Il test
ne esce **migliore**: finora si appoggiava a un effetto collaterale del kernel,
adesso provoca lui la condizione che misura.

**`tests/keyboard.sh`.** Cade il filtro `tr -d 'AB'`, perché il rumore non c'è
più a meno che qualcuno lo chieda. Cambia anche il marker atteso, da
`waltex: eco attiva` a `waltex: M7 ok`.

**Il `Makefile` di radice.** Una riga: `./tests/shell.sh $(KERNEL)` nel target
`test`. È l'unica modifica al Makefile che M7 richiede — i tre moduli nuovi li
prende `$(wildcard kernel/*.c)` da sé, ma un test nuovo non si aggiunge da solo,
e un test che nessuno lancia è peggio di un test che non esiste.

**`README.md` e `CLAUDE.md`.** La tabella delle milestone, lo stato, i numeri
dei test: 143 host (99 + 44), 43 self-check (40 + 3), 4 script.

**Verifica finale:** `make test` verde da zero, `make run` mostra un prompt che
risponde, zero warning.

Poi **propongo** il commit — `M7: shell del kernel, editor di riga e tabella dei
comandi` — e lo eseguo solo se confermi.

---

## Come si sbaglia

**Leggere la tastiera da due posti.** Se `kmain` continua a fare l'eco mentre la
shell legge, i due si rubano i caratteri a vicenda: digitando `echo` la shell ne
vede `eh` e `kmain` stampa `co`. Il sintomo è caratteri che mancano a caso, e la
causa è la regola del ring buffer violata — un consumatore solo.

**Echeggiare il backspace a riga vuota.** Tre caratteri di troppo e il prompt
si accorcia di uno ogni volta. Dopo otto backspace il prompt è sparito del tutto.
Il test c'è proprio per questo.

**Decrementare il cursore a zero senza guardarlo.** `uint16_t` che va a 65535,
il controllo di scroll scatta, e lo schermo scorre quando premi backspace di
troppo.

**Mandare solo `\b` per cancellare.** Il cursore torna indietro ma il carattere
resta a schermo, e il successivo lo sovrascrive: sembra funzionare finché non
cancelli l'ultimo carattere di una parola.

**`shell_split` che produce parole vuote.** Con spazi multipli o iniziali,
`argv[0]` diventa `""` e nessun comando corrisponde più. Sintomo: `help` con due
spazi davanti non funziona, `help` da solo sì.

**Tenere la riga dopo lo split.** `shell_split` ci scrive dei NUL dentro: dopo
la chiamata `line` è solo la prima parola. Un comando che volesse tutto il resto
deve prenderselo da `argv`, non da `line`.

**`shell_parse_hex` che accetta la stringa vuota.** `peek` senza argomenti
leggerebbe l'indirizzo 0 invece di dire che manca un argomento.

**`hlt` dentro `shell_task`.** Funziona oggi, prende un #GP in M14.

**Il comando `panic` che non stampa.** Serve a dimostrare che M3 funziona ancora
e che il dump è leggibile dal prompt. Se non produce nome dell'eccezione, `EIP` e
registri, il problema è in `panic_regs`, non nella shell.

## Lettura di accompagnamento

Poco, per questa milestone: non c'è hardware nuovo e non c'è nessun manuale
Intel da consultare. È la prima milestone del progetto interamente di software.

Se vuoi un confronto, `sh.c` di xv6 è cento righe e fa molto di più — ma le fa
con `fork`, `exec` e le pipe, cioè con tutto quello che qui arriva da M15 in
avanti. Vale la pena leggerlo **adesso** per vedere dove si sta andando, e
riletto dopo M16 per vedere quanto ci si è arrivati vicino.
