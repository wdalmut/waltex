#ifndef WALTEX_TASK_H
#define WALTEX_TASK_H

#include "types.h"

#define MAX_TASKS 8
#define TASK_STACK_SIZE 4096

/* Quante parole compongono il frame falsificato di un task nuovo: i quattro
   callee-saved, eflags, e l'indirizzo da cui partira'.

   eflags fa parte del contesto, e in M6b non e' un dettaglio. schedule() gira
   dentro il gestore del timer, dove il gate ha azzerato il flag di interrupt:
   senza eflags nel frame, un task appena creato partirebbe con gli interrupt
   spenti, ereditati da chi lo ha avviato, e non verrebbe mai piu' interrotto.
   Il kernel stamperebbe la lettera del primo task all'infinito. */
#define TASK_FRAME_WORDS 6

/* eflags con cui parte un task nuovo: bit 9 (IF) acceso perche' sia
   interrompibile, bit 1 sempre a 1 perche' l'architettura lo esige. */
#define TASK_INITIAL_EFLAGS 0x00000202u

enum task_state {
    TASK_FREE = 0,
    TASK_READY
};

/* Un task e' uno stack piu' un esp salvato. Non c'e' altro da conservare:
   registri, indirizzi di ritorno e variabili locali di ogni funzione in corso
   vivono gia' sullo stack. Cambiare esp significa cambiare chi sta
   eseguendo. */
struct task {
    uint32_t esp;                       /* l'unico stato fuori dallo stack */
    int      state;
    uint8_t  stack[TASK_STACK_SIZE];
};

/* Registra il contesto corrente — quello di kmain — come task 0. Il suo esp
   non va inizializzato: lo scrivera' il primo task_switch che lo abbandona.
   Il suo stack e' quello montato da _start, quindi l'array stack del task 0
   resta inutilizzato. */
void task_init(void);

/* Prepara un task che partira' da entry, falsificandogli uno stack che sembra
   quello di un task sospeso. Ritorna l'indice, o -1 se la tabella e' piena. */
int task_create(void (*entry)(void));

/* Cede il controllo al prossimo task pronto. Ritorna quando qualcun altro
   cede il controllo a noi — che puo' essere molto tempo dopo.

   E' la via VOLONTARIA: un task decide di rinunciare al resto del proprio
   quanto. */
void task_yield(void);

/* La via IMPOSTA: chiamata dal gestore del timer, toglie il controllo al task
   corrente senza che lui ne sappia niente.

   Il meccanismo e' lo stesso di task_yield — round-robin, prev e next
   distinti, task_switch — quindi una delle due puo' essere un guscio
   dell'altra. Cambia solo chi decide.

   Contesto in cui gira, e vale la pena tenerlo a mente:
   - dentro un interrupt handler, quindi con gli interrupt gia' disabilitati
     dal gate, e sopra la struct regs impilata da isr.S;
   - lo stack che abbandona conserva i cinque livelli di chiamata piu' la
     struct regs, e la iret che chiudera' questo interrupt avverra' sullo
     stesso stack molto tempo dopo, quando qualcuno tornera' a questo task. */
void schedule(void);

/* Indice del task in esecuzione. Serve ai test e al dump di panic. */
int task_current(void);

/* Definita in switch.S.

   Salva i registri callee-saved sullo stack corrente, scrive esp in *old_esp,
   carica new_esp, spila i callee-saved dal nuovo stack e ritorna — su quel
   nuovo stack, quindi in un altro flusso di esecuzione.

   Riceve un puntatore al campo esp e un valore, non due struct task*: cosi'
   l'assembly non conosce il layout della struct e riordinare i campi non puo'
   rompere nulla. */
void task_switch(uint32_t *old_esp, uint32_t new_esp);

/* Scelta round-robin: dato chi sta girando, a chi tocca dopo. Guarda solo il
   campo state, e la scansione parte da current + 1 riavvolgendosi — partire da
   0 sceglierebbe sempre l'indice piu' basso, e i task in mezzo non girerebbero
   mai.

   Ritorna -1 se non c'e' nessun ALTRO task pronto, mai current: cedere a se'
   stessi userebbe come new_esp un valore non ancora salvato.

   Riceve la tabella invece di un array di stati, cosi' non serve copiare
   niente per chiamarla. Resta logica pura — nessuna globale, nessun hardware —
   quindi si prova sull'host con una tabella costruita a mano. */
int task_next(const struct task *table, int n, int current);

/* Accesso a un posto della tabella, che e' static dentro task.c.

   Esiste per i test: senza, lo stack falsificato da task_create sarebbe
   verificabile solo saltandoci dentro, cioe' nel momento in cui un errore
   diventa una tripla fault muta. Con questo, il layout si controlla mentre e'
   ancora solo memoria.

   Torna utile anche al dump di panic, il giorno che vorra' dire quale task
   stava girando. Ritorna 0 per un indice fuori intervallo. */
struct task *task_slot(int i);

#endif
