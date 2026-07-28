# M3 — IDT, exception, PIC: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** sono
> deliberatamente privi di implementazione: contengono interfaccia, test, dati
> hardware e concetti, ma il codice lo scrive Walter (vedi `CLAUDE.md`). I task
> marcati **[CLAUDE]** sono infrastruttura e vanno implementati come scritto.

**Obiettivo:** dare al kernel la capacità di gestire interrupt ed eccezioni, e
di raccontare cosa è andato storto invece di riavviarsi.

**Architettura:** una tabella di 256 gate (IDT) che dice alla CPU dove saltare
per ogni vettore; 48 stub in assembly che salvano lo stato della CPU in un
formato uniforme e chiamano un dispatcher in C; il rimappaggio del PIC 8259 per
spostare gli IRQ hardware fuori dai vettori delle eccezioni; e un `panic` che
stampa registri e indirizzo del guasto prima di fermare la macchina.

**Perché è la milestone che conta di più:** finora ogni errore ha prodotto una
tripla fault, cioè una VM che riparte senza dire niente. Da qui in poi una
divisione per zero produce un messaggio con `EIP` e registri. M4, M5 e M6
costano molto meno perché ogni errore parla.

## Vincoli globali

Quelli di M1 e M2, invariati, più:

- Gli interrupt restano **disabilitati** per tutta M3: nessuna `sti`. Si
  installa la capacità di gestirli, non se ne riceve ancora nessuno
  dall'hardware. La prima sorgente reale arriva in M4 con il timer.
- Tutti i gate sono *interrupt gate* a 32 bit con DPL 0: nessun vettore
  invocabile da ring 3, e gli interrupt restano disabilitati dentro gli
  handler.
- Nessun TSS: senza cambio di privilegio la CPU non commuta stack, quindi non
  serve.

## Struttura dei file al termine di M3

| File | Responsabilità | Chi |
|---|---|---|
| `include/idt.h` | `struct regs`, `idt_init`, `irq_register` | CLAUDE |
| `include/pic.h` | `pic_init`, `pic_eoi`, `pic_mask` | CLAUDE |
| `include/panic.h` | `panic`, `assert` | CLAUDE |
| `kernel/isr.S` | 48 stub generati da macro, salvataggio stato | CLAUDE |
| `kernel/pic.c` | rimappaggio del 8259, EOI, maschere | **WALTER** |
| `kernel/panic.c` | dump dei registri e halt | **WALTER** |
| `kernel/idt.c` | gate, `idt_init`, dispatcher, `irq_register` | **WALTER** |
| `kernel/selftest.c` | check su IDT, PIC e gestione di `int $3` | CLAUDE |

**Interfacce prodotte da M3:**

```c
/* Lo stato della CPU al momento dell'interruzione, nell'ordine esatto in cui
   gli stub lo impilano. Cambiare l'ordine qui senza cambiarlo in isr.S
   produce un dump plausibile e sbagliato. */
struct regs {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;   /* pusha */
    uint32_t vec, err;                                  /* i nostri stub */
    uint32_t eip, cs, eflags;                           /* la CPU */
};

void idt_init(void);
void irq_register(uint8_t irq, void (*handler)(struct regs *));

void pic_init(void);                 /* rimappa a 32-47 e maschera tutto */
void pic_eoi(uint8_t irq);
void pic_mask(uint8_t irq, int masked);

void panic(const char *fmt, ...);    /* non ritorna mai */
#define assert(cond) ...             /* sempre attivo */
```

---

## Il quadro d'insieme, prima dei dettagli

Tre meccanismi distinti che si incastrano.

