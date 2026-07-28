# M4 — Timer PIT: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** contengono
> interfaccia, test, dati hardware e concetti, ma il codice lo scrive Walter
> (vedi `CLAUDE.md`). I task **[CLAUDE]** sono infrastruttura.

**Obiettivo:** programmare il PIT a 100 Hz, agganciarlo all'IRQ 0, e abilitare
gli interrupt. Da questo momento il kernel fa qualcosa senza che nessuno glielo
chieda.

**Architettura:** `timer.c` programma il chip, registra il proprio gestore con
`irq_register(0, ...)` e smaschera la linea. Il gestore incrementa un contatore
e basta. La verifica misura la frequenza reale usando l'orologio CMOS come
riferimento indipendente.

## Perché questa milestone è diversa dalle prime tre

M1, M2 e M3 erano tutte "installa una struttura e verifica che ci sia". Qui
compare la prima `sti` del progetto, e con essa **due flussi di esecuzione**
che toccano gli stessi dati: il gestore dell'interrupt e il codice normale.

È il concetto che regge tutto M6. Vale la pena incontrarlo ora, su un contatore
da quattro byte, invece che in mezzo a un context switch.

## Vincoli globali

Quelli precedenti, più:

- Il gestore del timer fa **una cosa sola**: incrementare il contatore. Niente
  `kprintf`, niente logica. Un handler che stampa a 100 Hz satura la seriale e
  cambia il comportamento di ciò che sta misurando.
- Nessuna variabile condivisa fra handler e codice normale che non sia
  `volatile`.
- Un solo `sti` in tutto il kernel, in `kmain`, dopo che tutti i sottosistemi
  sono pronti.

## Struttura dei file al termine di M4

| File | Responsabilità | Chi |
|---|---|---|
| `include/timer.h` | `timer_init`, `timer_ticks`, `pit_divisor` | CLAUDE |
| `kernel/timer.c` | programmazione del PIT, gestore dell'IRQ 0 | **WALTER** |
| `kernel/rtc.c` | lettura dell'orologio CMOS, solo per i test | CLAUDE |
| `tests/host/test_timer.c` | il calcolo del divisore, senza QEMU | CLAUDE |
| `kernel/selftest.c` | frequenza misurata, maschera dell'IRQ 0 | CLAUDE |
| `kernel/main.c` | `timer_init`, la prima `sti`, ciclo di idle | CLAUDE |

**Interfacce prodotte da M4:**

```c
/* Il divisore da caricare nel PIT per ottenere una data frequenza.
   Funzione pura, senza effetti sull'hardware: e' testabile sull'host, ed e'
   il pezzo dove si sbaglia l'aritmetica. */
uint16_t pit_divisor(uint32_t hz);

/* Programma il canale 0, registra il gestore, smaschera l'IRQ 0. */
void timer_init(uint32_t hz);

/* Quanti tick dall'avvio. */
uint32_t timer_ticks(void);
```

---

## Il PIT in breve

Un 8253/8254 con tre canali indipendenti. Solo il canale 0 ci interessa: la sua
uscita è cablata all'IRQ 0.

| Porta | Cosa |
|---|---|
| `0x40` | dati del canale 0 |
| `0x41` | canale 1 — rinfresco della DRAM, obsoleto |
| `0x42` | canale 2 — altoparlante del PC |
| `0x43` | registro di comando |

**Il chip conta all'indietro** da un valore che gli dai tu, a una frequenza
fissa di **1193182 Hz**. Arrivato a zero emette un impulso e ricomincia. Quindi
la frequenza degli interrupt è `1193182 / divisore`, e il divisore che ti serve
è `1193182 / frequenza_voluta`.

Quel numero strano viene dal cristallo dei primi PC: 14.31818 MHz diviso 12. Il
14.31818 a sua volta è quattro volte la frequenza della sottoportante colore
NTSC, perché nel 1981 conveniva usare un cristallo già prodotto in massa per i
televisori. Quarantacinque anni dopo, ogni PC lo eredita.

**Il byte di comando** sulla porta `0x43`:

```
bit 7-6  canale        00 = canale 0
bit 5-4  accesso       11 = prima il byte basso, poi l'alto
bit 3-1  modalita'     011 = mode 3, onda quadra
bit 0    formato       0 = binario
                       -> 0x36
```

Poi il divisore va scritto sulla porta `0x40` in **due volte**: prima il byte
basso, poi l'alto. È l'accesso `11` che hai appena richiesto.

---

## Task 1 [CLAUDE]: header, test host, riferimento RTC, self-check

- [ ] **`include/timer.h`** con le tre funzioni e le costanti delle porte.

- [ ] **`tests/host/test_timer.c`**: il calcolo del divisore è aritmetica pura,
  quindi si prova in millisecondi. I casi:

  | Frequenza | Divisore atteso | Perché |
  |---|---|---|
  | 100 | 11931 | il caso normale |
  | 1000 | 1193 | |
  | 18 | 66287 → **non ci sta** | il divisore è a 16 bit, max 65535 |
  | 0 | — | divisione per zero: va gestita, non subita |
  | 2000000 | — | divisore 0, che il PIT interpreta come 65536 |

  I due estremi sono il punto: `pit_divisor` deve limitare l'intervallo invece
  di produrre un valore che il chip interpreterà in un modo che non intendevi.

