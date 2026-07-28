# M6a — Multitasking cooperativo: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** contengono
> interfaccia, test, dati e concetti, ma il codice lo scrive Walter (vedi
> `CLAUDE.md`). I task **[CLAUDE]** sono infrastruttura.

**Obiettivo:** due flussi di esecuzione indipendenti che si passano il
controllo chiamando `task_yield()`, ciascuno con il proprio stack.

**Architettura:** una tabella statica di task, ognuno con uno stack proprio e
un `esp` salvato. `task_switch` in assembly salva i registri callee-saved sullo
stack che sta abbandonando, scambia `esp`, e ripristina quelli dell'altro. Un
task appena creato ha uno stack **falsificato** che sembra quello di un task
sospeso.

## L'idea da cui dipende tutto il resto

> **Un task è uno stack più un `esp` salvato.**

Non c'è altro. Tutti i registri, gli indirizzi di ritorno, le variabili locali
di tutte le funzioni in corso — tutto vive già sullo stack. Se ne cambi
l'indirizzo, hai cambiato *chi sta eseguendo*.

`task_switch` fa tre cose:

1. impila i registri che deve preservare sullo stack corrente
2. salva `esp` da qualche parte, e ne carica un altro
3. spila i registri dal nuovo stack, e ritorna

E la `ret` finale **non torna al chiamante**: torna dove punta il nuovo stack.
È tutta la magia del multitasking, in una decina di istruzioni.

## Vincoli globali

Quelli precedenti, più:

- Nessuna allocazione: `MAX_TASKS 8`, stack di dimensione fissa dentro la
  tabella, tutto in `.bss`.
- Nessun cambio di privilegio, nessun TSS: tutti i task girano in ring 0 e
  condividono la stessa GDT. Non serve commutare stack via hardware.
- In M6a **niente prelazione**: il timer continua a contare ma non tocca lo
  scheduler. Il controllo passa solo su chiamata esplicita a `task_yield`.
- I task condividono tutto tranne lo stack: stesso codice, stessi dati
  globali. Non c'è memoria protetta.

## Struttura dei file al termine di M6a

| File | Responsabilità | Chi |
|---|---|---|
| `include/task.h` | `struct task`, l'API, le costanti | CLAUDE |
| `kernel/switch.S` | `task_switch` | **WALTER** |
| `kernel/task.c` | tabella, creazione, `task_yield` | **WALTER** |
| `tests/host/test_task.c` | lo stack falsificato e il round-robin | CLAUDE |
| `tests/tasks.sh` | verifica l'alternanza sulla seriale | CLAUDE |
| `kernel/main.c` | crea due task e cede il controllo | CLAUDE |

**Interfacce prodotte da M6a:**

```c
#define MAX_TASKS 8
#define TASK_STACK_SIZE 4096

enum task_state { TASK_FREE = 0, TASK_READY };

struct task {
    uint32_t esp;                       /* l'unico stato salvato fuori dallo stack */
    int      state;
    uint8_t  stack[TASK_STACK_SIZE];
};

/* Registra il contesto corrente — quello di kmain — come task 0. Da questo
   momento kmain e' un task come gli altri e puo' cedere il controllo. */
void task_init(void);

/* Prepara un task che partira' da entry. -1 se la tabella e' piena. */
int task_create(void (*entry)(void));

/* Cede il controllo al prossimo task pronto. Ritorna quando qualcuno lo cede
   di nuovo a noi. */
void task_yield(void);

/* In assembly. Salva esp in *old_esp, carica new_esp, e ritorna sul nuovo
   stack. */
void task_switch(uint32_t *old_esp, uint32_t new_esp);
```

---

## La firma di `task_switch`, e perché è fatta così

La versione che si trova nei tutorial prende due `struct task *` e legge `esp`
con un offset scritto a mano nell'assembly:

```gas
movl 4(%eax), %esp        /* speriamo che esp sia a offset 4 */
```

È la fonte del bug più insidioso di questa milestone: aggiungi un campo alla
struct, gli offset slittano, e l'assembly continua a compilare leggendo il
campo sbagliato. Non c'è nessun errore — c'è un salto a un indirizzo arbitrario.

