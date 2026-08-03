# waltex

Un kernel monolitico per x86 a 32 bit, scritto da zero e avviato in QEMU.
Tremilaottocento righe fra C e assembly, dal boot a un prompt che risponde.

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
  devs    elenca i device registrati
  ls      naviga il filesystem
  cat     mostra il contenuto di un file
  lsblk   elenca i dischi con la loro capacita'
  rdsect  rdsect [disco] <settore> [n] - dump, in decimale
  wrsect  wrsect [disco] <settore> <hex> - riempie il settore ripetendo il pattern
  mkdir   mkdir <path> - crea una directory
  write   write <path> <testo...> - crea o SOVRASCRIVE un file
waltex> devs
  console         5:1 -w
  ttyS0           4:64 -w
  kbd             13:64 r-
waltex> peek b8000 10
b8000:  77 07 61 07 6c 07 74 07 65 07 78 07 3a 07 20 07
```

I tre dispositivi sono lo schermo, la porta seriale e la tastiera, dietro una
sola interfaccia: `console` e `ttyS0` scrivono e non leggono, `kbd` il
contrario. Le due lettere non vengono da un campo, si leggono dalla **nullità
dei puntatori** alle operazioni — un puntatore a zero dice «questo dispositivo
non fa quella cosa».

I numeri sono quelli veri di Linux: `/dev/console` è 5:1 e `/dev/ttyS0` è 4:64.
Non servivano a niente in M8 e costavano zero; da M9b l'inode di un file di
dispositivo memorizza quella coppia, e `ls` la mostra.

Quel dump è il framebuffer VGA riletto da dentro, all'indirizzo `0xB8000`: byte
pari i caratteri, byte dispari l'attributo. `77 07` è una `w` grigia su nero, e
i sette caratteri che seguono compitano `waltex:` più uno spazio — l'angolo in
alto a sinistra dello schermo, dove lo scroll ha lasciato una delle righe di
boot.

`peek` non è un giocattolo: è lo strumento con cui si ispezioneranno le
tabelle delle pagine mentre si scrive il paging, quando un errore non produce
un messaggio ma una tripla fault.

E gli stessi tre dispositivi, visti come file:

```text
waltex> ls /dev
  3 console
  4 ttyS0
  5 kbd
waltex> ls /dev/kbd
  5 kbd  chardev 13:64
waltex> cat /dev/kbd
ciao
```

Quel `ciao` non è l'eco dell'editor di riga: mentre `cat` gira, la shell non
sta leggendo la tastiera. I caratteri attraversano IRQ 1, ring buffer,
`keyboard_getchar`, `kbd_dev_read`, `chardev_read` e `vfs_read` prima di essere
stampati — sette livelli, e il comando che li stampa non sa che dall'altra parte
ci sia una tastiera.

`/dev` non è memorizzata da nessuna parte: le sue voci **sono** il registro dei
dispositivi, generate quando qualcuno le chiede. Un `struct inode` non ha
puntatori ai figli — ogni directory sa solo rispondere «dammi il figlio che si
chiama così», e camminare un path è una catena di domande. È ciò che permette a
minix, poco più sotto, di leggere le risposte dal disco senza tenerlo tutto in
RAM — e allo stesso `cat` di funzionare su entrambi senza una riga di modifica.

E da M10 c'è un disco vero sotto:

```text
waltex> lsblk
  hda  2048 settori  (1024 KB)
waltex> rdsect 1 32
0:  03 0a 11 18 1f 26 2d 34 3b 42 49 50 57 5e 65 6c
10:  73 7a 81 88 8f 96 9d a4 ab b2 b9 c0 c7 ce d5 dc
waltex> wrsect 7 c0ffee
  scritti 512 byte nel settore 7
waltex> rdsect 7 32
0:  c0 ff ee c0 ff ee c0 ff ee c0 ff ee c0 ff ee c0
10:  ff ee c0 ff ee c0 ff ee c0 ff ee c0 ff ee c0 ff
```

Quel `03 0a 11 18` **non l'ha scritto il kernel**: è il pattern che
`tools/mkdisk.sh` mette nel settore 1 prima che la VM parta, e rileggerlo
identico è la sola prova che il driver legga davvero il settore che gli si
chiede. Un disco non può verificare se stesso — è la stessa disciplina con cui
in M4 la frequenza del timer si misura contro l'orologio CMOS.

Il controllo va anche nell'altro verso: `tests/disk.sh` fa scrivere il kernel,
chiude QEMU e poi rilegge l'immagine **da fuori la VM** con `od`. È il solo test
del progetto in cui la verifica avviene fuori dalla macchina che ha fatto il
lavoro, ed è servito subito: ha trovato una `write` che scriveva un settore in
più: dentro la VM tornava tutto, e nel settore successivo finiva lo stack.

E sul secondo disco c'è un filesystem vero:

```text
waltex> lsblk
  hda  2048 settori  (1024 KB)
  hdb  512 settori  (256 KB)
