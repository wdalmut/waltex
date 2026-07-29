# M6b — Multitasking preemptive: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** contengono
> interfaccia, test e concetti, ma il codice lo scrive Walter (vedi
> `CLAUDE.md`). I task **[CLAUDE]** sono infrastruttura.

**Obiettivo:** togliere il controllo a un task invece di aspettare che lo ceda.
Le `task_yield()` sparaiscono dai due task di prova, e l'alternanza continua
perché la decide il timer.

**Architettura:** il gestore dell'IRQ 0 chiama `schedule()`, che sceglie il
prossimo task e commuta. Il cambio di contesto avviene quindi **dentro un
interrupt handler**, e questo cambia tre cose: l'ordine dell'EOI, la forma
dello stack salvato, e — per la prima volta nel progetto — la necessità di una
sezione critica.

## Il codice nuovo è pochissimo. Il cambiamento è grosso.

In M6a un task **decide** di cedere. Se `task_a` non chiamasse `task_yield()`
nessun altro girerebbe mai: potere assoluto, cooperazione obbligatoria.

In M6b il controllo viene **tolto**, cento volte al secondo, mentre il task
stava facendo qualcos'altro e senza che ne sappia niente. Da cui la conseguenza
che percorre tutta questa milestone: **qualunque punto del codice può essere
interrotto**, quindi qualunque stato condiviso può essere osservato a metà
aggiornamento.

## Vincoli globali

Quelli precedenti, più:

- Il quanto di tempo è un tick: si commuta a ogni interrupt del timer. Nessuna
  priorità, nessun conteggio di quanti.
- `task_yield()` resta nell'API. Non serve più ai task di prova, ma è il modo
  in cui un task può rinunciare volontariamente al resto del proprio quanto.
- Nessuna sezione critica che duri più di poche istruzioni, e nessuna che
  contenga un `kprintf`.

---

## 1. Dove avviene il cambio di contesto, adesso

La catena di chiamate quando scatta il timer:

```
irq0 (stub)  →  isr_common  →  isr_handler  →  timer_handler  →  schedule
                                                                     ↓
                                                               task_switch
```

`task_switch` viene chiamata **in fondo a cinque livelli di interrupt
handling**. Lo stack che abbandona contiene, dal basso:

```
   i 4 callee-saved impilati da task_switch
   il frame di schedule
   il frame di timer_handler
   il frame di isr_handler
   il frame di isr_common
   struct regs, impilata dagli stub di isr.S
   eip, cs, eflags, impilati dalla CPU quando l'interrupt e' arrivato
```

Tutto questo resta lì, intatto, sullo stack di quel task. Quando qualcuno
tornerà a lui, `task_switch` ritorna a `schedule`, che ritorna a
`timer_handler`, che ritorna a `isr_handler`, che ritorna a `isr_common`, che
spila la `struct regs` e fa `iret`.

**L'interrupt viene chiuso sullo stesso stack su cui è stato preso, molto tempo
dopo.** È elegante e funziona senza codice aggiuntivo — ma è anche la ragione
del problema che segue.

## 2. L'EOI deve partire prima del cambio

Oggi `isr_handler` fa così:

```c
handler(r);
pic_eoi(irq);
```

Se `handler` è `timer_handler` e questo commuta, **si esce dalla funzione senza
essere mai arrivati a `pic_eoi`**. Il PIC resta convinto che l'IRQ 0 sia ancora
in servizio e non ne presenta più nessuno: il timer si ferma dopo il primo
tick, e con lui la prelazione.

Attenzione, perché il caso è più insidioso di così: a volte funziona. Se il
task a cui si commuta era anch'esso sospeso dentro il proprio
`timer_handler`, quando riprende arriva fino a `pic_eoi` e lo manda — al PIC
non importa chi glielo manda. Il guasto si manifesta solo commutando verso un
task **appena creato**, il cui frame falsificato salta direttamente alla
funzione d'ingresso e non ha nessun `isr_handler` in attesa di completarsi.

Quindi: il timer sembra funzionare, finché non crei un task nuovo.

**La correzione è spostare `pic_eoi` prima della chiamata al gestore.** È
sicuro perché i nostri gate sono *interrupt gate*: la CPU azzera il flag di
interrupt entrando, quindi mandare l'EOI in anticipo non permette a un secondo
interrupt di annidarsi.

Vale la pena sapere che i kernel veri fanno una terza cosa: il gestore non
commuta, **alza un flag** — in Linux si chiama `TIF_NEED_RESCHED` — e il cambio
avviene in un punto scelto, dopo che tutta la contabilità dell'interrupt è
conclusa. Costa più struttura e risolve una classe di problemi più larga di
questo.

## 3. La prima sezione critica del progetto

Questo è il punto in cui M6b ripaga tutto quello che abbiamo costruito.

`task_a` chiama `kprintf("A")`. Dentro, `vga_putc` fa:

```c
VGA_MEM[cursor] = (color << 8) | c;
cursor += 1;
```

Il timer scatta **fra le due righe**. Si commuta a `task_b`, che chiama
`vga_putc` a sua volta, legge lo stesso `cursor`, scrive nella **stessa cella**
e lo incrementa. Quando `task_a` riprende, esegue il suo `cursor += 1` su un
valore già cambiato.

Risultato: un carattere perso, e il cursore che avanza di uno invece di due. È
esattamente il problema del `count++` di cui parlavamo in M5, ma stavolta su
uno stato che *non abbiamo progettato* per essere condiviso — `cursor` e
`color` sono `static` di `vga.c`, nati quando esisteva un solo flusso di
esecuzione.