Con la firma proposta l'assembly **non conosce la struct**: riceve un puntatore
al campo `esp` e un valore. Gli offset li calcola il compilatore, e se un
giorno riordini i campi non cambia nulla.

Il prezzo è una riga in più in C, `task_switch(&tasks[cur].esp, tasks[next].esp)`.
Vale la pena.

---

## Il layout dello stack, che è il cuore di M6a

### Cosa `task_switch` deve preservare

La ABI di System V su i386 divide i registri in due gruppi. I
**callee-saved** — `ebx`, `esi`, `edi`, `ebp` — appartengono al chiamante: se
li usi, li devi restituire come li hai trovati. I **caller-saved** — `eax`,
`ecx`, `edx` — sono liberi, e chi teneva qualcosa di importante lì l'ha già
salvato da sé prima di chiamarti.

Quindi `task_switch` deve impilare **quattro** registri, non otto. Gli altri
sono già al sicuro per contratto, e salvarli sarebbe lavoro inutile a ogni
cambio di contesto.

### Lo stack di un task sospeso

Dopo i quattro push, lo stack che stai abbandonando è fatto così — indirizzi
crescenti verso il basso della pagina:

```
esp -> +0   edi          ]
       +4   esi          ]  i quattro callee-saved
       +8   ebx          ]  nell'ordine in cui li spilerai
       +12  ebp          ]
       +16  indirizzo di ritorno    <- dove la ret tornera'
       +20  old_esp      ]  i due argomenti, impilati dal chiamante
       +24  new_esp      ]
```

Da cui gli offset dei due argomenti: `20(%esp)` e `24(%esp)`. Se cambi il
numero di push, cambiano.

**L'ordine dei pop deve essere l'inverso esatto dei push.** Se impili
`ebp, ebx, esi, edi`, spili `edi, esi, ebx, ebp`. Sbagliarlo non produce
nessun errore: produce un task che riprende con i registri scambiati fra loro.

### Lo stack falsificato di un task nuovo

Qui sta il punto non ovvio. `task_switch` sa fare una cosa sola: ripristinare
un task **sospeso**. Un task appena creato non è mai stato sospeso.

La soluzione è costruirgli uno stack che *sembri* quello di un task che ha
chiamato `task_switch` ed è stato messo da parte. Cinque parole dalla cima del
suo stack verso il basso:

```
cima dello stack
       -4   entry            <- la ret salterà qui
       -8   0    (ebp)
       -12  0    (ebx)
       -16  0    (esi)
esp -> -20  0    (edi)
```

Quando `task_switch` caricherà questo `esp`, spilerà quattro zeri nei
callee-saved — sono valori che nessuno ha ancora messo lì, quindi zero va
benissimo — e poi la `ret` troverà `entry` e ci salterà. Il task comincia a
girare senza sapere di non essere mai stato interrotto.

**Il valore da salvare in `task.esp` è l'indirizzo della cella più bassa**,
quella dove sta lo zero di `edi`. Non la cima dello stack.

### Due trappole aritmetiche

**La cima dello stack non è `&stack[0]`.** Lo stack cresce verso indirizzi
*decrescenti*, quindi la cima è `&stack[TASK_STACK_SIZE]` — un byte oltre la
fine dell'array. È legale calcolarne l'indirizzo, non dereferenziarlo.

**L'allineamento.** `esp` deve restare multiplo di 4, e la cima dello stack
conviene allinearla a 16. Con un array di 4096 byte in una struct allineata,
di solito viene giusto — ma affidarsi al "di solito" in questo punto è una
brutta idea.

---

## Task 1 [CLAUDE]: header, test host, harness, collegamento in `kmain`

- [ ] `include/task.h` con la struct e l'API.

