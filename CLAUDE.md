# waltex

Kernel monolitico didattico per x86 32-bit, scritto da zero, avviato in QEMU.
Obiettivo: arrivare a un sistema di forma Unix — device, filesystem, shell come
processo utente in ring 3 — capendo ogni pezzo. Non è software da riutilizzare,
è un progetto per imparare.

Design completo e motivazioni delle scelte, in due spec:

- primo blocco (M1-M6b, kernel):
  `docs/superpowers/specs/2026-07-26-waltex-kernel-design.md`
- secondo blocco (M7-M17, userland):
  `docs/superpowers/specs/2026-07-29-waltex-userland-design.md`

Leggili prima di proporre modifiche architetturali.

Rispondi in italiano.

## Stato corrente

Stato: **primo blocco chiuso, secondo blocco progettato e non iniziato.**
Le prime sei milestone sono chiuse; M7 è la prossima.

M1 chiusa: boot Multiboot, VGA text mode con scroll, cursore hardware e colore
corrente, seriale COM1, `kprintf`, `memcpy`/`memset`/`memset16`.
M2 chiusa: GDT propria a tre descrittori piatti ring 0, caricata con `lgdt` e
far jump, verificata rileggendo la tabella con `sgdt`.
M3 chiusa: IDT a 256 gate, 48 stub in assembly, PIC rimappato a 32-47,
`panic` con dump dei registri. Un'eccezione ora produce nome, `EIP` e registri
invece di una tripla fault muta.

M4 chiusa: PIT a 100 Hz sull'IRQ 0, prima `sti` del progetto, `kmain` che non
ritorna piu' ma dorme in `hlt`. Frequenza verificata contro l'orologio CMOS,
che e' un riferimento indipendente: 100 tick misurati in un secondo reale.

M5 chiusa: driver tastiera sull'IRQ 1, decodifica dello scancode set 1, e un
ring buffer a produttore singolo fra il gestore e il codice normale. `kmain`
fa l'eco di quello che si digita. La catena completa e' verificata iniettando
tasti nel monitor di QEMU con `tests/keyboard.sh`.

M6a chiusa: multitasking cooperativo. Tre flussi di esecuzione — `kmain`,
`task_a`, `task_b` — su tre stack separati, che si passano il controllo con
`task_yield()`.
M6b chiusa: multitasking preemptive. Il gestore del timer chiama `schedule()`,
i task non cedono piu' niente e il controllo viene tolto cento volte al
secondo. `tests/tasks.sh` verifica due proprieta' distinte: che le transizioni
siano molte, e che le corse siano LUNGHE — corse di lunghezza 1 vorrebbero dire
che i task stanno ancora cedendo volontariamente, cioe' che la prelazione non
c'e'.

Stato dei test: 99 host, 40 self-check in QEMU, 6 marker, piu' i test di
tastiera e task dentro la VM.

Nota: da M6a i due task di prova stampano `A` e `B` in continuazione, quindi la
seriale e' dominata da quel rumore e l'eco della tastiera esce interlacciata.
`tests/keyboard.sh` filtra le maiuscole prima di cercare la stringa.

Le tre regole del context switch, da non violare:

- in `task_yield` l'indice di chi esce e quello di chi entra sono due cose
  distinte: `task_switch(&tasks[prev].esp, tasks[next].esp)`. Collassarli in
  uno fa salvare il contesto uscente nella casella di quello entrante, e il
  conto si presenta due switch dopo;
- il primo argomento e' un **posto** dove salvare, il secondo un **valore** da
  caricare. L'asimmetria e' voluta;
- il frame falsificato da `task_create` serve solo per il primo ingresso: la
  prima chiamata del task lo calpesta usando lo stack normalmente;
- `eflags` fa parte del contesto e `switch.S` lo salva con `pushfl`/`popfl`.
  Senza, un task appena creato erediterebbe gli interrupt spenti dal gate del
  timer e non verrebbe mai piu' interrotto: il kernel stamperebbe la lettera
  del primo task all'infinito. Verificato togliendolo;
- in `isr_handler` l'EOI va mandato **prima** di chiamare il gestore, perche'
  il gestore del timer commuta e non tornerebbe mai all'EOI. E' sicuro perche'
  i gate sono interrupt gate. Verificato invertendo l'ordine: 45000 caratteri
  della stessa lettera, zero transizioni;
- le sezioni critiche si chiudono con `irq_restore`, non con `sti`:
  `vga_putc` viene chiamata anche da `panic_regs`, dove gli interrupt sono
  spenti deliberatamente. E restano corte — `set_cursor` sta fuori, perche'
  quattro `outb` per carattere con gli interrupt spenti farebbero perdere tick
  al timer.

