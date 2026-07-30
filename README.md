# waltex

Un kernel monolitico per x86 a 32 bit, scritto da zero e avviato in QEMU.
Tremilatrecento righe fra C e assembly, dal boot a un prompt che risponde.

Non è software da riutilizzare: è un progetto per **capire come funziona un
kernel**, costruito un pezzo alla volta e con ogni pezzo verificato prima di
passare al successivo.

## Cosa fa, adesso

Si avvia via Multiboot, installa la propria GDT e la propria IDT, gestisce le
eccezioni della CPU con un dump leggibile, conta il tempo con il PIT a 100 Hz,
legge la tastiera, fa girare tre task su tre stack separati commutando cento
volte al secondo — e presenta una shell.

```text
waltex> help
  help    elenca i comandi
  echo    stampa i suoi argomenti
  ticks   tick del timer dal boot
  ps      stato della tabella dei task
  peek    peek <indirizzo> [n] - dump, entrambi in esadecimale
  spin    avvia i due task di prova rumorosi
  clear   pulisce lo schermo
  panic   provoca un panic deliberato
waltex> peek b8000 10
b8000:  77 07 61 07 6c 07 74 07 65 07 78 07 3a 07 20 07
```

Quel dump è il framebuffer VGA riletto da dentro, all'indirizzo `0xB8000`: byte
pari i caratteri, byte dispari l'attributo. `77 07` è una `w` grigia su nero, e
i sette caratteri che seguono compitano `waltex:` più uno spazio — l'angolo in
alto a sinistra dello schermo, dove lo scroll ha lasciato una delle righe di
boot.

`peek` non è un giocattolo: è lo strumento con cui si ispezioneranno le
tabelle delle pagine mentre si scrive il paging, quando un errore non produce
un messaggio ma una tripla fault.

Una divisione per zero produce questo, invece di una VM che riparte in
silenzio:

```text
*** PANIC: Divide Error (vettore 0)
    eip=101750  cs=8  eflags=46  err=0
    eax=1  ebx=9500  ecx=0  edx=0
    esi=0  edi=1000  ebp=10a910  esp=10a8e4
    ds=10
    addr2line -e build/waltex.elf 101750
```

## Provarlo

Servono `qemu-system-x86` e `gcc-multilib`. Non serve un cross-compiler: si usa
il gcc di sistema in modalità freestanding.

```sh
sudo apt install qemu-system-x86 gcc-multilib

make          # build/waltex.elf
make run      # QEMU con finestra, output VGA visibile
make test     # headless: host, smoke, tastiera, shell, task. Exit code reale
make debug    # qemu -s -S, in attesa di gdb sulla :1234
```

`make run` apre un prompt. `help` elenca i comandi; `spin` avvia i due task di
prova, che da quel momento stampano `A` e `B` alternandosi perché il timer toglie
loro il controllo cento volte al secondo.

## Le sette milestone

Ognuna termina con un kernel che si avvia, un test verde e un commit. Lo scopo
della struttura incrementale è che quando qualcosa si rompe la superficie di
sospetto sia di poche decine di righe: in kernel dev non ci sono stack trace,
c'è una tripla fault che riavvia la macchina.

| | Cosa aggiunge | Il pezzo interessante |
|---|---|---|
| **M1** | boot Multiboot, VGA text mode, seriale, `kprintf` | due canali di output, e la seriale è ciò che leggono i test |
| **M2** | GDT propria a tre descrittori piatti | se funziona non si vede niente: la verifica rilegge la tabella con `sgdt` |
| **M3** | IDT, 48 stub, rimappaggio del PIC, `panic` | la milestone in cui il kernel smette di morire in silenzio |
| **M4** | PIT a 100 Hz, la prima `sti` | due flussi di esecuzione che condividono un contatore |
| **M5** | tastiera, ring buffer | la prima concorrenza vera, risolta con la struttura invece che con un lock |
| **M6a** | multitasking cooperativo | un task è uno stack più un `esp` salvato |
| **M6b** | multitasking preemptive | il controllo viene **tolto**, non ceduto |
| **M7** | shell: editor di riga, tabella dei comandi | la prima milestone interamente di software, e la prima che si *usa* |