- [ ] `tests/host/test_task.c`. **Lo stack falsificato è verificabile
  senza mai eseguirlo**: `task_create` scrive cinque parole in memoria, e le
  possiamo rileggere. È il test che rende M6a affrontabile.

  - `task_create` restituisce indici crescenti, e `-1` a tabella piena
  - lo stack forgiato contiene `entry` nella posizione giusta
  - le quattro parole dei callee-saved sono azzerate
  - `task.esp` punta alla cella più bassa delle cinque
  - `esp` è allineato a 4
  - la selezione round-robin salta i task liberi e torna all'inizio

- [ ] `tests/tasks.sh`: estrae dalla seriale la sequenza di `A` e `B` e
  verifica che siano alternate almeno venti volte. Non basta che compaiano
  entrambe: due task che stampano venti `A` e poi venti `B` non si stanno
  alternando.

- [ ] `kernel/main.c`: `task_init()`, due `task_create`, e il ciclo di idle
  che chiama `task_yield`.

---

## Task 2 [WALTER]: `kernel/switch.S`

Dieci istruzioni. Quattro push, due `movl` per gli argomenti, il cambio di
`esp`, quattro pop, `ret`.

Serve la sezione `.note.GNU-stack` in fondo, come in `gdt.S` e `isr.S`,
altrimenti il linker avvisa.

## Task 3 [WALTER]: `kernel/task.c`

La tabella `static struct task tasks[MAX_TASKS]`, l'indice del task corrente,
`task_init`, `task_create` con la falsificazione dello stack, e `task_yield`
con la scelta round-robin.

`task_init` registra il contesto corrente come task 0: il suo `esp` non va
inizializzato, perché verrà scritto dal primo `task_switch` che lo abbandona.
Il suo stack è quello di boot, montato da `_start`, e l'array `stack` del task
0 resta inutilizzato.

---

## Task 4 [CLAUDE]: chiudere M6a

---

## Come si sbaglia, e perché qui fa più male

**Il `panic` di M3 qui non ti salva.** Ogni altra milestone, sbagliando,
produceva un'eccezione con un dump leggibile. Qui il guasto tipico è un `esp`
che punta a spazzatura: la `ret` salta a un indirizzo casuale, e il gestore
dell'eccezione — che ha bisogno di uno stack funzionante per girare — non ha
uno stack funzionante. Il sintomo è una tripla fault senza messaggio.

Per questo il test host sullo stack falsificato conta più che nelle altre
milestone: verifica il layout **prima** che qualcuno ci salti dentro.

**Gli errori in ordine di frequenza:**

1. `esp` inizializzato alla cima dello stack invece che venti byte più in
   basso. La `ret` legge dati oltre la fine dello stack.
2. Ordine dei pop non inverso ai push. Il task riprende con i registri
   scambiati: funziona per un po', poi si rompe in un punto senza rapporto.
3. Offset degli argomenti sbagliati, perché il numero di push è cambiato.
4. `task_yield` che cede a sé stesso. Con `old_esp` e `new_esp` che puntano
   allo stesso posto, salvi e ricarichi lo stesso valore: innocuo per caso, ma
   vale la pena escluderlo.
5. Lo stack di un task che sfora nell'altro. Non c'è memoria protetta: uno
   stack overflow corrompe il task vicino e si manifesta altrove.

## Come guardarci dentro con gdb

Il modo di rendere visibile ciò che è invisibile:

```
break task_switch
continue
x/8xw $esp            i registri appena impilati e l'indirizzo di ritorno
p/x *(unsigned int *)($esp + 20)   old_esp
p/x *(unsigned int *)($esp + 24)   new_esp
```

E dopo il cambio di `esp`, `bt` mostra lo stack dell'**altro** task. È il
momento in cui il multitasking smette di essere un'idea e diventa una cosa
che si vede.

## Lettura di accompagnamento

`include/linux/sched.h` di Linux 0.01, la macro `switch_to`. Fa una cosa
diversa dalla nostra: usa il **task switching hardware** dell'x86, con un TSS
per processo e una `ljmp` che fa commutare tutto alla CPU. Era la strada
prevista da Intel, ed è stata abbandonata da tutti — troppo lenta, e inutile
appena vuoi salvare qualcosa che il TSS non prevede. Confrontare le due è il
modo migliore di capire perché oggi il context switch si fa a mano.