**Cosa fa la CPU quando arriva un interrupt.** Ha un numero, il *vettore*, da
0 a 255. Va nell'IDT alla voce corrispondente, ne legge selettore e offset,
impila `eflags`, `cs`, `eip` (e per alcune eccezioni un codice d'errore), e
salta lì. Al ritorno, `iret` ripristina i tre valori impilati e riprende.

**Da dove vengono i vettori.** Tre sorgenti diverse che condividono la stessa
tabella:

| Vettori | Sorgente | Esempio |
|---|---|---|
| 0-31 | eccezioni della CPU | divisione per zero, page fault |
| 32-47 | IRQ hardware, dopo il rimappaggio | timer, tastiera |
| qualsiasi | istruzione `int N` | `int $3`, e le syscall su Linux |

**Perché il PIC va rimappato.** Di default il PIC presenta gli IRQ 0-7 sui
vettori 8-15. Ma i vettori 8-15 sono già le eccezioni: 8 è il double fault, 13
il general protection fault, 14 il page fault. Senza rimappaggio, un tick del
timer e un double fault arrivano sullo stesso vettore e sono
indistinguibili. In real mode al BIOS andava bene; in protected mode è
inaccettabile. Li spostiamo a 32-47, il primo intervallo libero dopo le
eccezioni.

---

## Task 1 [CLAUDE]: `idt.h`, `pic.h`, `panic.h`, `isr.S`, self-check

**Files:**
- Create: `include/idt.h`, `include/pic.h`, `include/panic.h`, `kernel/isr.S`
- Modify: `kernel/main.c`, `kernel/selftest.c`, `tests/smoke.sh`

- [ ] **Step 1: gli header con i contratti**

Come da interfacce sopra, più le costanti dei vettori delle eccezioni con nome
(`EXC_DIVIDE_ERROR`, `EXC_BREAKPOINT`, `EXC_GENERAL_PROTECTION`, …) e i nomi
testuali per il dump.

- [ ] **Step 2: `kernel/isr.S`, i 48 stub**

Generati da tre macro. Il problema che risolvono: **alcune eccezioni impilano
un codice d'errore e altre no**, quindi lo stack all'ingresso dell'handler ha
due forme diverse. Gli stub uniformano, impilando uno zero fittizio dove la CPU
non ha impilato niente, così il C vede sempre lo stesso `struct regs`.

Le eccezioni che impilano un codice d'errore su i386 sono 8, 10, 11, 12, 13,
14 e 17.

- [ ] **Step 3: i self-check**

Sei check, tutti su cose rileggibili:

1. `sidt` restituisce limite `256 * 8 - 1`
2. il gate 0 punta davvero a `isr0` — l'offset nel gate, ricomposto dai due
   pezzi, deve valere l'indirizzo del simbolo
3. il gate 32 punta a `irq0`
4. il byte di tipo del gate 0 è `0x8E` e il selettore `0x08`
5. dopo `int $3` un contatore nell'handler è salito, e la CPU è tornata viva
6. la `struct regs` vista dall'handler ha `vec == 3` e `cs == 0x08`

Più due sul PIC, che ha registri leggibili:

7. la maschera del master dopo `pic_init` vale `0xFB` (tutto mascherato tranne
   la cascata su IRQ2)
8. la maschera dello slave vale `0xFF`

- [ ] **Step 4: `kmain` e marker**

`idt_init()` e `pic_init()` dopo `gdt_init()`, marker finale `waltex: M3 ok`.

---

## Task 2 [WALTER]: `kernel/pic.c`

Il più isolato dei tre, e per questo il primo: non dipende da niente e i suoi
effetti sono leggibili.

**Cos'è il PIC.** Due chip 8259 in cascata, otto linee ciascuno, sedici in
tutto. Lo slave è collegato alla linea 2 del master, il che spiega perché
l'IRQ2 non esiste come sorgente e perché resta sempre smascherato.

| Chip | Porta comandi | Porta dati | IRQ |
|---|---|---|---|
| master | `0x20` | `0x21` | 0-7 |
| slave | `0xA0` | `0xA1` | 8-15 |

**L'inizializzazione** è una sequenza di quattro parole di controllo, in ordine
fisso, su entrambi i chip:

| Parola | Valore | Significato |
|---|---|---|
| ICW1 | `0x11` | inizia l'inizializzazione, ICW4 seguirà |
| ICW2 | `0x20` master, `0x28` slave | il vettore base: 32 e 40 |
| ICW3 | `0x04` master, `0x02` slave | dove sta la cascata |
| ICW4 | `0x01` | modalità 8086 |

Poi si scrive la maschera sulla porta dati. Un bit a 1 significa **linea
disabilitata**: `0xFF` spegne tutto. Sul master lascia acceso solo il bit 2,
la cascata, quindi `0xFB`.

L'ICW3 del master è una **maschera di bit** — `0x04` è il bit 2, cioè "sulla
linea 2 c'è uno slave". Quello dello slave è un **numero** — `0x02` è "sono
attaccato alla linea 2". Stessa informazione, due codifiche diverse: è la
sorgente di confusione più comune di tutto il chip.

**L'EOI**, End Of Interrupt: il valore `0x20` sulla porta comandi. Per gli IRQ
da 8 a 15 va mandato a **entrambi** i chip, prima allo slave e poi al master —
altrimenti il master non sa che la cascata si è liberata e il secondo interrupt
non arriva mai. È il bug numero uno di questa milestone.

**Verifica:** la porta dati in lettura restituisce la maschera. I check 7 e 8
rileggono e confrontano.

---

## Task 3 [WALTER]: `kernel/panic.c`

**Cosa deve fare:** stampare un messaggio formattato, il contenuto dei
registri, e fermare la macchina con `cli` seguito da `hlt` in un ciclo. Non
ritorna mai — dichiaralo `__attribute__((noreturn))`, così il compilatore non
avvisa sui percorsi che non ritornano.

Va scritto **in rosso**: `vga_set_color(VGA_WHITE, VGA_RED)` prima di stampare.
È il caso d'uso per cui il colore corrente esiste.

`assert(cond)` è una macro che, se la condizione è falsa, chiama `panic` con
file e riga. Sempre attiva, mai compilata via.

**Il vincolo che rende `panic` diverso da qualunque altra funzione:** gira
quando qualcosa è già rotto. Non deve allocare, non deve ricorrere, non deve
dipendere da sottosistemi che potrebbero essere proprio quelli guasti. Se
`kprintf` fosse la causa del problema, un `panic` che la usa non stampa nulla.
Per questo il dump dei registri conviene farlo con `serial_putc` diretto, non
con la catena completa.

---

## Task 4 [WALTER]: `kernel/idt.c`

Il pezzo che tiene insieme gli altri due.

**Il gate**, otto byte come i descrittori della GDT ma con campi diversi:

| Byte | Contenuto |
|---|---|
| 0-1 | offset del gestore, bit 0-15 |
| 2-3 | selettore di segmento — `0x08`, il tuo codice |
| 4 | zero |
| 5 | tipo e attributi |
| 6-7 | offset del gestore, bit 16-31 |

Il byte di tipo per un interrupt gate a 32 bit vale `0x8E`: presente, DPL 0,
bit S a **zero** perché è un descrittore di sistema, tipo `1110`.

Nota il parallelo con M2: stessa dimensione, stessa idea di tabella e registro
che la punta, `lidt` invece di `lgdt`. E qui il bit S è zero — sono esattamente
i *system descriptor* di cui parlavamo.

Un *trap gate* ha tipo `1111`, cioè `0x8F`: l'unica differenza è che non
azzera il flag di interrupt entrando. Noi usiamo solo interrupt gate.

**`idt_init`** riempie 256 gate. Per i primi 48 gli indirizzi degli stub, per i
restanti un gestore comune o zero. Poi `lidt`.

Gli stub sono simboli globali dichiarabili con `extern void isr0(void);` e
simili: `isr.S` li esporta tutti.

**Il dispatcher `isr_handler`**, chiamato da `isr.S` con il puntatore alla
`struct regs`. Deve distinguere tre casi dal campo `vec`:

- vettore < 32: è un'eccezione della CPU, e nessuno l'ha chiesta. Chiama
  `panic` con il nome dell'eccezione, i registri e l'`EIP`.
- vettore fra 32 e 47: è un IRQ. Cerca un gestore registrato, lo chiama se
  c'è, e **manda comunque l'EOI**. Saltare l'EOI perché nessuno ha registrato
  un gestore blocca quella linea per sempre.
- altro: `int` software. Per ora basta contarlo.

**`irq_register`** è un array di sedici puntatori a funzione. Serve a far sì
che `timer.c` in M4 e `keyboard.c` in M5 non sappiano niente dell'IDT.

---

## Task 5 [CLAUDE]: chiudere M3

Verifica completa, dimostrazione che un'eccezione vera produce un dump
leggibile invece di un riavvio, aggiornamento di `CLAUDE.md`.

---

## Come si sbaglia, in ordine di frequenza

**L'ordine dei campi di `struct regs` diverso dall'ordine dei push in
`isr.S`.** Il dump esce plausibile e completamente sbagliato: vedi `EIP` dove
c'è `eflags`. Non è una fault, è peggio — è un debugger che mente. Se i valori
del dump sembrano assurdi, il sospetto è questo prima di tutto.

**EOI mancante, o mandato al solo master per un IRQ dello slave.** Il primo
interrupt arriva, il secondo mai. Sintomo classico in M4: il timer conta fino
a uno e si ferma.

**Il gate che punta all'indirizzo sbagliato**, di solito per l'offset spezzato
in due pezzi come nella GDT. Salta in mezzo al codice e produce una fault
dentro la gestione della fault, cioè un double fault.

**`sti` prima che l'IDT sia pronta.** In M3 non c'è nessuna `sti`, ed è
deliberato: la capacità di gestire interrupt si installa prima di riceverne.

## Lettura di accompagnamento

`kernel/traps.c` e `kernel/asm.s` di Linux 0.01. Trovi la stessa distinzione
fra eccezioni con e senza codice d'errore, risolta con due macro come qui, e un
dump dei registri scritto a mano. `include/asm/system.h` ha `set_trap_gate` e
`set_intr_gate`, che sono il tuo `idt_set_gate` in forma di macro assembly.
