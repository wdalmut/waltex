#include "gdt.h"

/* I 48 punti d'ingresso: 32 per le eccezioni della CPU, 16 per gli IRQ
   hardware rimappati sui vettori 32-47.

   Il problema che risolvono. Quando arriva un'interruzione la CPU impila
   eflags, cs ed eip — e per alcune eccezioni anche un codice d'errore, per
   altre no. Lo stack all'ingresso ha quindi due forme diverse, e il codice C
   dovrebbe distinguerle. Gli stub uniformano: dove la CPU non ha impilato
   niente, impilano uno zero fittizio. Cosi' isr_handler vede sempre lo stesso
   struct regs.

   Le eccezioni che impilano un codice d'errore su i386 sono la 8, 10, 11, 12,
   13, 14 e 17. Tutte le altre no.

   Nessun cli negli stub: i gate dell'IDT sono interrupt gate, e la CPU azzera
   gia' il flag di interrupt entrando. */

/* Senza codice d'errore: ne impiliamo uno finto per uniformare. */
.macro ISR_NOERR num
.global isr\num
.type isr\num, @function
isr\num:
    pushl $0
    pushl $\num
    jmp isr_common
.size isr\num, . - isr\num
.endm

/* Con codice d'errore: lo ha gia' impilato la CPU. */
.macro ISR_ERR num
.global isr\num
.type isr\num, @function
isr\num:
    pushl $\num
    jmp isr_common
.size isr\num, . - isr\num
.endm

/* IRQ hardware: mai un codice d'errore. Il numero della linea e il vettore
   sono diversi — la linea 0 arriva sul vettore 32. */
.macro IRQ num, vec
.global irq\num
.type irq\num, @function
irq\num:
    pushl $0
    pushl $\vec
    jmp isr_common
.size irq\num, . - irq\num
.endm

.section .text

ISR_NOERR  0
ISR_NOERR  1
ISR_NOERR  2
ISR_NOERR  3
ISR_NOERR  4
ISR_NOERR  5
ISR_NOERR  6
ISR_NOERR  7
ISR_ERR    8
ISR_NOERR  9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

IRQ  0, 32
IRQ  1, 33
IRQ  2, 34
IRQ  3, 35
IRQ  4, 36
IRQ  5, 37
IRQ  6, 38
IRQ  7, 39
IRQ  8, 40
IRQ  9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* Il tronco comune. Completa il salvataggio dello stato, passa al C un
   puntatore a quello che il C vede come struct regs, e ripristina tutto.

   L'ordine dei push qui e l'ordine dei campi in struct regs sono la stessa
   informazione scritta in due posti: vanno tenuti allineati a mano. */
.type isr_common, @function
isr_common:
    pusha                        /* eax ecx edx ebx esp ebp esi edi */

    /* Salva ds. pusha ha gia' messo al sicuro eax, quindi possiamo usarlo.
       L'azzeramento serve perche' i 16 bit alti di un registro di segmento
       letto in un registro a 32 bit non sono definiti. */
    xorl %eax, %eax
    movw %ds, %ax
    pushl %eax

    /* L'interruzione puo' essere arrivata con segmenti dati qualsiasi: il
       codice C ha bisogno dei nostri. */
    movw $GDT_SEL_DATA, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    /* esp punta ora al primo campo di struct regs. */
    pushl %esp
    call isr_handler
    addl $4, %esp

    popl %eax                    /* il ds di prima */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    popa
    addl $8, %esp                /* scarta il vettore e il codice d'errore */
    iret
.size isr_common, . - isr_common

.section .note.GNU-stack, "", @progbits
