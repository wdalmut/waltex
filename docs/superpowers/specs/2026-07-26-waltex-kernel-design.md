# waltex — design di un kernel didattico i386

Data: 2026-07-26
Stato: approvato

## Obiettivo

Scrivere da zero un kernel monolitico minimo in C per x86 32-bit, avviabile in
virtualizzazione, che arrivi fino al multitasking preemptive: due task che si
alternano perché lo decide uno scheduler nostro. È il punto in cui Linus
Torvalds, nel 1991, si accorse di avere un kernel invece di un terminal
emulator.

L'obiettivo primario è **capire**, non produrre software riutilizzabile. Ogni
scelta di design privilegia la comprensibilità e la velocità del ciclo
prova-errore rispetto a completezza, portabilità e realismo.

Criterio di successo: al termine, l'autore sa spiegare senza guardare il codice
cosa contiene una GDT, cosa accade fra un `int` e l'esecuzione dell'handler,
perché il PIC va rimappato, e cosa esattamente costituisce un "processo" dal
punto di vista della CPU.

## Scelte fondanti

**Architettura i386 (x86 32-bit protected mode).** La stessa di Linux 0.01.
GRUB e QEMU consegnano la CPU già in protected mode, quindi si salta il real
mode e i 16-bit, ma restano da scrivere GDT, IDT e PIC, che sono esattamente i
concetti da imparare. La documentazione disponibile (OSDev wiki, tutorial
storici) usa in massima parte questo target.

Scartate: x86_64 long mode, che richiede di allestire il paging a 4 livelli
prima di eseguire una riga di C utile; RISC-V/ARM64 su QEMU virt, più puliti ma
lontani dallo spirito del progetto e con meno materiale.

**Boot via header Multiboot 1, caricato da `qemu-system-i386 -kernel`.** Il
ciclo build-and-run è di pochi secondi, non serve costruire una ISO e non
servono pacchetti aggiuntivi. Una ISO avviabile con GRUB resta come appendice
opzionale a progetto funzionante.

Scartate: bootloader proprio in assembly 16-bit (fedele a Linus e molto
istruttivo, ma giorni di debug su codice che poi si butta); ISO con GRUB fin
dall'inizio (ciclo di iterazione più lento a ogni test).

**Costruzione a milestone verticali incrementali.** A ogni milestone il kernel
booota e mostra qualcosa; ogni milestone è un commit con un test che passa. In
kernel dev non ci sono debugger, printf o stack trace per default: l'unico
sintomo di un errore è una tripla fault che riavvia la VM. La bisezione per
milestone è lo strumento di debug principale.

Scartata: costruire tutti i sottosistemi e poi accendere, che al primo avvio
produce un reboot loop silenzioso con cinque sospetti contemporanei.

**Modalità di lavoro: scheletro dell'assistente, cuore dell'autore.** Claude
scrive build system, linker script, boilerplate assembly meccanico e
l'infrastruttura di test. L'autore scrive i moduli concettualmente rilevanti,
con Claude che spiega prima e fa code review dopo. La ripartizione precisa dei
file è in `CLAUDE.md`, che è la fonte di verità operativa.

**Linux 0.01 come lettura di accompagnamento**, non come struttura da seguire.
Ogni milestone indica il file storico corrispondente.

## Architettura

### Toolchain

gcc di sistema in modalità freestanding, senza cross-compiler dedicato:

```make
CFLAGS  = -m32 -ffreestanding -nostdlib -fno-pie -fno-stack-protector \
          -fno-builtin -Wall -Wextra -std=gnu11 -g
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib
```

Assembly in sintassi GNU as (file `.S`, assemblati via gcc), non nasm: una
dipendenza in meno a parità di concetti. Kernel linkato a 1 MiB (`0x100000`),
l'indirizzo canonico Multiboot.