I documenti di progetto stanno in [docs/superpowers/](docs/superpowers/): due
spec con le motivazioni delle scelte e le alternative scartate, e un piano per
milestone.

## Com'è organizzato

```text
boot/multiboot.S     header Multiboot, stack, ingresso in kmain a 1 MiB
kernel/
  main.c             la sequenza di boot, leggibile in venti righe
  vga.c              text mode 80x25: scroll, cursore hardware, colore corrente
  serial.c           COM1 in polling — il canale dei test
  kprintf.c          formatter %d %x %s %c, con il cuore separato dal sink
  memory.c           memcpy, memset, memset16
  gdt.c   gdt.S      segmentazione: descrittori, lgdt, far jump
  idt.c   isr.S      256 gate, 48 stub generati da macro, il dispatcher
  exceptions.c       i nomi Intel delle 32 eccezioni, per il dump
  pic.c              rimappaggio del 8259 da 8-15 a 32-47
  panic.c            dump dei registri, in rosso, e halt
  timer.c            PIT canale 0, gestore dell'IRQ 0
  keyboard.c         scancode set 1, gestore dell'IRQ 1
  ring.c             buffer circolare a produttore e consumatore singoli
  task.c  switch.S   tabella dei task, scheduler, cambio di contesto
  lineedit.c         da tasti a righe: accumula, corregge, dice quando è finita
  shell.c            otto comandi, il dispatcher, il ciclo del prompt
  demo.c             i due task rumorosi, accesi dal comando spin
  rtc.c              orologio CMOS: serve solo ai test
  selftest.c         i controlli che girano dentro la VM
include/             i contratti d'interfaccia, tipi scritti a mano, inline asm
tests/               due livelli di verifica, vedi sotto
```

## Come si verifica un kernel

È la parte del progetto su cui vale la pena soffermarsi, perché un kernel non
si può eseguire in un test runner.

**Livello 1: la logica pura, sull'host.** Il formatter di `kprintf`, la tabella
scancode, il buffer circolare, il calcolo del divisore del PIT, la
falsificazione dello stack di un task nuovo: tutte cose che non toccano
hardware. Si compilano con il gcc dell'host e si provano in millisecondi.

```text
test_memory      59 controlli        test_lineedit    29 controlli
test_keyboard    23 controlli        test_shell       27 controlli
test_kprintf     22 controlli        test_ring        12 controlli
test_task        15 controlli        test_timer        9 controlli
```

Centonovantasei in tutto, e la quota testabile sull'host **sale** con le
milestone invece di scendere: `lineedit` e le due funzioni pure di `shell` non
toccano hardware, quindi sono interamente verificabili in millisecondi.

Quello su `task` merita una nota: lo stack falsificato da `task_create` è solo
memoria, quindi si verifica **senza mai saltarci dentro** — cioè mentre un
errore è ancora leggibile invece di essere una tripla fault muta.

**Livello 2: dentro la VM.** Tutto il resto esiste solo davanti all'hardware.
Quarantatre self-check girano nel kernel e riportano l'esito sulla seriale,
verificando le cose **rileggendole**: il framebuffer dopo averci scritto, i
registri del cursore, la GDT con `sgdt`, l'IDT con `sidt`, le maschere del PIC.

Su hardware muto la rilettura è l'unica conferma che esista.

E quattro test guardano il kernel da fuori:

- `tests/smoke.sh` cerca i marker sulla seriale con un timeout
- `tests/keyboard.sh` digita `walter` nel monitor di QEMU e cerca l'eco
- `tests/shell.sh` digita `echo ciao` e verifica che la shell l'abbia eseguito,
  più che il prompt ricompaia — cioè che il ciclo continui invece di fermarsi al
  primo comando
- `tests/tasks.sh` manda `spin` dal prompt e poi verifica che i task si alternino
  **e** che il cambio sia involontario — corse di lunghezza 1 vorrebbero dire che
  stanno cedendo volontariamente, cioè che la prelazione non c'è

La frequenza del timer si misura contro l'**orologio CMOS**, che è un
riferimento indipendente: un timer non può misurare se stesso, e uno
programmato al doppio della frequenza voluta passerebbe qualunque verifica
basata sui propri tick.

