#ifndef WALTEX_DEMO_H
#define WALTEX_DEMO_H

/* I due task di prova di M6a-M6b, tirati fuori da main.c in M7.

   Da M7 partono SILENZIOSI: stampavano 'A' e 'B' in continuazione, il che era
   il punto della milestone della prelazione ma rende una shell illeggibile.

   Restano perche' tests/tasks.sh misura su di loro le due proprieta' che
   distinguono la prelazione dalla cooperazione: che le transizioni siano molte,
   e che le corse siano lunghe. Il test li accende da se' mandando "spin" dal
   prompt, quindi adesso provoca la condizione che misura invece di appoggiarsi
   a un effetto collaterale del kernel. */

/* Crea i due task. Da chiamare dopo task_init(). */
void demo_tasks_init(void);

/* Accende la stampa. La chiama il comando "spin" della shell. */
void demo_tasks_start(void);

#endif
