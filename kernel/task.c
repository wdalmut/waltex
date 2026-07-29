#include "task.h"
#include "types.h"
#include "irq.h"

static int current_task;
static struct task tasks[MAX_TASKS];

void task_init(void)
{
    int i;

    for (i = 0; i < MAX_TASKS; i++)
        tasks[i].state = TASK_FREE;

    /* Il task 0 e' il contesto che sta girando in questo momento: kmain, sullo
       stack montato da _start. Non si falsifica niente, e in particolare non si
       tocca il suo esp — quello e' una casella dove il primo task_switch che
       abbandona questo contesto scrivera' il punto in cui era arrivato.

       E' l'opposto di task_create, che inventa un passato per un task che non
       ha mai girato. */
    tasks[0].state = TASK_READY;
    current_task = 0;
}

int task_create(void (*entry)(void))
{
    struct task *t;
    uint32_t *sp;
    int i, slot = -1;

    for (i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_FREE) {
            slot = i;
            break;
        }
    }

    if (slot < 0)
        return -1;

    t = &tasks[slot];

    /* Falsifica lo stack di un task sospeso: esattamente cio' che task_switch
       si aspetta di trovare quando lo ripristina.

       La cima e' un byte oltre la fine dell'array, perche' lo stack cresce
       verso indirizzi decrescenti. L'idioma *--sp fa quello che fa una push:
       arretra e scrive. Al termine sp e' proprio il valore che serve a esp. */
    sp = (uint32_t *)(t->stack + TASK_STACK_SIZE);

    *--sp = (uint32_t)entry;         /* dove salterà la ret               */
    *--sp = TASK_INITIAL_EFLAGS;     /* con il flag di interrupt acceso   */
    *--sp = 0;                       /* ebp: nessuno ci ha messo niente   */
    *--sp = 0;                       /* ebx                               */
    *--sp = 0;                       /* esi                               */
    *--sp = 0;                       /* edi                               */

    t->esp = (uint32_t)sp;
    t->state = TASK_READY;

    return slot;
}

int task_current(void)
{
    return current_task;
}

struct task *task_slot(int i)
{
    if (i < 0 || i >= MAX_TASKS)
        return 0;

    return &tasks[i];
}

int task_next(const struct task *table, int n, int current)
{
    int k;

    /* L'offset parte da 1, cosi' current e' escluso per costruzione, e si
       ferma a n-1, cosi' ogni altra casella viene esaminata una volta sola.
       Le due condizioni al contorno si risolvono da se'. */
    for (k = 1; k < n; k++) {
        int i = (current + k) % n;

        if (table[i].state == TASK_READY)
            return i;
    }

    return -1;
}

void schedule(void)
{
    int prev, next;

    next = task_next(tasks, MAX_TASKS, current_task);

    /* Nessun altro pronto: si continua a girare. Non si cede a se' stessi —
       userebbe come new_esp un valore non ancora salvato. */
    if (next < 0)
        return;

    /* prev e next sono due cose distinte: DOVE salvare chi esce, e COSA
       caricare per chi entra. Collassarli in un solo indice fa salvare il
       contesto uscente nella casella di quello entrante, e il conto si
       presenta due cambi di contesto dopo. */
    prev = current_task;
    current_task = next;

    task_switch(&tasks[prev].esp, tasks[next].esp);

    /* La riga qui sotto non viene eseguita "dopo": viene eseguita quando
       qualcuno cede il controllo a noi, che puo' essere molto tempo dopo. */
}

void task_yield(void)
{
    uint32_t flags;

    /* schedule() presuppone gli interrupt disabilitati, e quando la chiama il
       timer lo sono per via dell'interrupt gate. Qui no: senza protezione il
       timer potrebbe scattare fra la scelta di next e il task_switch, e
       salverebbe il contesto nella casella sbagliata — esattamente il guasto
       che la distinzione prev/next esiste per evitare.

       irq_restore viene eseguita al ritorno, cioe' quando qualcuno cede il
       controllo a noi: flags vive sul nostro stack, che resta conservato. */
    flags = irq_save();
    schedule();
    irq_restore(flags);
}
