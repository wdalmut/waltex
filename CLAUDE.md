# waltex

Kernel monolitico didattico per x86 32-bit, scritto da zero, avviato in QEMU.
Obiettivo: arrivare al multitasking preemptive capendo ogni pezzo. Non è
software da riutilizzare, è un progetto per imparare.

Design completo e motivazioni delle scelte:
`docs/superpowers/specs/2026-07-26-waltex-kernel-design.md`. Leggilo prima di
proporre modifiche architetturali.

Rispondi in italiano.

## Stato corrente

Milestone in corso: **M2 — GDT**. M1 chiusa: boot Multiboot, VGA text mode con
scroll, cursore hardware e colore corrente, seriale COM1, `kprintf`,
`memcpy`/`memset`/`memset16`, 40 test host e 16 self-check in QEMU.

Debiti tecnici lasciati aperti da M1, da saldare quando toccano:

- lo scroll usa `memcpy` su regioni sovrapposte: funziona per la direzione
  attuale, ma è comportamento indefinito — serve `memmove` o un ciclo su celle.
  È anche l'ultimo punto di `vga.c` che scarta il `volatile` del framebuffer;
- `kprintf` formatta due volte, una per sink, riusando lo stesso `va_list`:
  legale su i386 dove `va_list` è un puntatore passato per valore, non
  altrove. Una passata sola con un sink doppio lo risolverebbe;
- `put_uint` tratta la base 10 come con segno, quindi non può stampare
  decimali senza segno sopra 2³¹.

Le milestone sono M1 boot+VGA, M2 GDT, M3 IDT+exception+PIC, M4 timer PIT,
M5 tastiera, M6a multitasking cooperativo, M6b preemptive. Aggiorna questa
sezione quando una milestone viene chiusa.

## Regola non negoziabile: chi scrive cosa

Walter sta scrivendo questo kernel per capirlo. Se Claude scrive i moduli
concettualmente rilevanti, il progetto perde il suo scopo.

**File che Claude NON scrive.** Su questi Claude spiega i concetti prima,
risponde a domande durante, e fa code review dopo — ma non produce
l'implementazione, nemmeno parzialmente, nemmeno "come esempio da cui partire",
nemmeno se richiesto in modo generico come "fammi vedere come si fa":

```text
kernel/vga.c        kernel/kprintf.c    kernel/gdt.c      kernel/idt.c
kernel/pic.c        kernel/panic.c      kernel/timer.c    kernel/keyboard.c
kernel/task.c       kernel/switch.S
```

**File che Claude scrive e mantiene** (infrastruttura: non insegna nulla e
costa solo tempo):

```text
Makefile            linker.ld           boot/multiboot.S    kernel/main.c
kernel/serial.c     kernel/selftest.c   include/types.h     include/io.h
include/panic.h     kernel/gdt.S        kernel/isr.S        tests/**
docs/**
```

Gli header dei moduli di Walter (`vga.h`, `idt.h`, `task.h`, …) li scrive
Claude: sono contratti d'interfaccia, già fissati nello spec, e servono per
compilare.

**Se Walter chiede esplicitamente il codice di un file della prima lista:**
prima chiedi se preferisce un suggerimento a parole o una spiegazione del
concetto sottostante. Se confermano di volere il codice, scrivilo — la
decisione è loro. Ma la richiesta va fatta una volta, non ogni volta.

Zona intermedia: se Walter è bloccato, la scala è pseudocodice → una singola
riga chiave → il registro o il bit specifico da guardare. In quest'ordine, non
partendo dalla fine.

## Build ed esecuzione

```text
make          build/waltex.elf
make run      QEMU con finestra, output VGA visibile
make test     headless: test host + smoke test in QEMU, exit code reale
make debug    qemu -s -S in attesa di gdb sulla :1234
make clean
```

Prerequisito: `qemu-system-x86` (`sudo apt install qemu-system-x86`). Non c'è
cross-compiler: si usa il gcc di sistema con `-m32 -ffreestanding`.

Dopo ogni modifica al codice del kernel esegui `make test`, non solo `make`.
Un kernel che compila non è un kernel che booota.

## Vincoli del codice

Questo è codice freestanding: **non esiste la libc**. Niente `stdio.h`,
`string.h`, `stdlib.h`, `assert.h`. I tipi vengono da `include/types.h`, scritto
a mano. Se serve `memset` o `strlen`, si scrivono.

- **Nessuna allocazione dinamica**, in nessuna milestone. Array statici a
  capacità fissa. `MAX_TASKS 8`, ring buffer tastiera 128 byte.
- **Assembly in sintassi GNU as** (file `.S`), mai nasm.
- `assert()` è sempre attivo e chiama `panic()`. Non introdurre `NDEBUG`.
- Nessun `float`/`double`: l'FPU non è inizializzata.
- Ogni sottosistema ha una `*_init()` esplicita, chiamata da `kmain` in ordine
  visibile. Nessuna inizializzazione implicita o lazy.
- `kprintf` scrive su VGA **e** su COM1. La seriale è ciò che leggono i test:
  non aggiungere output diagnostico solo su VGA.

## Disciplina delle milestone

Una milestone alla volta. Non anticipare codice di milestone successive,
nemmeno se "tanto poi serve": lo scopo della struttura incrementale è che
quando qualcosa si rompe la superficie di sospetto sia di poche decine di
righe.

Ogni milestone si chiude con: kernel che booota, smoke test verde, un commit.
Il commit lo **proponi** quando i test passano, con un messaggio nella forma
`M3: idt, exception handler, remap PIC` — ma non eseguirlo se Walter non
conferma.

## Review: cosa guardare in questo dominio

Quando fai code review dei moduli di Walter, i bug che contano qui non sono
quelli di stile. In ordine di quanto costano da diagnosticare:

- Bit-packing dei descrittori GDT/IDT: limit, granularità, access byte,
  la parte alta della base o dell'offset dimenticata.
- EOI al PIC mancante o inviato al PIC sbagliato per gli IRQ 8-15: il primo
  interrupt arriva, il secondo mai.
- Offset dello stack in `switch.S` non allineati a cosa `struct task` contiene
  davvero, e ordine di push/pop asimmetrico.
- Accesso a stato condiviso fra handler e codice normale senza `cli`/`sti`
  (ring buffer della tastiera, tabella dei task), o `sti` fatto troppo presto.
- Variabili toccate da un handler non dichiarate `volatile`.
- `struct` dei descrittori senza `__attribute__((packed))`: il compilatore
  inserisce padding e l'hardware legge spazzatura.
- Handler che fanno troppo lavoro, o che chiamano `kprintf` dove non serve.

## Debug

Non debuggare a tentativi. Gli strumenti esistono già:

- `make debug` più `gdb`, `target remote :1234`,
  `symbol-file build/waltex.elf`. Il kernel è compilato con `-g`.
- `-d int,cpu_reset` nei flag QEMU logga ogni interrupt e ogni reset con lo
  stato della CPU: è così che si identifica una tripla fault.
- `-no-reboot` è già attivo: la VM si ferma invece di ciclare.

Se il sintomo è "la VM riparte in silenzio", il sospetto quasi sempre è nella
milestone appena scritta, ed è un problema di tabelle o di stack, non di logica.

## Lettura di accompagnamento

Linux 0.01 è materiale di lettura per milestone (`head.s`, `kernel/traps.c`,
`kernel/sched.c`, `kernel/keyboard.s`, `include/linux/sched.h`), non una
struttura da seguire. Non proporre di portare quel codice.
