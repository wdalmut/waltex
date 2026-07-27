#include "types.h"
#include "serial.h"
#include "vga.h"
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

    failures = selftest_run();
    if (failures != 0) {
        kprintf("waltex: %d selftest falliti\n", failures);
        return;
    }
    kprintf("waltex: selftest ok\n");

    /* Ultima riga di kmain. Il marker che lo smoke test cerca deve significare
       "tutto quello che precede ha funzionato": spostarlo piu' in alto lo
       trasforma in una decorazione che resta verde anche a kernel rotto. */
    kprintf("waltex: M1 ok\n");
}