La regola del ring buffer, da non violare: `head` lo scrive solo il produttore
(il gestore), `tail` solo il consumatore (`kmain`). Ognuno legge l'indice
dell'altro e non lo tocca. Per questo non serve nessun `cli`, e per questo non
c'e' un contatore degli elementi: sarebbe l'unica variabile scritta da
entrambi. Non chiamare `keyboard_getchar` da un interrupt handler: il buffer
ammette un solo consumatore.

Corollario per M7: il consumatore va **spostato**, non aggiunto. Oggi e' il
ciclo di idle di `kmain`; quando la shell prendera' il suo posto, quel ciclo
deve smettere di leggere, non leggere in parallelo.

Il debito di concorrenza del secondo blocco, da tenere presente da M9: la
tabella dei descrittori e' **per task**, ma la tabella dei file aperti e la
cache di inode sono **condivise** fra task prelazionati cento volte al secondo.
`refs++` e' read-modify-write, cioe' lo stesso `count++` che nel ring buffer si
e' evitato con la struttura — e qui la struttura non salva, perche' tutti i task
aprono file per definizione. Servono `irq_save`/`irq_restore` intorno ai
refcount e alla ricerca di uno slot libero, corti come in `vga_putc` e per la
stessa ragione: una sezione critica lunga fa perdere tick, e il self-check di M4
sulla frequenza lo nota.

Lo smoke test ora concede 15 secondi invece di 5, perche' la misura della
frequenza costa due secondi di tempo reale. L'uscita anticipata resta, quindi
in pratica termina in poco piu' di due secondi.

Nota sul provare le eccezioni a mano: non usare `int $N` su un vettore che ha
un codice d'errore (8, 10-14, 17). L'`int` software non ne fa impilare uno,
ma lo stub corrispondente e' generato con `ISR_ERR` e assume che ci sia:
lo stack risulta sfalsato di quattro byte e il dump mente. Usa un vettore
senza codice d'errore, o provoca una fault vera.

Debiti tecnici lasciati aperti da M1, da saldare quando toccano:

- lo scroll usa `memcpy` su regioni sovrapposte: funziona per la direzione
  attuale, ma è comportamento indefinito — serve `memmove` o un ciclo su celle.
  È anche l'ultimo punto di `vga.c` che scarta il `volatile` del framebuffer;
- `kprintf` formatta due volte, una per sink, riusando lo stesso `va_list`:
  legale su i386 dove `va_list` è un puntatore passato per valore, non
  altrove. Una passata sola con un sink doppio lo risolverebbe;
- `put_uint` tratta la base 10 come con segno, quindi non può stampare
  decimali senza segno sopra 2³¹;
- `ring.c` avanza gli indici con `% RING_SIZE` invece di `& RING_MASK`: una
  divisione dentro il gestore della tastiera, dove una maschera basterebbe;
- la tastiera tiene un flag singolo per lo shift, quindi rilasciarne uno lo
  spegne anche se l'altro è ancora premuto. Servirebbe una maschera a due bit.

Le milestone del primo blocco sono M1 boot+VGA, M2 GDT, M3 IDT+exception+PIC,
M4 timer PIT, M5 tastiera, M6a multitasking cooperativo, M6b preemptive.

Il secondo blocco, in ordine — la forma Unix prima, l'isolamento dopo:

```text
M7   shell            editor di riga + tabella comandi
M8   device layer     struct device, registro, i driver si iscrivono
M9   VFS + devfs      path, inode, tabella fd, open/read/write
M10  ATA PIO          driver disco in polling + strato a blocchi
M11  minix v1         superblocco, bitmap, inode — lettura, poi scrittura
M12  memoria          mmap Multiboot, allocatore di pagine, kmalloc
M13  paging           page directory, spazi di indirizzamento per processo
M14  TSS + ring 3     int 0x80, ABI Linux i386, validazione puntatori utente
M15  ELF + exec       loader, build user-space, crt0, stub delle syscall
M16  fork/wait        init come PID 1, /bin/sh in ring 3
M17  newlib           opzionale: printf e malloc veri
```

Aggiorna questa sezione quando una milestone viene chiusa.

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
kernel/task.c       kernel/switch.S     kernel/ring.c

