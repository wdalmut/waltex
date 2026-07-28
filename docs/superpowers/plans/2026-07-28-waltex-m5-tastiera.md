# M5 — Tastiera: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** contengono
> interfaccia, test, dati hardware e concetti, ma il codice lo scrive Walter
> (vedi `CLAUDE.md`). I task **[CLAUDE]** sono infrastruttura.

**Obiettivo:** leggere la tastiera dall'IRQ 1, tradurre gli scancode in
caratteri, e consegnarli al codice normale attraverso un buffer circolare. Alla
fine il kernel fa l'eco di quello che digiti.

**Architettura:** due moduli separati. Un buffer circolare generico, che è dove
sta il ragionamento sulla concorrenza e si testa interamente sull'host. E un
driver che legge la porta, decodifica gli scancode e infila i caratteri nel
buffer — con la tabella di decodifica anch'essa testabile fuori da QEMU.

## Perché questa milestone conta più di quanto sembri

In M4 il gestore e il codice normale condividevano **un contatore da quattro
byte**, che su i386 si legge con una sola istruzione: impossibile
sorprenderlo a metà.

Qui condividono una **struttura con due indici**. Non esiste nessuna istruzione
che li aggiorni insieme, quindi per la prima volta bisogna ragionare su cosa
può accadere se l'interrupt cade fra due righe. È lo stesso problema che in M6
riguarderà la tabella dei task, ma su venti righe di codice invece di un
context switch.

## Vincoli globali

Quelli precedenti, più:

- Il gestore dell'IRQ 1 legge la porta **una volta** e non blocca mai. Se il
  buffer è pieno il carattere si perde: perdere un tasto è accettabile,
  fermarsi dentro un interrupt handler no.
- Nessun lock, nessuna disabilitazione degli interrupt nel percorso normale.
  La correttezza deve venire dalla struttura del buffer, non da `cli`.
- Solo scancode set 1, layout US, nessun caps lock, nessun tastierino
  numerico. Gli scancode estesi vanno **riconosciuti e scartati**, non
  interpretati male.

## Struttura dei file al termine di M5

| File | Responsabilità | Chi |
|---|---|---|
| `include/ring.h` | il buffer circolare | CLAUDE |
| `kernel/ring.c` | push, pop, stato | **WALTER** |
| `include/keyboard.h` | `keyboard_init`, `keyboard_getchar`, decodifica | CLAUDE |
| `kernel/keyboard.c` | tabella scancode, gestore IRQ 1 | **WALTER** |
| `tests/host/test_ring.c` | il buffer, senza QEMU | CLAUDE |
| `tests/host/test_keyboard.c` | la decodifica, senza QEMU | CLAUDE |
| `tests/sendkeys.py` | inietta tasti in QEMU dal monitor | CLAUDE |
| `tests/keyboard.sh` | digita `walter` e verifica l'eco | CLAUDE |
| `kernel/main.c` | ciclo di idle che fa l'eco | CLAUDE |

**Interfacce prodotte da M5:**

```c
/* Buffer circolare a produttore singolo e consumatore singolo. */
struct ring {
    volatile uint8_t  buf[RING_SIZE];
    volatile uint32_t head;      /* scritto SOLO dal produttore */
    volatile uint32_t tail;      /* scritto SOLO dal consumatore */
};

void ring_init(struct ring *r);
int  ring_push(struct ring *r, uint8_t v);   /* 0 se pieno, 1 se accodato */
int  ring_pop(struct ring *r);               /* -1 se vuoto */
int  ring_empty(const struct ring *r);

/* Traduzione pura: nessuno stato, nessun hardware. */
int scancode_to_char(uint8_t scancode, int shift);   /* -1 se non stampabile */

void keyboard_init(void);
int  keyboard_getchar(void);                 /* -1 se non c'e' nulla */
```

---

## L'hardware, in breve

Il controller PS/2 è un 8042. Due porte:

| Porta | In lettura | In scrittura |
|---|---|---|
| `0x60` | il byte ricevuto dalla tastiera | comandi alla tastiera |
| `0x64` | registro di stato | comandi al controller |

