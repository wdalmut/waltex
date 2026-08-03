#include "panic.h"
#include "types.h"
#include "kprintf.h"
#include "vga.h"
#include "serial.h"

/* cli perche' nessun interrupt ci risvegli, hlt per fermare la CPU. Il ciclo
   c'e' perche' hlt puo' comunque terminare per un NMI, che cli non maschera:
   in quel caso si rientra e ci si ferma di nuovo. */
static void halt(void) __attribute__((noreturn));

void panic(const char *fmt, ...)
{
    va_list args;

    vga_set_color(VGA_WHITE, VGA_RED);

    /* UNA passata sola, con il sink doppio di kprintf.c.
       Le due chiamate di prima passavano lo STESSO va_list a entrambe, ed era il
       debito di M1 nella sua forma peggiore: kprintf se l'era tolto con un
       va_copy, qui era rimasto. Funziona su i386 perche' li' va_list e' un
       puntatore passato per valore, e su qualunque altra architettura il secondo
       dump e' spazzatura — cioe' un panic che mente, che e' il posto peggiore
       dove averlo. */
    va_start(args, fmt);
    kvprintf(kputc_console, 0, fmt, args);
    va_end(args);

    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    halt();
}

static void halt(void)
{
    for (;;)
        __asm__ volatile ("cli; hlt");
}

void panic_regs(struct regs *r)
{
    vga_set_color(VGA_WHITE, VGA_RED);

    kprintf("\n*** PANIC: %s (vettore %d)\n",
            exception_name(r->vec), (int)r->vec);

    /* Lo stato che dice DOVE e' successo. eip e' il campo piu' prezioso del
       dump: e' l'indirizzo dell'istruzione colpevole. */
    kprintf("    eip=%x  cs=%x  eflags=%x  err=%x\n",
            r->eip, r->cs, r->eflags, r->err);

    /* Lo stato che dice CON QUALI DATI. */
    kprintf("    eax=%x  ebx=%x  ecx=%x  edx=%x\n",
            r->eax, r->ebx, r->ecx, r->edx);
    kprintf("    esi=%x  edi=%x  ebp=%x  esp=%x\n",
            r->esi, r->edi, r->ebp, r->esp);
    kprintf("    ds=%x\n", r->ds);

    /* Il comando da incollare nel terminale per avere file e riga. */
    kprintf("    addr2line -e build/waltex.elf %x\n", r->eip);

    halt();
}