## Vincoli

Codice freestanding: **non esiste la libc**. Niente `stdio.h`, `string.h`,
`stdlib.h`. I tipi vengono da `include/types.h`, scritto a mano. Se serve
`memset`, si scrive.

**Nessuna allocazione dinamica**, in nessuna milestone. Non è pigrizia: senza
leggere la mappa di memoria Multiboot il kernel non sa quanta RAM esista, e con
array statici il fallimento è deterministico (`task_create` restituisce `-1` in
un punto prevedibile) e il bilancio della memoria si vede a tempo di link.

```text
   text    data     bss
  19322       1   51952      ~70 KB in tutto
```

I 52 KB di `.bss` sono in gran parte la tabella dei task: otto task da 4 KB di
stack sono 32 KB, più del codice del kernel. Con array statici ogni costante è
una decisione sul consumo di RAM, ed è visibile — la riga di comando da 128 byte
di M7 si vede in quel numero, e così sarà per le tabelle del VFS.

Altri vincoli: assembly in sintassi GNU as, `assert()` sempre attivo che chiama
`panic()`, nessun `float` perché l'FPU non è inizializzata, e ogni sottosistema
con una `*_init()` esplicita chiamata da `kmain` in ordine visibile.

## Debug

Non si debugga a tentativi, gli strumenti ci sono:

```sh
make debug
gdb -q build/waltex.elf -ex 'target remote :1234' -ex 'break kmain'
```

E `-d int,cpu_reset` nei flag di QEMU logga ogni interrupt e ogni reset con lo
stato della CPU: è così che si identifica una tripla fault, e si legge dove.

Se il sintomo è "la VM riparte in silenzio", il sospetto è quasi sempre nella
milestone appena scritta, ed è un problema di tabelle o di stack, non di logica.

## Dove va, da qui

Il secondo blocco è progettato:
[lo spec](docs/superpowers/specs/2026-07-29-waltex-userland-design.md) porta a
`/bin/sh` come processo utente in ring 3, caricato da un filesystem minix su
disco. Dieci milestone, la forma Unix prima e l'isolamento dopo:

```text
M8   device layer     struct device, registro, i driver si iscrivono
M9   VFS + devfs      path, inode, tabella fd  ← «tutto è un file»
M10  ATA PIO          driver disco in polling
M11  minix v1         superblocco, bitmap, inode
M12  memoria          mmap Multiboot, allocatore di pagine, kmalloc
M13  paging           page directory, spazi di indirizzamento per processo
M14  TSS + ring 3     int 0x80, ABI Linux i386
M15  ELF + exec       loader, build user-space, crt0
M16  fork/wait        init come PID 1, /bin/sh in ring 3
```

Il confine delle syscall è POSIX per scelta — numeri veri di Linux i386, `errno`
negativo — perché costa zero e tiene aperta la porta a **newlib**, che vuole una
quindicina di stub, cioè esattamente la forma del VFS di M9.

Resta fuori: glibc e i coreutils GNU. Misurato invece che stimato — un
`puts("ciao")` statico contro glibc su i386 fa undici syscall distinte, dieci di
cerimonia per una di lavoro, e fra quelle `set_thread_area`, `getrandom` e
l'infrastruttura futex. Quello è un progetto di compatibilità ABI Linux, non di
scrittura di un kernel.

E resta fuori del tutto: segnali, pipe, copy-on-write, SMP, rete, virgola mobile.

## Lettura di accompagnamento

**Linux 0.01** come materiale storico per milestone — `head.s`, `kernel/traps.c`,
`kernel/sched.c`, `kernel/keyboard.s`, `include/linux/sched.h`. Interessante
soprattutto dove divergiamo: la macro `switch_to` di Linus usa il task
switching hardware dell'x86, con un TSS per processo, la strada prevista da
Intel e abbandonata da tutti.

**[OSDev wiki](https://wiki.osdev.org/)** mentre si scrive, il **manuale Intel
volume 3A** quando OSDev è ambiguo, e **xv6** del MIT per vedere come lo fa
qualcuno che sa.