Del registro di stato ci serve il **bit 0**: 1 significa che c'è un byte da
leggere sulla `0x60`. Nel gestore dell'IRQ 1 in realtà è già garantito — se
l'interrupt è arrivato, il byte c'è — ma controllarlo è l'abitudine giusta.

### Scancode set 1

Ogni tasto ha un **make code**, emesso alla pressione. Il **break code** del
rilascio è lo stesso valore con il **bit 7 acceso**: `0x1E` è la pressione di
`a`, `0x9E` il suo rilascio. Quindi:

```
scancode & 0x80   ->  e' un rilascio
scancode & 0x7F   ->  quale tasto
```

I codici che ti servono, layout US:

| Scancode | Tasto | | Scancode | Tasto |
|---|---|---|---|---|
| `0x02`-`0x0B` | `1234567890` | | `0x1E`-`0x26` | `asdfghjkl` |
| `0x0C` | `-` | | `0x27` | `;` |
| `0x0D` | `=` | | `0x28` | `'` |
| `0x0E` | backspace | | `0x29` | `` ` `` |
| `0x0F` | tab | | `0x2A` | shift sinistro |
| `0x10`-`0x19` | `qwertyuiop` | | `0x2B` | `\` |
| `0x1A` | `[` | | `0x2C`-`0x32` | `zxcvbnm` |
| `0x1B` | `]` | | `0x33` `0x34` `0x35` | `,` `.` `/` |
| `0x1C` | invio | | `0x36` | shift destro |
| `0x1D` | ctrl sinistro | | `0x39` | spazio |

**Gli scancode estesi** cominciano con il byte `0xE0`, seguito dal codice vero:
frecce, ctrl destro, alt destro, tasti del blocco navigazione. Senza gestirli,
premere una freccia produce due caratteri casuali. La regola minima: visto un
`0xE0`, scartare anche il byte successivo.

---

## Task 1 [CLAUDE]: header, test host, harness di iniezione, self-check

- [ ] `include/ring.h` e `include/keyboard.h` con i contratti.

- [ ] `tests/host/test_ring.c`. Il buffer si pilota direttamente, quindi si
  verifica tutto senza QEMU:

  - push e pop restituiscono i valori nello stesso ordine
  - pop su buffer vuoto dà `-1`
  - il wraparound funziona: riempi, svuota, riempi di nuovo
  - a buffer pieno `ring_push` dà 0 e **non** sovrascrive nulla
  - `RING_SIZE - 1` elementi ci stanno, l'ennesimo no (vedi sotto)

- [ ] `tests/host/test_keyboard.c` per `scancode_to_char`:

  - `0x1E` → `a`, con shift → `A`
  - `0x02` → `1`, con shift → `!`
  - `0x39` → spazio, `0x1C` → `\n`
  - `0x2A` e `0x36` (gli shift) → `-1`, non sono caratteri
  - `0x9E`, cioè un break code → `-1`
  - `0x00` e `0x58`, fuori tabella → `-1`

- [ ] `tests/sendkeys.py`: si collega al monitor di QEMU su socket unix e manda
  `sendkey`. È il modo di premere tasti senza una tastiera.

- [ ] `tests/keyboard.sh`: avvia il kernel, digita `walter`, verifica che
  l'eco compaia sulla seriale.

- [ ] I self-check dentro la VM: IRQ 1 smascherato, e il buffer inizialmente
  vuoto.

- [ ] `kernel/main.c`: il ciclo di idle diventa un eco. `hlt`, poi svuota il
  buffer stampando quello che trova.

---

## Task 2 [WALTER]: `kernel/ring.c`

Quaranta righe, e sono le più concettuali della milestone.

**La struttura.** Un array e due indici. `head` è dove il produttore scriverà
il prossimo elemento, `tail` da dove il consumatore leggerà il prossimo.
`head == tail` significa vuoto.

**La regola che rende tutto sicuro senza lock**, e va rispettata alla lettera:

> `head` è scritto **solo** dal produttore. `tail` è scritto **solo** dal
> consumatore. Ognuno dei due legge l'indice dell'altro ma non lo modifica mai.

Se questa proprietà vale, non serve nessun `cli`: ogni indice ha un solo
scrittore, e su i386 leggere un `uint32_t` allineato è atomico. Il consumatore
può leggere un `head` un po' vecchio — vedrà il buffer più vuoto di quanto sia
— ma mai un valore incoerente. E "più vuoto del vero" è innocuo: al prossimo
giro lo vedrà.

**Il tranello del contatore.** La tentazione naturale è tenere un `count` per
sapere quanti elementi ci sono. Non farlo: `count` sarebbe incrementato dal
produttore e decrementato dal consumatore, cioè **scritto da entrambi**. Un
`count++` è leggi-modifica-scrivi, e l'interrupt può cadere nel mezzo:
l'incremento e il decremento si perdono a vicenda. Introdurre `count`
distrugge esattamente la proprietà che rende il buffer sicuro.

Da cui la conseguenza: senza contatore, **"pieno" e "vuoto" andrebbero
confusi**, perché entrambi sarebbero `head == tail`. La soluzione è sacrificare
uno slot: il buffer è pieno quando il prossimo `head` raggiungerebbe `tail`.
Con `RING_SIZE` a 128 se ne usano 127. È il prezzo di non avere lock, ed è un
buon prezzo.

**Il wraparound.** Con una dimensione potenza di due, avanzare un indice è
`(i + 1) & (RING_SIZE - 1)`. Con il modulo funzionerebbe uguale ma costerebbe
una divisione, e questo codice gira dentro un interrupt handler.

---

## Task 3 [WALTER]: `kernel/keyboard.c`

**`scancode_to_char(scancode, shift)`** — funzione pura, due tabelle costanti
(normale e con shift), e il controllo del bit 7 per scartare i rilasci.
Restituisce `-1` per tutto ciò che non è un carattere stampabile: shift, ctrl,
tasti fuori tabella.

**Il gestore dell'IRQ 1** — legge `0x60`, e ha tre casi:

1. il byte è `0xE0`: alza un flag "il prossimo va scartato" e ritorna
2. il byte è un make o break code di uno shift: aggiorna lo stato dello shift
3. altrimenti: decodifica e, se è un carattere, `ring_push`

Lo stato — shift premuto, `0xE0` visto — vive in `static` di `keyboard.c` ed è
toccato **solo** dal gestore. Questo lo mette al sicuro senza pensarci: non è
condiviso, è privato di un unico flusso.

**`keyboard_getchar()`** — un `ring_pop`, e nient'altro.

**`keyboard_init()`** — `irq_register(1, ...)` e `pic_mask(1, 0)`.

---

## Task 4 [CLAUDE]: chiudere M5

Verifica completa, iniezione di tasti, aggiornamento di `CLAUDE.md`.

---

## Come si sbaglia

**Non leggere la porta `0x60` nel gestore.** Il controller non presenta un
altro interrupt finché il byte non è stato letto: la tastiera si blocca dopo il
primo tasto. È il gemello del bug dell'EOI.

**Interpretare i break code come caratteri.** Digitando `a` arrivano due byte,
`0x1E` e `0x9E`. Senza il controllo del bit 7 vedi ogni lettera due volte, la
seconda come carattere sbagliato.

**Toccare `tail` dal gestore, o `head` dal lettore.** Distrugge la proprietà a
scrittore unico e reintroduce la race che il progetto del buffer esiste per
evitare. Un `ring_pop` chiamato dentro un interrupt handler è lo stesso errore.

**Il gestore che stampa.** `kprintf` dentro il gestore della tastiera scrive
su una seriale in polling, cioè aspetta l'hardware dentro un interrupt
handler. Il carattere va nel buffer; a stamparlo pensa il codice normale.

## Lettura di accompagnamento

`kernel/keyboard.s` di Linux 0.01 — l'intero driver è scritto in assembly, con
la tabella di decodifica come sequenza di byte e la coda gestita a mano. È
istruttivo vedere quanto costa in leggibilità, e capire perché oggi si scrive
in C tutto ciò che non deve essere assembly.