secondo blocco:
kernel/shell.c      kernel/device.c     kernel/vfs.c      kernel/devfs.c
kernel/minixfs.c    kernel/pmm.c        kernel/paging.c   kernel/syscall.c
user/sh.c
```

Vale anche per le **aggiunte** a `kernel/task.c`: `fork`, `exec` e `wait` sono
di Walter come il resto del file.

**File che Claude scrive e mantiene** (infrastruttura: non insegna nulla e
costa solo tempo):

```text
Makefile            linker.ld           boot/multiboot.S    kernel/main.c
kernel/serial.c     kernel/selftest.c   include/types.h     include/io.h
include/panic.h     kernel/gdt.S        kernel/isr.S        tests/**
docs/**

secondo blocco:
kernel/ata.c        kernel/elf.c        kernel/tss.c
user/crt0.S         user/syscall.S      user/Makefile     user/user.ld
tools/**
```

`ata.c` e' una sequenza fissa di `outb` trascritta da un datasheet, `elf.c` e'
parsing di struct: sono i due punti del blocco dove il rapporto fra tempo speso
e concetti appresi e' piu' basso. `tss.c` e' bit-packing in un descrittore GDT,
che Walter ha gia' fatto in M2.

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

- **Nessuna allocazione dinamica fino a M12.** Array statici a capacità fissa:
  `MAX_TASKS 8`, ring buffer tastiera 128 byte, e nel secondo blocco
  `MAX_DEVICES 16`, `MAX_INODES 64`, `MAX_OPEN_FILES 32`, `TASK_FDS 8`.
  L'allocatore arriva in M12 perché il paging lo **forza** — una page directory
  per processo, allineata a 4 KB — non prima. Con gli array statici il
  fallimento è deterministico e il bilancio della RAM si vede a tempo di link.
- **Assembly in sintassi GNU as** (file `.S`), mai nasm.
- `assert()` è sempre attivo e chiama `panic()`. Non introdurre `NDEBUG`.
- Nessun `float`/`double`: l'FPU non è inizializzata.
- Ogni sottosistema ha una `*_init()` esplicita, chiamata da `kmain` in ordine
  visibile. Nessuna inizializzazione implicita o lazy.
- `kprintf` scrive su VGA **e** su COM1. La seriale è ciò che leggono i test:
  non aggiungere output diagnostico solo su VGA.

**Vincolo POSIX, dal secondo blocco.** Non si porta glibc e non si eseguono i
coreutils GNU — misurato: un `puts` statico contro glibc fa dieci syscall di
cerimonia per una di lavoro, e fra quelle `set_thread_area` (che implica una LDT
per thread, perché `errno` sta in TLS), `getrandom` e l'infrastruttura futex, più
il vettore ausiliario con `AT_RANDOM` prima di `main`. Quello è un progetto di
compatibilità ABI Linux, non di scrittura di un kernel.

Ma la porta si tiene aperta, e costa zero:

- codici d'errore come **ritorno negativo** con i valori Linux (`-ENOENT` = -2,
  `-EBADF` = -9, `-EINVAL` = -22, `-ENOSYS` = -38), mai un `-1` generico;
- i **numeri di syscall veri di Linux i386**: 1 exit, 2 fork, 3 read, 4 write,
  5 open, 6 close, 7 waitpid, 11 execve, 12 chdir, 19 lseek, 20 getpid,
  39 mkdir, 41 dup, 42 pipe, 45 brk, 54 ioctl, 106 stat, 108 fstat,
  141 getdents. Così gli stub di una libc compilata per Linux funzionano non
  modificati;
- `int $0x80` con gli argomenti in `ebx ecx edx esi edi ebp`;
- `struct stat` col layout Linux i386;
- firme POSIX nel VFS: `open(path, flags, mode)`, non `vfs_apri()`.

La vetta raggiungibile è **newlib** (M17): vuole ~15 stub, che sono esattamente
la forma del VFS di M9.

**Un puntatore che arriva da ring 3 non è un puntatore.** Da M14 ogni argomento
di syscall che è un indirizzo va validato prima dell'uso — dentro lo spazio
utente, mappato, con la lunghezza che non scavalca il confine. Saltare questo
controllo significa che un processo utente può far scrivere al kernel dove
vuole.

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

Dal secondo blocco si aggiungono, nello stesso ordine di costo:

- `CR0.PG` acceso con una mappa identità che non copre il kernel **e le tabelle
  stesse**: il prossimo fetch fa fault e la tripla fault non dice niente. Le
  tabelle si camminano e si verificano *prima* di scrivere `CR0`.
- Il TSS: serve per una cosa sola, da dove la CPU prende `esp0` quando un
  interrupt arriva mentre è in ring 3. Sbagliato, e *ogni* interrupt in user mode
  è un double fault.
- `refs++` sui refcount condivisi del VFS senza sezione critica.
- Argomenti di syscall che sono indirizzi, usati senza validazione.
- `argc`/`argv`/`envp` disposti male sullo stack utente in `execve`: il
  programma parte e legge spazzatura, cioè fallisce lontano dalla causa.

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

Nel secondo blocco Linux 0.01 diventa **più** pertinente, non meno: `fs/`
implementa esattamente il minix v1 di M11, `fs/exec.c` è l'`execve` di M15,
`kernel/fork.c` è M16.

Si aggiungono **xv6** del MIT — il riferimento migliore per il VFS a tre livelli
di M9 e per il confine delle syscall di M14, perché fa le stesse scelte spiegate
meglio — il **Tanenbaum** per il capitolo sui filesystem, che descrive minix v1
perché minix è suo, e il **manuale Intel volume 3A** capitoli 4 (paging) e 7
(task management, per il TSS), che in M13 e M14 serve spesso perché OSDev è
ambiguo.