Un cross-compiler `i686-elf-gcc` sarebbe la scelta ortodossa, perché il gcc di
sistema può in teoria emettere riferimenti alla libc dell'host. Con
`-ffreestanding -nostdlib -fno-builtin` il rischio è nullo per questo scope, e
si risparmia la build di binutils+gcc. Se in futuro servisse, si aggiunge senza
toccare il codice del kernel.

Prerequisiti da installare sulla macchina: `qemu-system-x86`. Per l'appendice
ISO servirebbero anche `grub-pc-bin` e `xorriso` (l'installazione GRUB presente
ha solo i moduli `x86_64-efi`, non `i386-pc`).

### Layout dei file

Un `.c` più un `.h` per sottosistema, ognuno con una responsabilità sola e una
funzione di init esplicita.

```text
waltex/
├── Makefile
├── linker.ld
├── CLAUDE.md
├── boot/
│   └── multiboot.S      header Multiboot, _start, stack, salto in kmain
├── kernel/
│   ├── main.c           kmain: la sequenza di boot, leggibile in 20 righe
│   ├── vga.c            text mode 80x25 su 0xB8000
│   ├── serial.c         COM1 — il canale su cui girano i test
│   ├── kprintf.c        formatter minimo (%d %x %s %c %%)
│   ├── gdt.c   gdt.S    segmentazione
│   ├── idt.c   isr.S    tabella interrupt, stub per exception e IRQ
│   ├── pic.c            remap del 8259
│   ├── timer.c          PIT a 100 Hz
│   ├── keyboard.c       scancode set 1 → ASCII, ring buffer
│   ├── task.c  switch.S task e context switch
│   ├── panic.c          dump dei registri e halt
│   └── selftest.c       self-check eseguiti dentro la VM, uno per milestone
├── include/
│   ├── types.h          uint8_t … uintptr_t, definiti a mano
│   ├── io.h             inb/outb/io_wait come inline asm
│   └── panic.h          panic(), assert()
├── tests/
│   ├── host/            test della logica pura, compilati col gcc host
│   └── smoke.sh         avvio QEMU headless e match sui marker seriali
└── docs/
```

### Interfacce pubbliche

Tutto il resto è interno al modulo.

```c
/* vga.h */
void vga_init(void);
void vga_putc(char c);
void vga_clear(void);

/* serial.h */
void serial_init(void);
void serial_putc(char c);

/* kprintf.h */
void kprintf(const char *fmt, ...);   /* scrive su VGA e su COM1 */

/* Il cuore del formatter è separato dal sink, in modo che sia compilabile e
   testabile col gcc dell'host senza VGA né seriale: */
void kvprintf(void (*putc)(char), const char *fmt, va_list ap);

/* gdt.h */
void gdt_init(void);

/* idt.h */
struct regs;                          /* stato salvato dagli stub in isr.S */
void idt_init(void);                  /* installa i gate e rimappa il PIC */
void irq_register(uint8_t irq, void (*handler)(struct regs *));

/* timer.h */
void timer_init(uint32_t hz);
uint32_t timer_ticks(void);

/* keyboard.h */
void keyboard_init(void);
int  keyboard_getchar(void);          /* -1 se il buffer è vuoto */

/* task.h */
void task_init(void);
int  task_create(void (*entry)(void)); /* -1 se la tabella è piena */
void task_yield(void);                 /* milestone 6a */
void schedule(void);                   /* chiamato dall'handler del timer */
```

Il punto chiave: `irq_register` fa sì che `timer.c` e `keyboard.c` non sappiano
nulla dell'IDT, e `schedule()` non sappia nulla del PIT. Ogni modulo si può
capire, e rompere, isolatamente.

### Flusso di boot

```text
QEMU (-kernel, header Multiboot)
  └─ _start            monta lo stack
       └─ kmain
            ├─ vga_init, serial_init
            ├─ kprintf  banner
            ├─ gdt_init
            ├─ idt_init          (rimappa anche il PIC)
            ├─ timer_init(100)
            ├─ keyboard_init
            ├─ task_init, task_create(task_a), task_create(task_b)
            ├─ sti
            └─ loop: hlt
```

