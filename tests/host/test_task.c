/* Test della creazione dei task col gcc dell'host.

   Perche' questi test contano piu' degli altri: in ogni altra milestone un
   errore produceva un'eccezione con un dump leggibile. Qui il guasto tipico e'
   un esp che punta a spazzatura, la ret salta a un indirizzo casuale, e il
   gestore delle eccezioni non ha uno stack funzionante su cui girare. Il
   sintomo e' una tripla fault muta.

   Ma lo stack falsificato e' solo memoria: task_create scrive cinque parole e
   noi possiamo rileggerle, senza mai saltarci dentro. Il layout si verifica
   quando un errore e' ancora leggibile. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "task.h"

static int failures;

static void check(const char *name, int ok)
{
    printf("%s -- %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

/* task.c chiama questa nel cambio di contesto: sull'host non la eseguiamo mai,
   ci serve solo che il link riesca. Se venisse chiamata sarebbe un errore del
   test, quindi lo diciamo forte. */
void task_switch(uint32_t *old_esp, uint32_t new_esp)
{
    (void)old_esp;
    (void)new_esp;
    printf("FAIL -- task_switch non deve essere chiamata dai test host\n");
    failures++;
}

/* Punti d'ingresso finti: non vengono mai eseguiti, ci interessa solo il loro
   indirizzo. */
static void entry_a(void) { }
static void entry_b(void) { }

/* La tabella e' static dentro task.c, quindi non la vediamo. Ma task_create
   restituisce l'indice, e da fuori possiamo verificare il frame solo se
   sappiamo dove sta lo stack. Per questo task.h espone task_stack_of(). */
extern struct task *task_slot(int i);

static void test_creazione(void)
{
    int a, b, i, ok;

    a = task_create(entry_a);
    b = task_create(entry_b);

    check("il primo task_create restituisce un indice valido", a > 0);
    check("il secondo restituisce un indice diverso", b > 0 && b != a);

    /* La tabella ha MAX_TASKS posti, uno dei quali e' il task 0 di kmain.
       Riempiendola, prima o poi si esaurisce. */
    ok = 0;
    for (i = 0; i < MAX_TASKS + 2; i++)
        if (task_create(entry_a) == -1)
            ok = 1;
    check("a tabella piena task_create restituisce -1", ok);
}

static void test_frame(void)
{
    struct task *t;
    uint32_t *frame;
    int idx;

    idx = task_create(entry_a);
    if (idx < 0) {
        check("serve almeno un posto libero per provare il frame", 0);
        return;
    }

    t = task_slot(idx);

    /* Lo stack cresce verso indirizzi decrescenti, quindi la cima e' un byte
       oltre la fine dell'array e il frame sta sotto di essa. */
    check("esp punta TASK_FRAME_WORDS parole sotto la cima dello stack",
          (uint8_t *)(uintptr_t)t->esp ==
              t->stack + TASK_STACK_SIZE - TASK_FRAME_WORDS * 4);

    check("esp e' allineato a 4", (t->esp & 3) == 0);

    frame = (uint32_t *)(uintptr_t)t->esp;

    /* I quattro callee-saved: nessuno ci ha ancora messo niente, quindi zero.
       L'ordine dal basso e' edi, esi, ebx, ebp — l'inverso dei push. */
    check("le quattro parole dei callee-saved sono azzerate",
          frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == 0);

    /* La quinta e' dove la ret salterà. Se questa e' sbagliata, il task parte
       da un indirizzo arbitrario. */
    check("la quinta parola e' il punto d'ingresso",
          frame[4] == (uint32_t)(uintptr_t)entry_a);

    check("il task creato risulta pronto", t->state == TASK_READY);
}

/* Una tabella finta, static perche' otto task da 4 KB sullo stack sarebbero
   32 KB. A task_next interessa solo il campo state. */
static struct task finta[MAX_TASKS];

static void test_round_robin(void)
{
    int i;

    for (i = 0; i < MAX_TASKS; i++)
        finta[i].state = TASK_FREE;

    /* Un solo task pronto: non c'e' nessun altro a cui cedere. */
    finta[0].state = TASK_READY;
    check("con un solo task pronto non c'e' un prossimo",
          task_next(finta, MAX_TASKS, 0) == -1);

    /* Due task: si alternano. */
    finta[3].state = TASK_READY;
    check("da 0 si passa a 3", task_next(finta, MAX_TASKS, 0) == 3);
    check("da 3 si torna a 0", task_next(finta, MAX_TASKS, 3) == 0);

    /* Tre task: l'ordine e' crescente e circolare, e i posti liberi si
       saltano. */
    finta[5].state = TASK_READY;
    check("da 0 si passa a 3, non a 5", task_next(finta, MAX_TASKS, 0) == 3);
    check("da 3 si passa a 5",         task_next(finta, MAX_TASKS, 3) == 5);
    check("da 5 si riavvolge a 0",     task_next(finta, MAX_TASKS, 5) == 0);
}

int main(void)
{
    task_init();

    test_frame();
    test_creazione();
    test_round_robin();

    if (failures == 0) {
        printf("tutti i test dei task passano\n");
        return 0;
    }
    printf("%d test falliti\n", failures);
    return 1;
}
