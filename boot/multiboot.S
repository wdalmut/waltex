/* Header Multiboot 1 ed entry point.
   QEMU cerca la firma nei primi 8 KiB dell'immagine; se la trova, carica il
   kernel, passa in protected mode e salta a _start con:
     eax = 0x2BADB002  (conferma di essere stati avviati da un loader Multiboot)
     ebx = indirizzo della struttura di info sul sistema
   Nessuno stack è impostato: è la prima cosa da fare. */

.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    (1 << 0) | (1 << 1)      /* moduli allineati, info memoria */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM

.section .bss, "aw", @nobits
.align 16
stack_bottom:
    .skip 16384                            /* 16 KiB */
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    movl $stack_top, %esp                  /* lo stack cresce verso il basso */

    pushl %ebx                             /* secondo argomento di kmain */
    pushl %eax                             /* primo argomento di kmain */
    call  kmain

    /* kmain non deve ritornare, ma se lo fa fermiamo la CPU invece di
       eseguire byte arbitrari. */
    cli
1:  hlt
    jmp 1b

.size _start, . - _start

/* Senza questa sezione il linker assume uno stack eseguibile e avvisa. Nel
   kernel la distinzione è teorica (non c'è NX finché non c'è paging), ma un
   build deve restare pulito: un warning tollerato è un warning che smette di
   essere letto. */
.section .note.GNU-stack, "", @progbits