- [ ] **`kernel/rtc.c`**: lettura dell'orologio CMOS sulle porte `0x70`/`0x71`.
  Serve solo ai test, come riferimento temporale **indipendente** dal timer che
  stiamo misurando. Senza, potremmo verificare che i tick avanzano ma non a
  quale velocità.

- [ ] **I self-check:**

  1. dopo `timer_init` l'IRQ 0 è smascherato — bit 0 dell'IMR del master a zero
  2. dopo la `sti`, `timer_ticks()` avanza
  3. **la frequenza misurata è 100 ± 5**: si legge il secondo dall'RTC, si
     aspetta che cambi, si campionano i tick, si aspetta il cambio successivo,
     si campionano di nuovo
  4. il contatore non salta all'indietro fra due letture consecutive

- [ ] **`kernel/main.c`**: `timer_init(100)`, la prima `sti`, e il ciclo di
  idle finale.

  Nota su quest'ultimo: oggi `kmain` ritorna e `_start` fa `cli; hlt`, quindi
  gli interrupt si spengono. Da M4 `kmain` non deve più ritornare: deve
  terminare con `for (;;) hlt;` **senza** `cli`, così la CPU dorme e si sveglia
  a ogni interrupt. Il `cli; hlt` di `_start` resta come rete di sicurezza per
  il caso "kmain è tornato, non doveva".

---

## Task 2 [WALTER]: `kernel/timer.c`

Quattro cose:

**`pit_divisor(hz)`** — la divisione, con i due estremi gestiti. Funzione pura,
niente `outb`: è quella coperta dai test host.

**`timer_init(hz)`** — scrive il comando `0x36` sulla porta `0x43`, poi il
divisore in due byte sulla `0x40`, registra il gestore con `irq_register(0,
...)` e smaschera con `pic_mask(0, 0)`.

**Il gestore** — incrementa il contatore e ritorna. Nient'altro. Non deve
mandare l'EOI: lo fa già `isr_handler`, e mandarlo due volte sballa lo stato
del PIC.

**`timer_ticks()`** — restituisce il contatore.

### Le due cose concettuali di questa milestone

**Il contatore deve essere `volatile`.** Viene scritto dal gestore e letto dal
codice normale, e per il compilatore quei due flussi non hanno nessun rapporto:
guardando `while (timer_ticks() < n) ;` non vede nessuna scrittura a `ticks`,
quindi è autorizzato a leggerlo una volta sola e trasformare il ciclo in un
`while (true)`.

È un `volatile` diverso da quello del framebuffer. Lì diceva "questa memoria
la guarda anche qualcun altro"; qui dice "**questa memoria la cambia anche
qualcun altro**, e non puoi prevedere quando".

Avvertenza onesta: il kernel compila a `-O0`, quindi il bug non si manifesta
oggi. Si manifesterebbe il giorno che aggiungiamo `-O2`. Scriverlo giusto ora
costa una parola.

**Leggere il contatore è atomico, ma per un motivo fragile.** Un `uint32_t`
allineato si legge con una singola istruzione su i386, quindi non puoi
sorprenderlo a metà aggiornamento. Se un giorno il contatore diventasse a 64
bit servirebbero due letture, e l'interrupt potrebbe cadere in mezzo: leggeresti
la metà bassa nuova e quella alta vecchia. La soluzione sarebbe `cli`/`sti`
attorno alla lettura, oppure leggere due volte finché i valori coincidono.

Non serve oggi. Serve saperlo, perché in M6 le strutture condivise saranno più
grandi di quattro byte e il problema diventerà reale.

---

## Task 3 [CLAUDE]: chiudere M4

Verifica completa, prova a mano che il kernel resta vivo e reattivo dopo la
`sti`, aggiornamento di `CLAUDE.md`.

---

## Come si sbaglia

**L'EOI mandato due volte**, una nel gestore e una in `isr_handler`. Il PIC
riceve un end-of-interrupt per un interrupt che non è in servizio, e il
comportamento diventa erratico.

**`sti` prima che il gestore sia registrato.** Il primo tick arriva, non trova
nessuno, e `isr_handler` manda comunque l'EOI: nessun danno, ma se anche l'IDT
non fosse pronta sarebbe una tripla fault immediata. L'ordine giusto è: IDT,
PIC, timer, `sti`.

**Il divisore scritto in un colpo solo** invece che in due byte separati. Il
chip ne legge uno, aspetta l'altro, e resta in uno stato incoerente.

**Il gestore che fa troppo.** Chiamare `kprintf` a 100 Hz significa che il
kernel passa la maggior parte del tempo dentro l'handler, e la misura della
frequenza diventa priva di senso.

## Lettura di accompagnamento

`kernel/sched.c` di Linux 0.01, la funzione `do_timer`. Trovi la stessa
struttura — un contatore incrementato e nient'altro di pesante — e in fondo la
chiamata a `schedule()`, che è esattamente il collegamento che farai in M6b.
`init/main.c` dello stesso kernel mostra `sti()` nella stessa posizione: ultima
cosa, dopo che tutti i sottosistemi sono pronti.
