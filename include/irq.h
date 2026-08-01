#ifndef WALTEX_IRQ_H
#define WALTEX_IRQ_H

#include "types.h"

/* Sezioni critiche.

   Da M6b qualunque punto del codice puo' essere interrotto, quindi qualunque
   stato condiviso puo' essere osservato a metà aggiornamento. Dove la
   struttura dei dati non basta a garantire la correttezza — come nel ring
   buffer, che ha un solo scrittore per indice — serve impedire che l'interrupt
   cada nel mezzo.

   Perche' non basta cli/sti. Una funzione protetta puo' essere chiamata anche
   da dentro un interrupt handler, dove gli interrupt sono GIA' disabilitati:
   vga_putc chiamata da panic_regs e' il caso concreto. Un sti finale li
   riabiliterebbe in un contesto che li aveva spenti deliberatamente, e il
   panic si farebbe interrompere a metà dump.

   Quindi non si abilita: si RIPRISTINA lo stato precedente.

       uint32_t f = irq_save();
       ... poche istruzioni ...
       irq_restore(f);

   La sezione va tenuta corta: con gli interrupt spenti il timer perde tick, e
   il self-check di M4 sulla frequenza lo noterebbe. */

#ifdef WALTEX_HOSTED

/* Sull'host le due funzioni non fanno niente, e non e' una comodita': `cli` e'
   un'istruzione privilegiata, quindi in user space e' una violazione di
   protezione. Un test host che chiamasse la versione vera morirebbe di SIGSEGV —
   verificato, exit 139.

   Senza questo ramo, nessun test host potrebbe esercitare codice che contiene
   una sezione critica: e da M9a il VFS ne ha una nell'allocazione degli slot,
   attraversata da ogni singola vfs_open.

   Le due versioni non sono equivalenti, e va saputo: sull'host la protezione non
   c'e'. Ma sull'host non c'e' nemmeno da cosa proteggersi, perche' non esistono
   interrupt — quindi cio' che il test verifica resta vero. */

static inline uint32_t irq_save(void)
{
    return 0;
}

static inline void irq_restore(uint32_t flags)
{
    (void)flags;
}

#else

/* Disabilita gli interrupt e restituisce lo stato precedente di eflags. */
static inline uint32_t irq_save(void)
{
    uint32_t flags;

    /* pushfl impila eflags, popl lo porta in una variabile. Il flag di
       interrupt e' il bit 9. */
    __asm__ volatile ("pushfl\n\t"
                      "popl %0\n\t"
                      "cli"
                      : "=r"(flags)
                      :
                      : "memory");
    return flags;
}

/* Ripristina eflags come era. Se gli interrupt erano gia' spenti, restano
   spenti. */
static inline void irq_restore(uint32_t flags)
{
    __asm__ volatile ("pushl %0\n\t"
                      "popfl"
                      :
                      : "r"(flags)
                      : "memory", "cc");
}

#endif  /* WALTEX_HOSTED */

#endif