Nel ring buffer il problema si è risolto con la struttura: un solo scrittore
per indice. Qui non si può: tutti i task scrivono a schermo, e la posizione è
per definizione condivisa. Serve **impedire che l'interrupt cada in mezzo**.

### E come si scrive una sezione critica

L'istinto è `cli` all'inizio e `sti` alla fine. **È sbagliato**, e vale la pena
capire perché prima di scriverlo.

`vga_putc` può essere chiamata anche da dentro un interrupt handler — per
esempio da `panic_regs`, dove gli interrupt sono già disabilitati. Un `sti`
finale li **riabiliterebbe** in un contesto che li aveva spenti
deliberatamente, e il panic verrebbe interrotto.

La forma corretta salva lo stato precedente e lo ripristina:

```
salva eflags
cli
   ... la sezione critica ...
ripristina eflags
```

Su x86 sono `pushfl` / `popfl`, e il flag di interrupt è il bit 9 di `eflags`.
Metterò in `include/irq.h` due funzioni inline, `irq_save` e `irq_restore`,
perché sono infrastruttura e le riuserai.

---

## Struttura dei file al termine di M6b

| File | Cosa cambia | Chi |
|---|---|---|
| `include/irq.h` | `irq_save`, `irq_restore` | CLAUDE |
| `kernel/idt.c` | `pic_eoi` prima del gestore | **WALTER** |
| `kernel/timer.c` | il gestore chiama `schedule()` | **WALTER** |
| `kernel/task.c` | `schedule()` | **WALTER** |
| `kernel/vga.c` | sezione critica in `vga_putc` | **WALTER** |
| `kernel/main.c` | via le `task_yield`, torna `hlt` | CLAUDE |
| `tests/tasks.sh` | distingue prelazione da cooperazione | CLAUDE |

```c
/* Sceglie il prossimo task e commuta. Chiamata dal gestore del timer, quindi
   gira con gli interrupt disabilitati e sopra la struct regs. */
void schedule(void);
```

---

## Task 1 [CLAUDE]: `irq.h`, `main.c`, test rinforzato

- [ ] `include/irq.h` con `irq_save`/`irq_restore`.

- [ ] `kernel/main.c`: togliere `task_yield()` dai due task, che diventano
  cicli che stampano e nient'altro. E il ciclo di idle torna a `hlt`: con la
  prelazione non serve più che qualcuno ceda volontariamente, quindi `kmain`
  può dormire e lasciare che il timer lo svegli.

- [ ] `tests/tasks.sh`: **la forma della sequenza cambia**, e il test deve
  accorgersene.

  In M6a nessuno stampava due lettere di fila, perché ogni carattere era
  seguito da una cessione: la sequenza era `ABABAB`, con corse di lunghezza 1.
  In M6b un task stampa **per tutto il suo quanto**, quindi la sequenza diventa
  `AAAA...BBBB...AAAA` con corse lunghe.

  Il numero di transizioni resta un buon indicatore — a 100 Hz ce ne saranno
  decine al secondo — ma va aggiunta la verifica opposta: **le corse devono
  essere lunghe**. Corse di lunghezza 1 significherebbero che i task stanno
  ancora cedendo volontariamente, cioè che la prelazione non c'è e stiamo
  guardando M6a.

  È il modo di provare dall'esterno che il cambio è **involontario**.

## Task 2 [WALTER]: `schedule()` e l'ordine dell'EOI

`schedule()` in `task.c` fa la stessa cosa di `task_yield` — scelta
round-robin, aggiornamento di `current_task`, `task_switch` con i due indici
distinti. Se la logica è identica, `task_yield` può diventare un guscio che la
chiama, o viceversa: sono lo stesso meccanismo attivato da due cause diverse.

In `timer.c`, il gestore incrementa il contatore **e poi** chiama `schedule()`.

In `idt.c`, `pic_eoi` va spostata prima della chiamata al gestore.

## Task 3 [WALTER]: la sezione critica in `vga.c`

`vga_putc` protegge la lettura-modifica-scrittura di `cursor`. Usa `irq_save` e
`irq_restore`, non `cli`/`sti`.

Nota che la sezione deve restare **corta**: dentro non ci va la chiamata a
`set_cursor`, che sono quattro `outb` e quindi quattro accessi a porta. Il
minimo indispensabile è l'aggiornamento della posizione.

## Task 4 [CLAUDE]: chiudere M6b e il progetto

---

## Come si sbaglia

**L'EOI dopo il cambio di contesto.** Sintomo: funziona, e poi si ferma quando
crei un task nuovo. Il più insidioso della milestone.

**`sti` invece di ripristinare `eflags`.** Sintomo: il `panic` viene
interrotto a metà dump, o un interrupt handler si fa interrompere. Raro e
difficilissimo da riprodurre.

**Sezione critica troppo larga.** Mettendoci dentro `set_cursor` o un
`kprintf` intero, gli interrupt restano spenti per centinaia di cicli e il
timer perde tick. Il sintomo è una frequenza misurata più bassa di 100 Hz —
e il self-check di M4 lo catturerebbe.

**`schedule()` chiamata da `isr_handler` per un IRQ qualsiasi** invece che dal
solo gestore del timer. Commutare su ogni interrupt, tastiera compresa, rende
il comportamento dipendente da quanto scrivi.

## Lettura di accompagnamento

`kernel/sched.c` di Linux 0.01: `do_timer` in fondo chiama `schedule()`, ed è
esattamente il collegamento che stai per fare. Vale la pena notare cosa Linus
aveva già lì e noi non abbiamo: un contatore di quanti per task, cosicché un
task non venga interrotto dopo un solo tick, e le priorità. Sono le due cose da
aggiungere se questo progetto continuasse.
