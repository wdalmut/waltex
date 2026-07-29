#include "types.h"
#include "serial.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "task.h"
#include "selftest.h"
#include "kprintf.h"

/* Valore che un loader Multiboot 1 conforme lascia in eax. Se non corrisponde,
   non sappiamo nulla di affidabile sull'ambiente in cui siamo partiti. */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002


/* I due task di prova. Da M6b non cedono piu' il controllo: stampano e
   nient'altro. L'alternanza che si vede sulla seriale e' quindi la prova che
   qualcuno glielo TOGLIE, cento volte al secondo, mentre erano nel mezzo di
   una kprintf e senza che ne sappiano niente.

   Nessuna task_yield qui dentro: e' precisamente il punto della milestone. */
static void task_a(void)
{
    for (;;)
        kprintf("A");
}

static void task_b(void)
{
    for (;;)
        kprintf("B");
}

void kmain(uint32_t magic, void *mbinfo)
{
    int failures;

    (void)mbinfo;

    vga_init();
    serial_init();

    kprintf("waltex: booting\n");

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        kprintf("waltex: magic Multiboot errato: %x\n", magic);
        return;
    }
    kprintf("waltex: multiboot ok\n");

    /* Da qui in poi la CPU usa la nostra tabella dei descrittori invece di
       quella del bootloader. La riga dopo non e' decorativa: se la GDT fosse
       malformata, la CPU non arriverebbe a eseguirla. */
    gdt_init();
    kprintf("waltex: gdt caricata\n");

    /* Prima la tabella dei gestori, poi il chip che generera' gli interrupt.
       Nessuna sti in M3: installiamo la capacita' di gestirli, non ne
       riceviamo ancora. La prima sorgente reale e' il timer, in M4. */
    idt_init();
    pic_init();
    kprintf("waltex: idt e pic pronti\n");

    timer_init(100);
    keyboard_init();
    kprintf("waltex: timer a 100 Hz\n");

    /* La prima sti del progetto. Da questa istruzione il kernel ha due flussi
       di esecuzione: questo, e il gestore del timer che lo interrompe cento
       volte al secondo. Tutto cio' che e' condiviso fra i due va trattato di
       conseguenza. */
    __asm__ volatile ("sti");

    failures = selftest_run();
    if (failures != 0) {
        kprintf("waltex: %d selftest falliti\n", failures);
        return;
    }
    kprintf("waltex: selftest ok\n");

    /* Ultima riga di kmain. Il marker che lo smoke test cerca deve significare
       "tutto quello che precede ha funzionato": spostarlo piu' in alto lo
       trasforma in una decorazione che resta verde anche a kernel rotto. */
    kprintf("waltex: M6b ok\n");
    kprintf("waltex: eco attiva\n");

    /* Da qui kmain e' il task 0: il suo stack e' quello montato da _start, e
       il suo esp verra' scritto dal primo task_switch che lo abbandona.

       kmain non ritorna piu' da M4, e il cli; hlt in fondo a _start resta come
       rete di sicurezza per il caso "e' tornato, non doveva".

       In M6a il ciclo di idle non poteva dormire: in un sistema cooperativo un
       task che si addormenta non cede il controllo a nessuno, quindi doveva
       girare e chiamare task_yield, con la CPU al 100%. Con la prelazione
       quel vincolo cade. */
    task_init();
    task_create(task_a);
    task_create(task_b);

    /* Con la prelazione il ciclo di idle puo' tornare a dormire: non serve piu'
       che qualcuno ceda volontariamente, perche' il timer sveglia e commuta
       comunque. E' cio' che in M6a era impossibile — un task che dorme in un
       sistema cooperativo blocca tutti. */
    for (;;) {
        int c;

        while ((c = keyboard_getchar()) >= 0)
            kprintf("%c", (char)c);

        __asm__ volatile ("hlt");
    }
}
