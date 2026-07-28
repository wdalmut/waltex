/* Il cambio di contesto: dieci istruzioni, ed e' tutto il multitasking.
 *
 *   void task_switch(uint32_t *old_esp, uint32_t new_esp);
 *
 * Salva i registri che la ABI ci obbliga a preservare sullo stack che stiamo
 * abbandonando, scrive esp in *old_esp, carica new_esp, e spila i registri
 * dell'altro task. La ret finale non torna al chiamante: torna dove punta il
 * nuovo stack.
 *
 * All'ingresso, subito dopo la call, lo stack e' cosi'
 * (indirizzi crescenti verso il basso):
 *
 *      0(%esp)   indirizzo di ritorno   <- impilato dalla call
 *      4(%esp)   old_esp                <- primo argomento
 *      8(%esp)   new_esp                <- secondo argomento
 */

.section .text
.global task_switch
.type task_switch, @function
task_switch:

    /* I quattro callee-saved della ABI System V i386. Solo questi: eax, ecx
     * ed edx sono caller-saved, quindi chi ci teneva qualcosa li ha gia'
     * salvati da se' prima di chiamarci. */
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi

    /* Quattro push da 4 byte hanno abbassato esp di 16, quindi tutto quello
     * che c'era prima e' ora 16 byte piu' in alto:
     *
     *      0(%esp)   edi                    ]
     *      4(%esp)   esi                    ]  appena impilati
     *      8(%esp)   ebx                    ]
     *     12(%esp)   ebp                    ]
     *     16(%esp)   indirizzo di ritorno
     *     20(%esp)   old_esp
     *     24(%esp)   new_esp
     */

    /* Salva lo stack pointer corrente dove il C ci ha detto di salvarlo.
     * Due istruzioni perche' x86 non sa scrivere un registro all'indirizzo
     * contenuto in una cella di memoria in un colpo solo: prima l'indirizzo
     * finisce in un registro, poi si scrive attraverso di esso. */
    movl 20(%esp), %eax        /* eax = old_esp, cioe' &tasks[cur].esp */
    movl %esp, (%eax)          /* *old_esp = esp   <- il task e' salvato */

    /* La riga che cambia tutto. Da qui in poi ogni riferimento allo stack
     * riguarda un altro flusso di esecuzione: le sue variabili locali, i suoi
     * indirizzi di ritorno, la sua catena di chiamate in corso. Siamo
     * diventati l'altro task. */
    movl 24(%esp), %esp        /* esp = new_esp */

    /* Ordine inverso esatto dei push. Questi valori li ha messi li' l'altro
     * task quando era lui a passare da queste stesse quattro righe — oppure
     * task_create, falsificandoli, se e' la prima volta che gira. */
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp

    /* Su x86 la ret prende l'indirizzo di ritorno DALLO STACK, non da un
     * registro di link. Avendo cambiato stack, torniamo dove era arrivato
     * l'altro task. E' il fatto su cui poggia tutto il meccanismo. */
    ret

.size task_switch, . - task_switch

.section .note.GNU-stack, "", @progbits
