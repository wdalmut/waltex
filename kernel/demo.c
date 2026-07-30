#include "demo.h"
#include "types.h"
#include "task.h"
#include "kprintf.h"

/* I due task di prova di M6a-M6b, tirati fuori da main.c in M7 perche' main.c
   torni a essere la sola sequenza di boot.

   Da M7 partono SILENZIOSI: la loro stampa continua era il punto della
   milestone della prelazione, ma rende una shell illeggibile. Li accende il
   comando "spin", e tests/tasks.sh se lo manda da se' — quindi adesso il test
   provoca la condizione che misura, invece di appoggiarsi a un effetto
   collaterale del kernel. */

/* volatile, e qui non e' cautela: la scrive shell_spin, la leggono i due task
   dentro un ciclo che non fa nient'altro. Senza volatile il compilatore e'
   autorizzato a leggerla una volta sola prima del ciclo e tenerla in un
   registro, e i task non vedrebbero mai il cambiamento. */
static volatile int spinning;

/* Nessuna task_yield: e' precisamente il punto di M6b. L'alternanza che si vede
   sulla seriale e' la prova che il controllo viene TOLTO cento volte al secondo
   mentre erano nel mezzo di una kprintf. */
static void task_a(void)
{
    for (;;) {
        if (spinning)
            kprintf("A");
    }
}

static void task_b(void)
{
    for (;;) {
        if (spinning)
            kprintf("B");
    }
}

void demo_tasks_init(void)
{
    task_create(task_a);
    task_create(task_b);
}

void demo_tasks_start(void)
{
    spinning = 1;
}