Dopo la `sti` il controllo del flusso passa agli interrupt: IRQ0 dal PIT chiama
il tick e poi `schedule()`, IRQ1 dalla tastiera riempie il ring buffer.

## Milestone

Ognuna termina con un kernel che booota, un commit e un test che passa.

### M1 — Boot e schermo

Claude: `Makefile`, `linker.ld`, `boot/multiboot.S`, scheletro di `main.c`,
`serial.c`, `include/io.h`, `include/types.h`.
Autore: `vga.c` (scrittura carattere, newline, scroll, cursore hardware),
`kprintf.c`.

Verifica: la finestra QEMU mostra il banner; lo smoke test lo ritrova su
seriale. Test host sul formatter di `kprintf`.

Concetti: header Multiboot, linker script e sezioni `.text/.rodata/.bss`,
memory-mapped I/O, cosa significa "freestanding".

### M2 — GDT

Claude: `gdt.S` (`lgdt` e ricaricamento dei selettori di segmento).
Autore: `gdt.c`, il bit-packing dei descrittori — null, code ring 0, data
ring 0.

Verifica: il kernel continua a stampare dopo il reload dei segmenti. Un bit
sbagliato produce una tripla fault e un riavvio, che è già una lezione.

Concetti: segmentazione, struttura di un descrittore, ring di privilegio, far
jump per applicare `cs`.
Lettura laterale: `head.s` e `include/asm/system.h` di Linux 0.01.

### M3 — IDT, exception, PIC

La milestone più lunga, e quella che fornisce gli strumenti di diagnosi per
tutte le successive.

Claude: i 48 stub in `isr.S` (generati da macro, lavoro meccanico) e la
`struct regs`.
Autore: `idt_set_gate` e `idt_init`, `pic.c` con il remap a 0x20/0x28,
`panic.c` con il dump dei registri.

Verifica: un `int $3` volontario viene gestito senza riavvio; una divisione per
zero produce un dump con l'EIP corretto.

Concetti: gate dell'IDT, salvataggio e ripristino dello stato, differenza fra
exception e IRQ, perché il PIC va rimappato (di default gli IRQ 0-7 atterrano
sui vettori delle exception 8-15).
Lettura laterale: `kernel/traps.c` di Linux 0.01.

### M4 — Timer PIT

Autore: `timer.c` — divisore `1193182/hz`, canale 0 in mode 3, handler
registrato con `irq_register` che incrementa il contatore e manda l'EOI.
Claude: nulla, a parte review.

Verifica: dopo un secondo di attesa i tick sono 100 ± 5.

Concetti: come si programma una frequenza, cos'è l'EOI, quanto pochissimo deve
fare un interrupt handler.
Lettura laterale: `do_timer` in `kernel/sched.c` di Linux 0.01.

### M5 — Tastiera

Autore: `keyboard.c` — scancode set 1, gestione di shift e dei break code,
ring buffer da 128 byte fra handler e codice normale.
Claude: l'harness che inietta i tasti in QEMU via QMP `sendkey`.

Verifica: il test digita `w a l t e r` e ritrova `walter` sulla seriale. Test
host sulla tabella scancode→ASCII.

Concetti: il ring buffer come confine fra contesto di interrupt e codice
normale — la prima concorrenza vera del progetto.
Lettura laterale: `kernel/keyboard.s` di Linux 0.01, scritto interamente in
assembly.

### M6 — Multitasking

Autore: `task.c` e `switch.S`, con Claude che spiega il layout dello stack
prima e fa review dopo. È il pezzo più insidioso del progetto: un offset
sbagliato non produce un errore diagnosticabile, produce un salto a un
indirizzo arbitrario.

