/* Il kernel vive a 1 MiB: sotto quell'indirizzo la memoria fisica è
   frammentata fra BIOS, aree riservate e memoria video. L'header Multiboot
   deve stare nei primi 8 KiB del file, quindi la sua sezione va per prima. */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot ALIGN(4) : { *(.multiboot) }

    .text   ALIGN(4K) : { *(.text) }
    .rodata ALIGN(4K) : { *(.rodata) }
    .data   ALIGN(4K) : { *(.data) }
    .bss    ALIGN(4K) : { *(COMMON) *(.bss) }

    kernel_end = .;
}
