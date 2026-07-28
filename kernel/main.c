#include "types.h"
#include "serial.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "selftest.h"
#include "kprintf.h"

/* Valore che un loader Multiboot 1 conforme lascia in eax. Se non corrisponde,
   non sappiamo nulla di affidabile sull'ambiente in cui siamo partiti. */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

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
    kprintf("waltex: M4 ok\n");

    /* Da M4 kmain non ritorna piu'. hlt ferma la CPU fino al prossimo
       interrupt, quindi il kernel dorme e si risveglia a ogni tick invece di
       bruciare cicli in un ciclo vuoto.

       Il cli; hlt in fondo a _start resta come rete di sicurezza: se kmain
       ritornasse davvero, quello spegne tutto. */
    for (;;)
        __asm__ volatile ("hlt");
}