**M6a, cooperativo.** `struct task { uint32_t esp; uint8_t stack[4096]; int
state; }`, `switch.S` che salva e ripristina ESP e i registri callee-saved,
`task_yield()`. Due task stampano A e B chiamando `task_yield()` a mano.

**M6b, preemptive.** `schedule()` invocato dall'handler del timer, round-robin
sulla tabella dei task. Rimosse le chiamate a `task_yield()`: l'alternanza
prosegue perché la decide lo scheduler.

Verifica: almeno 20 alternanze A→B→A nell'output seriale. Test host sulla
funzione di scelta del prossimo task.

Concetti: un task è uno stack più un ESP salvato; perché il context switch deve
stare in assembly; le race fra handler e codice normale, e il ruolo di
`cli`/`sti`.
Lettura laterale: `include/linux/sched.h` di Linux 0.01 e la macro `switch_to`.

## Verifica

`kprintf` scrive su due canali: VGA per l'osservazione umana, COM1 per i test.
QEMU ridirige COM1 su stdout, quindi un kernel dentro una VM diventa un normale
processo che stampa righe su cui fare match. È la decisione che rende il
progetto testabile automaticamente.

Target del Makefile:

```text
make run     finestra QEMU, output VGA visibile
make test    headless, seriale su stdout, timeout, exit code
make debug   qemu -s -S, in attesa di gdb
```

Due livelli di test.

**Test host, senza QEMU.** La logica pura — formatter di `kprintf`, tabella
scancode→ASCII, scelta del prossimo task — viene compilata anche col gcc
dell'host e verificata con `assert` normali. Ciclo da millisecondi, ed è dove
si può lavorare test-first.

**Smoke test in QEMU.** Per la glue hardware, che esiste solo dentro la VM:
`tests/smoke.sh` avvia il kernel headless, attende sulla seriale i marker
attesi per quella milestone entro un timeout, e fallisce se non arrivano. Il
kernel può terminare con un exit status reale scrivendo sulla porta `0xf4`
(`-device isa-debug-exit`), così `make test` restituisce un codice onesto
invece di dipendere solo dal grep.

## Gestione degli errori

In un kernel non esistono eccezioni né `errno`. La strategia è tre cose.

1. `panic(fmt, ...)` stampa file, riga, messaggio e dump dei registri, poi
   `cli; hlt`. Non ritorna mai.
2. Gli handler delle exception CPU (da M3) chiamano `panic` invece di lasciar
   propagare la fault. Senza questo ogni bug si manifesta come una VM che
   riparte in silenzio.
3. `assert()` sempre attivo, mai compilato via. Nel kernel proseguire dopo
   un'asserzione violata significa corrompere memoria arbitraria.

**Nessuna allocazione dinamica in tutto lo scope.** Nessun heap, nessun
`malloc`: array statici a capacità fissa (`MAX_TASKS 8`, ring buffer tastiera
128 byte). `task_create` restituisce `-1` a tabella piena. Un allocatore è un
progetto a sé e non serve per arrivare al multitasking.

## Strumenti di debug

Disponibili dall'inizio, non da aggiungere in emergenza:

- `-no-reboot`: una tripla fault ferma la VM invece di ciclare.
- `-d int,cpu_reset`: logga ogni interrupt e ogni reset con lo stato della CPU.
- `-s -S` più `gdb`, `target remote :1234`, `symbol-file build/waltex.elf`:
  breakpoint e `info registers` sul kernel. Il kernel è compilato con `-g`
  proprio per questo.

## Fuori scope

Esplicitamente non inclusi, per tenere il progetto a misura di comprensione:
paging e memoria virtuale, separazione kernel/user mode e syscall, allocatore
dinamico, filesystem, driver di disco, ELF loader, SMP, floating point. Ognuno
di questi merita un progetto e uno spec propri.

Appendice opzionale, da affrontare solo a progetto funzionante: costruzione di
una ISO avviabile con GRUB (`grub-pc-bin`, `xorriso`) per sostituire `-kernel`
con un boot realistico.
