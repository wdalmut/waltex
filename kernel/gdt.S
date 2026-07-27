#include "gdt.h"

/* Carica la GDT e ricarica tutti i selettori di segmento.
   I registri di segmento contengono una copia nascosta del descrittore,
   caricata all'ultimo assegnamento: finche' non li si riscrive, la CPU
   continua a usare i vecchi descrittori anche dopo lgdt. */

.section .text
.global gdt_flush
.type gdt_flush, @function
gdt_flush:
    movl 4(%esp), %eax           /* primo argomento: puntatore al gdtr */
    lgdt (%eax)

    /* I selettori di dati si ricaricano con una mov. */
    movw $GDT_SEL_DATA, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* cs no: non e' scrivibile con mov. L'unico modo di cambiarlo e' un
       trasferimento di controllo che porti con se' un nuovo selettore, cioe'
       un far jump. Il salto e' all'istruzione successiva, ma passa da
       GDT_SEL_CODE e questo e' l'unico scopo. */
    ljmp $GDT_SEL_CODE, $1f
1:
    ret

.size gdt_flush, . - gdt_flush

.section .note.GNU-stack, "", @progbits