waltex> ls /
  1 .
  1 ..
  2 hello.txt
  3 etc
  5 grande.txt
  6 enorme.txt
  7 vuoto.txt
  2 dev
waltex> cat /etc/motd
waltex M11: minix v1, sola lettura
waltex> ls /dev
  3 console
  4 ttyS0
  5 kbd
```

**La radice viene dal disco.** È un'immagine minix v1 costruita da `mkfs.minix`
e riempita da `mount`, cioè da due implementazioni che non sono la nostra: i
nomi in `ls /` non li ha scritti waltex.

`dev` è l'unica voce che non sta sull'immagine — è **innestata**: la `lookup`
della radice minix controlla uno slot in RAM prima di guardare il disco, ed è
tutto ciò che c'è di un mount. Nella stessa schermata convivono due filesystem
diversi, e `ls` non sa quale sta interrogando.

E `cat /etc/motd` è **lo stesso `cat`** che poco sopra legge la tastiera, senza
una riga di differenza: là attraversa tastiera → ring buffer → devfs → VFS, qui
ATA → blockdev → minix → VFS. È la prova che l'astrazione era nel punto giusto,
e non si poteva avere prima di adesso.

E da M11b il filesystem si **scrive**:

```text
waltex> mkdir /doc
  creata /doc
waltex> write /doc/note.txt ciao mondo
  scritti 11 byte
waltex> cat /doc/note.txt
ciao mondo
```

Quel `mkdir` alloca un inode accendendo un bit su una bitmap, gli scrive dentro
`.` e `..`, incrementa il conteggio dei link del genitore e inserisce una voce
da 16 byte nella directory radice — che a sua volta è un file, e cresce.

**E il giudice è fuori dalla VM.** `tests/minixwrite.sh` fa creare al kernel,
chiude QEMU e poi passa l'immagine a `fsck.minix`:

```text
$ fsck.minix -f build/minix.img
$ echo $?
0
```

`mount` da solo non basterebbe, ed è misurato: spegnendo un bit nella bitmap di
un'immagine sana, `mount` la accetta e `ls` funziona benissimo — mentre `fsck`
dice `Inode 2 marked unused, but used for file '/hello.txt'`. Un filesystem
incoerente si legge; il danno esce alla prossima allocazione, quando quell'inode
viene riusato e due file finiscono sopra lo stesso.

Ha ripagato subito: ha trovato un inode allocato e mai collegato, lasciato lì da
un `create` che validava il nome **dopo** aver toccato il disco. Tutti gli
ottantanove controlli host passavano.

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

## Le milestone

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
| **M8** | device layer: un registro, i driver si iscrivono | la tastiera smette di essere un caso speciale — e il prerequisito di «tutto è un file» |
| **M9a** | VFS: path, inode, tabella dei descrittori | la prima milestone **interamente fuori da QEMU**: 75 controlli host, zero self-check |
| **M9b** | devfs, `ls` e `cat` | «tutto è un file» diventa vero: `/dev` non è memorizzata, è il registro dei dispositivi interrogato |
| **M10** | driver ATA PIO, `struct blockdev` | la prima memoria che sopravvive allo spegnimento — e il primo test che verifica il lavoro **da fuori** la VM |
| **M11a** | minix v1 in lettura, la radice su disco | il riferimento è `mkfs.minix`: il parser cammina un'immagine che ha costruito qualcun altro |
| **M11b** | bitmap, allocazione, `mkdir` e `write` | il riferimento cambia verso: scrive il kernel, e **`fsck.minix`** dice se il risultato regge |

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
  shell.c            quattordici comandi, il dispatcher, il ciclo del prompt
  ata.c              disco ATA in polling: identify, settori, flush
  minixfs.c          minix v1: superblocco, inode, zone, directory
  demo.c             i due task rumorosi, accesi dal comando spin
  device.c           il registro: i driver si iscrivono, il resto li cerca
  vfs.c              path, inode, descrittori — non sa quali file esistano
  devfs.c            glielo dice: /dev generata dal registro dei dispositivi
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
test_minixfs     89 controlli        test_vfs         75 controlli
test_memory      72 controlli        test_shell       42 controlli
test_device      31 controlli        test_lineedit    29 controlli
test_keyboard    23 controlli        test_kprintf     22 controlli
test_task        15 controlli        test_ring        12 controlli
test_timer        9 controlli
```

Quattrocentodiciannove in tutto, ed erano novantanove alla fine del primo blocco: la
quota testabile sull'host **sale** con le milestone invece di scendere.

Il VFS è testabile perché `vfs_init` **riceve** la radice invece di
costruirsela: nel kernel gliela passa il filesystem, nei test un albero finto di
sei nodi scritto dentro il file di test. Senza quella scelta di interfaccia,
provare la risoluzione di un path richiederebbe un disco.

**`test_minixfs` è la verifica più forte del progetto**, e vale la pena
spiegarne il meccanismo. `struct blockdev` ha due puntatori a funzione — `read`
e `write` su settori — e sull'host diventano `fread` e `fseek` su un file:

```c
static int file_read(struct blockdev *b, uint32_t lba, void *buf, uint32_t n)
{
    FILE *f = (FILE *)b->priv;
    ...
}
```

Quattro righe, e `minixfs.c` non si accorge che sotto c'è un file invece di un
disco. Il file in questione è `tests/data/minix.img`, costruita da `mkfs.minix`
e riempita da `mount`: quando il parser e loro non sono d'accordo, la differenza
si localizza con `od`.

Il controllo che conta è la lettura di un file da 20000 byte. Sette zone stanno
nell'inode, le altre tredici in un **blocco indiretto**, e senza un file oltre i
7168 byte il bug classico di minix v1 — puntatori di zona letti come `uint32`,
che è il formato della v2 — non lo vedrebbe nessuno: i file piccoli
continuerebbero a funzionare.

Quello su `task` merita una nota: lo stack falsificato da `task_create` è solo
memoria, quindi si verifica **senza mai saltarci dentro** — cioè mentre un
errore è ancora leggibile invece di essere una tripla fault muta.

**Livello 2: dentro la VM.** Tutto il resto esiste solo davanti all'hardware.
Centootto self-check girano nel kernel e riportano l'esito sulla seriale,
verificando le cose **rileggendole**: il framebuffer dopo averci scritto, i
registri del cursore, la GDT con `sgdt`, l'IDT con `sidt`, le maschere del PIC.

Su hardware muto la rilettura è l'unica conferma che esista.

E sei test guardano il kernel da fuori:

- `tests/smoke.sh` cerca i marker sulla seriale con un timeout
- `tests/keyboard.sh` digita `walter` nel monitor di QEMU e cerca l'eco
- `tests/shell.sh` digita `echo ciao` e verifica che la shell l'abbia eseguito,
  che il prompt ricompaia — cioè che il ciclo continui invece di fermarsi al
  primo comando — e che `devs` elenchi i tre dispositivi con i loro numeri e le
  loro capacità. Quest'ultimo è un test di M8 travestito da test della shell: che
  i driver si siano iscritti davvero non è verificabile da nessun test host,
  perché li iscrivono le `*_init` dentro la VM. Da M9b digita anche `ls /`,
  `ls /dev` e `cat /dev/kbd`, e ogni controllo guarda **solo** le righe fra il
  proprio comando e il prompt successivo: cercare in tutto il log troverebbe
  l'eco del comando digitato — `waltex> ls /dev` contiene `dev` — e passerebbe
  con i comandi inesistenti
- `tests/tasks.sh` manda `spin` dal prompt e poi verifica che i task si alternino
  **e** che il cambio sia involontario — corse di lunghezza 1 vorrebbero dire che
  stanno cedendo volontariamente, cioè che la prelazione non c'è
- `tests/disk.sh` ricostruisce l'immagine, verifica che il settore di prova parta
  **a zeri** — un test che scrive nel proprio input non è ripetibile — fa partire
  la VM, la chiude dal monitor, e poi rilegge il file con `od`
- `tests/minixwrite.sh` fa creare una directory e un file **dal prompt**, e poi
  chiede a `fsck.minix` se il filesystem è coerente. È l'unico test del progetto
  con un oracolo che non abbiamo scritto noi

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
  46739       1   69744     ~113 KB in tutto
```

I 69 KB di `.bss` sono la tabella dei task, la cache degli inode e i nove buffer di blocco di minixfs: otto task da 4 KB di
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
disco. La forma Unix prima e l'isolamento dopo — le prime tre sono chiuse:

```text
M7   shell            editor di riga, tabella dei comandi       fatta
M8   device layer     registro, i driver si iscrivono           fatta
M9   VFS + devfs      path, inode, tabella fd                   fatta
M10  ATA PIO          driver disco in polling                   fatta
M11  minix v1         superblocco, bitmap, inode                fatta
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

I **debiti tecnici** aperti sono elencati in [CLAUDE.md](CLAUDE.md#debiti-tecnici),
ordinati per *quando mordono* invece che per anzianità — perché un debito annotato
con la propria cura si salda quando qualcos'altro rende quella cura conveniente,
non quando ci si ricorda di lui. Accanto ci sono le **assenze dichiarate**, che
sono una cosa diversa: non codice fatto male, ma codice non scritto, con la
ragione scritta una volta invece che ogni volta.

## Lettura di accompagnamento

**Linux 0.01** come materiale storico per milestone — `head.s`, `kernel/traps.c`,
`kernel/sched.c`, `kernel/keyboard.s`, `include/linux/sched.h`. Interessante
soprattutto dove divergiamo: la macro `switch_to` di Linus usa il task
switching hardware dell'x86, con un TSS per processo, la strada prevista da
Intel e abbandonata da tutti.

**[OSDev wiki](https://wiki.osdev.org/)** mentre si scrive, il **manuale Intel
volume 3A** quando OSDev è ambiguo, e **xv6** del MIT per vedere come lo fa
qualcuno che sa.
