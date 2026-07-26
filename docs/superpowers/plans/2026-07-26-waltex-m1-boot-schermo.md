# M1 — Boot e schermo: piano di implementazione

> **Nota sulla modalità di lavoro.** Questo piano non è interamente
> eseguibile da un agente. I task marcati **[WALTER]** sono deliberatamente
> privi di implementazione: contengono interfaccia, test, concetti e dati
> hardware, ma il codice lo scrive Walter (vedi `CLAUDE.md`). I task marcati
> **[CLAUDE]** sono infrastruttura e vanno implementati come scritto.

**Obiettivo:** un kernel i386 che si avvia in QEMU tramite header Multiboot,
scrive testo sia in VGA text mode sia su COM1, e ha un `make test` verde.

**Architettura:** ELF a 32 bit linkato a 1 MiB con header Multiboot nei primi
byte; QEMU lo carica direttamente con `-kernel`. `_start` in assembly monta uno
stack e chiama `kmain`, che inizializza i due sink di output e stampa i marker
che i test cercano. La verifica è su due livelli: test della logica pura
compilati col gcc dell'host, e self-check eseguiti dentro la VM che riportano
l'esito su seriale.

**Stack:** gcc 13 con `-m32 -ffreestanding`, GNU as, GNU ld, GNU make,
qemu-system-i386, bash.

## Vincoli globali

Copiati dallo spec, valgono per ogni task senza essere ripetuti:

- Nessuna libc: vietati `stdio.h`, `string.h`, `stdlib.h`, `assert.h`. Unica
  eccezione ammessa è `<stdarg.h>`, che è un header del **compilatore** e resta
  disponibile in freestanding.
- Nessuna allocazione dinamica. Array statici a capacità fissa.
- Assembly in sintassi GNU as, file `.S`, mai nasm.
- Nessun `float` né `double`: l'FPU non è inizializzata.
- Ogni sottosistema espone una `*_init()` esplicita, chiamata da `kmain` in
  ordine visibile.
- `CFLAGS` esatti: `-m32 -ffreestanding -nostdlib -fno-pie
  -fno-stack-protector -fno-builtin -Wall -Wextra -std=gnu11 -g`
- `LDFLAGS` esatti: `-m elf_i386 -T linker.ld -nostdlib`
- Indirizzo di caricamento: `0x100000` (1 MiB).
- Nessun codice appartenente a milestone successive alla M1.

## Struttura dei file al termine di M1

| File | Responsabilità | Chi |
|---|---|---|
| `Makefile` | build, run, test, debug, clean | CLAUDE |
| `linker.ld` | layout a 1 MiB, ordine delle sezioni | CLAUDE |
| `boot/multiboot.S` | header Multiboot, stack, `_start` | CLAUDE |
| `include/types.h` | tipi interi, con variante hosted per i test | CLAUDE |
| `include/io.h` | `inb`/`outb`/`io_wait` come inline asm | CLAUDE |
| `include/vga.h` | contratto d'interfaccia VGA | CLAUDE |
| `include/serial.h` | contratto d'interfaccia seriale | CLAUDE |
| `include/kprintf.h` | contratto d'interfaccia del formatter | CLAUDE |
| `include/selftest.h` | contratto dei self-check in-VM | CLAUDE |
| `kernel/serial.c` | driver COM1 in polling | CLAUDE |
| `kernel/selftest.c` | self-check eseguiti nella VM | CLAUDE |
| `kernel/main.c` | `kmain`, sequenza di boot | CLAUDE |
| `kernel/vga.c` | text mode 80x25, scroll, cursore | **WALTER** |
| `kernel/kprintf.c` | formatter `%d %x %s %c %%` | **WALTER** |
| `tests/smoke.sh` | avvio QEMU headless e match dei marker | CLAUDE |
| `tests/host/Makefile` | build dei test host | CLAUDE |
| `tests/host/test_kprintf.c` | test del formatter senza QEMU | CLAUDE |

**Interfacce prodotte da M1**, usate da tutte le milestone successive:

```c
void vga_init(void);
void vga_clear(void);
void vga_putc(char c);

void serial_init(void);
void serial_putc(char c);

void kvprintf(void (*putc)(char), const char *fmt, va_list ap);
void kprintf(const char *fmt, ...);
```

## Deviazione dallo spec, dichiarata

Lo spec prevede `-device isa-debug-exit` (porta `0xF4`) per dare a `make test`
un exit code prodotto dal kernel. In M1 **non** lo usiamo: il kernel non ha
ancora modo di distinguere un avvio normale da un avvio di test, e costruire un
secondo ELF con `-DWALTEX_SMOKE` aggiungerebbe complessità al build per un
guadagno nullo a questo stadio. `tests/smoke.sh` deriva l'esito dal match dei
marker su seriale, con un timeout. L'uscita autonoma del kernel arriva in M3,
dove il `panic` handler ha un motivo reale per terminare con uno status
diverso.

---

## Task 1 [CLAUDE]: build system, boot, output su seriale

Obiettivo del task: un kernel che si avvia in QEMU e stampa una riga su COM1.
Niente VGA, niente `kprintf`: prima si dimostra che il boot funziona, poi si
costruisce sopra. Se questo task è verde, il resto di M1 è lavoro applicativo.

**Files:**
- Create: `Makefile`, `linker.ld`, `boot/multiboot.S`, `include/types.h`,
  `include/io.h`, `include/serial.h`, `kernel/serial.c`, `kernel/main.c`,
  `tests/smoke.sh`
- Test: `tests/smoke.sh`

**Interfaces:**
- Consumes: nulla.
- Produces: `serial_init(void)`, `serial_putc(char)`, il target `make`, e
  l'entry point `kmain(uint32_t magic, void *mbinfo)`.

- [ ] **Step 1: Installare i prerequisiti**

Serve la password di Walter: questo step lo esegue lui.

```bash
sudo apt install -y qemu-system-x86 gcc-multilib
```

`gcc-multilib` porta `libc6-dev-i386`, che serve ai test host: il gcc di sistema
compila già in `-m32`, ma senza gli header e le librerie a 32 bit non riesce a
**linkare** un binario che usa la libc. Verificato: senza questo pacchetto
`gcc -m32 -o t t.c` con un `#include <stdio.h>` falla su
`bits/libc-header-start.h`. Il kernel non ne ha bisogno (è `-nostdlib`), i test
host sì — e li vogliamo nello stesso modello dati del kernel, non a 64 bit.

Verifica:

```bash
qemu-system-i386 --version
printf '#include <stdio.h>\nint main(void){puts("ok");return 0;}\n' > /tmp/h.c
gcc -m32 -o /tmp/h /tmp/h.c && /tmp/h
```

Atteso: la versione di QEMU, poi `ok`.

- [ ] **Step 2: Scrivere il test di fumo, che deve fallire**

`tests/smoke.sh`, reso eseguibile con `chmod +x`:

```bash
#!/usr/bin/env bash
# Avvia il kernel in QEMU headless, cattura COM1 su file e cerca i marker
# attesi. Appena l'ultimo marker compare, QEMU viene terminato: il test non
# aspetta il timeout se il kernel ha già detto tutto quello che doveva.
set -uo pipefail

KERNEL=${1:-build/waltex.elf}
LAST_MARKER="waltex: M1 ok"
MARKERS=("waltex: booting" "waltex: multiboot ok" "$LAST_MARKER")

LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT

qemu-system-i386 -kernel "$KERNEL" -display none -no-reboot \
    -serial "file:$LOG" >/dev/null 2>&1 &
QPID=$!

# fino a 5 secondi, controllando ogni 100 ms
for _ in $(seq 1 50); do
    grep -qF "$LAST_MARKER" "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

fail=0
for marker in "${MARKERS[@]}"; do
    if grep -qF "$marker" "$LOG"; then
        echo "ok   -- $marker"
    else
        echo "FAIL -- marker mancante: $marker"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "--- output seriale completo ---"
    cat "$LOG"
    echo "--- fine output ---"
fi

exit "$fail"
```

Nota: in questo task solo i primi due marker verranno prodotti. Il terzo
(`M1 ok`) arriva nel Task 4, quindi lo smoke test resta rosso fino a lì. È
voluto: il test descrive M1 completa dall'inizio.

- [ ] **Step 3: Eseguirlo per vedere che fallisce**

Run: `./tests/smoke.sh`
Atteso: FAIL su tutti i marker (il kernel non esiste ancora).

- [ ] **Step 4: `include/types.h`**

```c
#ifndef WALTEX_TYPES_H
#define WALTEX_TYPES_H

/* I test host compilano gli stessi sorgenti con la libc disponibile: in quel
   caso i tipi devono venire da lì, altrimenti i typedef collidono con quelli
   di glibc. Nel kernel invece non esiste nessuna libc e li definiamo noi. */
#ifdef WALTEX_HOSTED
#include <stdint.h>
#include <stddef.h>
#else

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;

typedef uint32_t size_t;
typedef uint32_t uintptr_t;

#define NULL ((void *)0)

#endif /* WALTEX_HOSTED */
#endif /* WALTEX_TYPES_H */
```

- [ ] **Step 5: `include/io.h`**

```c
#ifndef WALTEX_IO_H
#define WALTEX_IO_H

#include "types.h"

/* Le porte I/O dell'x86 sono uno spazio di indirizzamento separato dalla
   memoria: si raggiungono solo con le istruzioni in/out. Il vincolo "Nd"
   permette al compilatore di usare la forma immediata per porte < 256. */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Scrivere sulla porta 0x80 (usata solo per il POST code) costa circa un ciclo
   di bus e non ha effetti collaterali: è il modo canonico per dare a un chip
   lento il tempo di reagire fra due out consecutive. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif
```

- [ ] **Step 6: `linker.ld`**

```ld
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
```

- [ ] **Step 7: `boot/multiboot.S`**

```gas
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
```

- [ ] **Step 8: `include/serial.h`**

```c
#ifndef WALTEX_SERIAL_H
#define WALTEX_SERIAL_H

void serial_init(void);
void serial_putc(char c);

#endif
```

- [ ] **Step 9: `kernel/serial.c`**

```c
#include "serial.h"
#include "io.h"

/* COM1. Driver in polling: nessun interrupt, nessun buffer. È il canale su cui
   girano i test, quindi conta che sia semplice e sempre funzionante, non che
   sia efficiente. */
#define COM1 0x3F8

#define REG_DATA         (COM1 + 0)
#define REG_INT_ENABLE   (COM1 + 1)
#define REG_FIFO_CTRL    (COM1 + 2)
#define REG_LINE_CTRL    (COM1 + 3)
#define REG_MODEM_CTRL   (COM1 + 4)
#define REG_LINE_STATUS  (COM1 + 5)

#define LSR_TX_EMPTY 0x20

void serial_init(void)
{
    outb(REG_INT_ENABLE, 0x00);   /* nessun interrupt: leggiamo in polling  */
    outb(REG_LINE_CTRL,  0x80);   /* DLAB=1: i primi due registri diventano
                                     il divisore del baud rate             */
    outb(REG_DATA,       0x03);   /* divisore 3 -> 38400 baud, byte basso   */
    outb(REG_INT_ENABLE, 0x00);   /* byte alto del divisore                 */
    outb(REG_LINE_CTRL,  0x03);   /* DLAB=0, 8 bit, no parità, 1 stop bit   */
    outb(REG_FIFO_CTRL,  0xC7);   /* FIFO on, svuotate, soglia 14 byte      */
    outb(REG_MODEM_CTRL, 0x0B);   /* DTR e RTS attivi, OUT2 abilitato       */
}

void serial_putc(char c)
{
    while ((inb(REG_LINE_STATUS) & LSR_TX_EMPTY) == 0)
        ;
    outb(REG_DATA, (uint8_t)c);
}
```

- [ ] **Step 10: `kernel/main.c`, versione provvisoria**

```c
#include "types.h"
#include "serial.h"

/* Valore che un loader Multiboot 1 conforme lascia in eax. Se non corrisponde,
   non sappiamo nulla di affidabile sull'ambiente in cui siamo partiti. */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* Provvisoria: dal Task 4 tutto l'output passa da kprintf. */
static void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

void kmain(uint32_t magic, void *mbinfo)
{
    (void)mbinfo;

    serial_init();
    serial_puts("waltex: booting\n");

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        serial_puts("waltex: magic Multiboot errato\n");
        return;
    }
    serial_puts("waltex: multiboot ok\n");
}
```

- [ ] **Step 11: `Makefile`**

```make
CC  := gcc
LD  := ld

CFLAGS  := -m32 -ffreestanding -nostdlib -fno-pie -fno-stack-protector \
           -fno-builtin -Wall -Wextra -std=gnu11 -g -Iinclude
ASFLAGS := -m32 -g -Iinclude
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib

BUILD  := build
KERNEL := $(BUILD)/waltex.elf

CSRC := $(wildcard kernel/*.c)
SSRC := boot/multiboot.S $(wildcard kernel/*.S)
OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(CSRC)) $(patsubst %.S,$(BUILD)/%.o,$(SSRC))

QEMU      := qemu-system-i386
QEMUFLAGS := -kernel $(KERNEL) -no-reboot

all: $(KERNEL)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(KERNEL): $(OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

run: $(KERNEL)
	$(QEMU) $(QEMUFLAGS) -serial stdio

# -s apre il gdbserver sulla 1234, -S ferma la CPU al primo istruzione:
#   gdb -ex 'target remote :1234' -ex 'symbol-file build/waltex.elf'
debug: $(KERNEL)
	$(QEMU) $(QEMUFLAGS) -serial stdio -s -S

test: $(KERNEL)
	./tests/smoke.sh $(KERNEL)

clean:
	rm -rf $(BUILD)

.PHONY: all run debug test clean
```

- [ ] **Step 12: Build e verifica**

Run: `make`
Atteso: `build/waltex.elf` creato, nessun warning.

Run: `file build/waltex.elf`
Atteso: `ELF 32-bit LSB executable, Intel 80386`.

Run: `./tests/smoke.sh`
Atteso: `ok` sui primi due marker, `FAIL` su `waltex: M1 ok`. Se i primi due
sono `ok`, il boot funziona e il grosso del rischio di M1 è superato.

Run: `make run`
Atteso: nel terminale compaiono le due righe; la finestra QEMU resta nera,
perché il VGA non è ancora implementato.

- [ ] **Step 13: Commit**

```bash
git add Makefile linker.ld boot include kernel tests .gitignore CLAUDE.md docs
git commit -m "M1: build system, header Multiboot, driver seriale COM1"
```

---

## Task 2 [WALTER]: `kernel/vga.c`

**Files:**
- Create: `kernel/vga.c`
- Create: `include/vga.h` (lo scrive Claude, Step 1)
- Create: `kernel/selftest.c`, `include/selftest.h` (li scrive Claude, Step 2)
- Modify: `kernel/main.c`

**Interfaces:**
- Consumes: `outb` da `io.h`.
- Produces: `vga_init(void)`, `vga_clear(void)`, `vga_putc(char)`.

- [ ] **Step 1 [CLAUDE]: `include/vga.h`**

```c
#ifndef WALTEX_VGA_H
#define WALTEX_VGA_H

/* Text mode 80x25. vga_init lascia lo schermo pulito e il cursore a (0,0). */
void vga_init(void);
void vga_clear(void);

/* Scrive un carattere alla posizione corrente e avanza il cursore.
   '\n' va a inizio riga successiva. Raggiunto il fondo, il contenuto scorre
   di una riga verso l'alto e l'ultima riga resta vuota. */
void vga_putc(char c);

#endif
```

- [ ] **Step 2 [CLAUDE]: il self-check, che deve fallire**

I self-check girano dentro la VM e riportano l'esito su seriale: è il solo modo
di verificare codice che parla con l'hardware. `kernel/selftest.c` crescerà a
ogni milestone.

`include/selftest.h`:

```c
#ifndef WALTEX_SELFTEST_H
#define WALTEX_SELFTEST_H

/* Esegue i self-check e riporta ogni esito su seriale nella forma
   "selftest: ok -- <nome>" oppure "selftest: FAIL -- <nome>".
   Ritorna il numero di check falliti. */
int selftest_run(void);

#endif
```

`kernel/selftest.c`:

```c
#include "selftest.h"
#include "types.h"
#include "serial.h"
#include "vga.h"

#define VGA_MEM   ((volatile uint16_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25

static int failures;

static void puts_(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

static void report(const char *name, int ok)
{
    puts_(ok ? "selftest: ok   -- " : "selftest: FAIL -- ");
    puts_(name);
    serial_putc('\n');
    if (!ok)
        failures++;
}

/* Il framebuffer è memoria come tutte le altre: dopo aver scritto un
   carattere possiamo rileggere la cella e controllare cosa c'è davvero. */
static void check_putc(void)
{
    vga_clear();
    vga_putc('X');
    report("vga_putc scrive il carattere in (0,0)",
           (VGA_MEM[0] & 0xFF) == 'X');
    report("vga_putc lascia un attributo non nullo",
           (VGA_MEM[0] >> 8) != 0);
}

static void check_clear(void)
{
    vga_putc('Y');
    vga_clear();
    report("vga_clear azzera i caratteri",
           (VGA_MEM[0] & 0xFF) == ' ' || (VGA_MEM[0] & 0xFF) == 0);
}

static void check_newline(void)
{
    vga_clear();
    vga_putc('A');
    vga_putc('\n');
    vga_putc('B');
    report("newline porta il cursore a inizio riga 1",
           (VGA_MEM[VGA_COLS] & 0xFF) == 'B');
}

/* Riempita l'ultima riga, il contenuto deve salire di una posizione: quello
   che era in riga 1 finisce in riga 0. */
static void check_scroll(void)
{
    int i;

    vga_clear();
    vga_putc('0');
    vga_putc('\n');
    vga_putc('1');
    for (i = 0; i < VGA_ROWS - 1; i++)
        vga_putc('\n');

    report("lo scroll fa salire le righe",
           (VGA_MEM[0] & 0xFF) == '1');
}

int selftest_run(void)
{
    failures = 0;

    check_putc();
    check_clear();
    check_newline();
    check_scroll();

    vga_clear();
    return failures;
}
```

In `kernel/main.c`: aggiungere `#include "vga.h"` e `#include "selftest.h"` in
testa, far scrivere la `puts` provvisoria su **entrambi** i canali (così la
verifica visiva non richiede codice usa-e-getta), e inizializzare il VGA per
primo.

```c
/* Provvisoria: dal Task 4 tutto l'output passa da kprintf. */
static void kputs(const char *s)
{
    while (*s) {
        serial_putc(*s);
        vga_putc(*s);
        s++;
    }
}

void kmain(uint32_t magic, void *mbinfo)
{
    (void)mbinfo;

    vga_init();
    serial_init();

    kputs("waltex: booting\n");

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        kputs("waltex: magic Multiboot errato\n");
        return;
    }
    kputs("waltex: multiboot ok\n");

    if (selftest_run() == 0)
        kputs("waltex: selftest ok\n");
    else
        kputs("waltex: selftest con errori\n");
}
```

La vecchia `serial_puts` va rimossa: non serve più a nulla.

- [ ] **Step 3: Eseguire i self-check per vederli fallire**

Run: `make && make test`
Atteso: errore di link, `undefined reference to vga_init` — `kernel/vga.c` non
esiste ancora. Questo *è* il test rosso.

- [ ] **Step 4 [WALTER]: scrivere `kernel/vga.c`**

L'implementazione la scrivi tu. Quello che serve sapere, e che non è deducibile
dal codice:

Il framebuffer del text mode sta all'indirizzo fisico **`0xB8000`**, griglia di
**80 colonne per 25 righe**. Ogni cella occupa **2 byte**: il byte basso è il
codice del carattere, il byte alto è l'attributo, costruito come
`(sfondo << 4) | primo_piano`. I colori vanno da 0 (nero) a 15 (bianco
brillante); `0x07` è grigio chiaro su nero, la combinazione classica. Il
puntatore al framebuffer va dichiarato `volatile`: il compilatore non deve
ottimizzare via scritture che a lui sembrano inutili.

Il cursore hardware si muove con due porte: si scrive l'indice del registro
sulla **`0x3D4`** e il valore sulla **`0x3D5`**. Il registro **`0x0F`** riceve
il byte basso della posizione lineare (`riga * 80 + colonna`), il **`0x0E`** il
byte alto.

Da decidere tu: dove tieni la posizione corrente, come implementi lo scroll
(copia delle righe più svuotamento dell'ultima), se `vga_init` fa altro oltre a
pulire.

Attenzione: non c'è `memset` né `memcpy`. Se ti servono, scrivili — ma per lo
scroll un ciclo esplicito è più chiaro.

Prima di scrivere, se vuoi che spieghi il perché di `volatile` qui, o come si
ragiona sull'indice lineare, chiedi.

- [ ] **Step 5: Eseguire i self-check finché non passano**

Run: `make && make test`
Atteso: quattro righe `selftest: ok` sull'output, e `waltex: selftest ok`.

Se un check falla, `make run` mostra lo schermo reale: spesso il difetto si
vede a occhio prima che dal test.

- [ ] **Step 6: Verifica visiva**

Run: `make run`
Atteso: le righe di boot leggibili nella finestra QEMU, e il cursore
posizionato dopo l'ultimo carattere scritto.

Nota: `selftest_run` termina con `vga_clear()`, quindi al momento dello scatto
lo schermo mostra solo la riga `selftest ok`. Per vedere tutte le righe,
commenta temporaneamente il `vga_clear()` finale in `selftest.c` — quello sì è
un file mio, quindi se preferisci lo tolgo io.

- [ ] **Step 7: Commit**

```bash
git add include/vga.h include/selftest.h kernel/vga.c kernel/selftest.c kernel/main.c
git commit -m "M1: driver VGA text mode con scroll e cursore"
```

---

## Task 3 [WALTER]: `kernel/kprintf.c`

**Files:**
- Create: `kernel/kprintf.c`
- Create: `include/kprintf.h` (lo scrive Claude, Step 1)
- Create: `tests/host/Makefile`, `tests/host/test_kprintf.c` (li scrive
  Claude, Step 2)
- Modify: `Makefile`

**Interfaces:**
- Consumes: `vga_putc` da `vga.h`, `serial_putc` da `serial.h`.
- Produces: `kvprintf(void (*putc)(char), const char *fmt, va_list ap)` e
  `kprintf(const char *fmt, ...)`.

- [ ] **Step 1 [CLAUDE]: `include/kprintf.h`**

```c
#ifndef WALTEX_KPRINTF_H
#define WALTEX_KPRINTF_H

/* stdarg.h è un header del compilatore, non della libc: disponibile anche in
   freestanding. */
#include <stdarg.h>

#include "types.h"

/* Il cuore del formatter è separato dalla destinazione: riceve la funzione a
   cui consegnare un carattere alla volta. Questo lo rende compilabile e
   testabile col gcc dell'host, senza VGA né seriale.

   Specificatori richiesti: %d (int32 con segno), %x (uint32 esadecimale
   minuscolo, senza zeri iniziali), %s, %c, %%. Uno specificatore non
   riconosciuto viene emesso letteralmente, preceduto dal suo '%'. */
void kvprintf(void (*putc)(char), const char *fmt, va_list ap);

/* Scrive su VGA e su COM1. */
void kprintf(const char *fmt, ...);

#endif
```

- [ ] **Step 2 [CLAUDE]: scrivere i test host, che devono fallire**

`tests/host/test_kprintf.c`:

```c
/* Compilato col gcc dell'host, senza QEMU: ciclo di prova da millisecondi.
   WALTEX_HOSTED fa arrivare i tipi da stdint.h invece che da types.h. */
#define WALTEX_HOSTED 1

#include <stdio.h>
#include <string.h>

#include "kprintf.h"

/* kprintf.c fa riferimento ai due sink del kernel: qui non esistono, quindi
   li rimpiazziamo con stub inerti. Solo kvprintf è sotto test. */
void vga_putc(char c) { (void)c; }
void serial_putc(char c) { (void)c; }

static char buf[1024];
static size_t len;

static void collect(char c)
{
    if (len < sizeof(buf) - 1)
        buf[len++] = c;
}

static int failures;

static void expect(const char *want, const char *fmt, ...)
{
    va_list ap;

    len = 0;
    va_start(ap, fmt);
    kvprintf(collect, fmt, ap);
    va_end(ap);
    buf[len] = '\0';

    if (strcmp(buf, want) == 0) {
        printf("ok   -- \"%s\" -> \"%s\"\n", fmt, buf);
    } else {
        printf("FAIL -- \"%s\": atteso \"%s\", ottenuto \"%s\"\n",
               fmt, want, buf);
        failures++;
    }
}

int main(void)
{
    /* testo letterale */
    expect("", "");
    expect("ciao", "ciao");
    expect("a\nb", "a\nb");

    /* %d */
    expect("0", "%d", 0);
    expect("42", "%d", 42);
    expect("-42", "%d", -42);
    expect("2147483647", "%d", 2147483647);
    /* il minimo int32: negarlo va in overflow, è l'errore classico */
    expect("-2147483648", "%d", (int)(-2147483647 - 1));

    /* %x, minuscolo e senza zeri iniziali */
    expect("0", "%x", 0u);
    expect("ff", "%x", 255u);
    expect("2badb002", "%x", 0x2BADB002u);
    expect("ffffffff", "%x", 0xFFFFFFFFu);

    /* %s e %c */
    expect("waltex", "%s", "waltex");
    expect("", "%s", "");
    expect("A", "%c", 'A');

    /* %% e specificatore ignoto */
    expect("100%", "100%%");
    expect("%q", "%q");

    /* combinazioni */
    expect("tick=100 addr=b8000", "tick=%d addr=%x", 100, 0xB8000u);
    expect("[waltex] M1 ok", "[%s] M%d ok", "waltex", 1);

    if (failures == 0) {
        printf("tutti i test del formatter passano\n");
        return 0;
    }
    printf("%d test falliti\n", failures);
    return 1;
}
```

`tests/host/Makefile`:

```make
CFLAGS := -m32 -Wall -Wextra -std=gnu11 -g -DWALTEX_HOSTED \
          -I../../include

BIN := test_kprintf

all: run

$(BIN): test_kprintf.c ../../kernel/kprintf.c
	$(CC) $(CFLAGS) -o $@ $^

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all run clean
```

In `Makefile` alla radice, sostituire il target `test` e aggiungere
`host-test`:

```make
host-test:
	$(MAKE) -C tests/host

test: $(KERNEL) host-test
	./tests/smoke.sh $(KERNEL)
```

e aggiungere `host-test` a `.PHONY`, più la pulizia in `clean`:

```make
clean:
	rm -rf $(BUILD)
	$(MAKE) -C tests/host clean
```

- [ ] **Step 3: Eseguire i test host per vederli fallire**

Run: `make host-test`
Atteso: errore di compilazione, `kernel/kprintf.c` non esiste. Test rosso.

- [ ] **Step 4 [WALTER]: scrivere `kernel/kprintf.c`**

L'implementazione la scrivi tu. Due sole funzioni: `kvprintf`, che cammina sul
formato e consegna caratteri alla callback, e `kprintf`, che apre la `va_list`
e la passa a `kvprintf` con un sink che scrive su entrambe le destinazioni.

Il caso su cui inciampano quasi tutti è `%d` con `-2147483648`: non ha
corrispettivo positivo rappresentabile in `int32_t`, quindi negarlo per
riutilizzare il codice dei positivi non funziona. Il test lo copre.

Ricorda: non hai `snprintf`, non hai `strlen`, non hai buffer dinamici. Per
convertire un numero in cifre servirà un array locale a dimensione fissa.

- [ ] **Step 5: Eseguire i test host finché non passano**

Run: `make host-test`
Atteso: tutte righe `ok`, poi `tutti i test del formatter passano`.

- [ ] **Step 6: Verificare che kprintf compili anche nel kernel**

Run: `make`
Atteso: build senza warning. Le stesse righe di codice ora vivono in due
ambienti diversi.

- [ ] **Step 7: Commit**

```bash
git add include/kprintf.h kernel/kprintf.c tests/host Makefile
git commit -m "M1: formatter kprintf con test host"
```

---

## Task 4 [CLAUDE]: chiudere M1

**Files:**
- Modify: `kernel/main.c`, `CLAUDE.md`

**Interfaces:**
- Consumes: tutto quanto prodotto dai Task 1-3.
- Produces: il marker `waltex: M1 ok`, che chiude lo smoke test.

- [ ] **Step 1: `kernel/main.c`, versione definitiva**

Rimuove `serial_puts` provvisoria: da qui in poi ogni output del kernel passa
da `kprintf` e finisce su entrambi i canali.

```c
#include "types.h"
#include "vga.h"
#include "serial.h"
#include "kprintf.h"
#include "selftest.h"

/* Valore che un loader Multiboot 1 conforme lascia in eax. */
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

    kprintf("waltex: M1 ok\n");
}
```

Nota: `selftest_run` termina con `vga_clear()`, quindi le righe di boot
scompaiono dallo schermo ma restano su seriale. Se preferisci vederle in
finestra, sposta la chiamata a `selftest_run` prima del primo `kprintf`.

- [ ] **Step 2: Eseguire tutta la suite**

Run: `make clean && make test`
Atteso, nell'ordine: i test host tutti `ok`, poi i tre marker dello smoke test
`ok`, exit code 0.

Run: `echo $?`
Atteso: `0`.

- [ ] **Step 3: Verifica visiva finale**

Run: `make run`
Atteso: la finestra QEMU mostra testo, il terminale mostra le stesse righe più
i `selftest: ok`.

- [ ] **Step 4: Aggiornare lo stato in `CLAUDE.md`**

Nella sezione "Stato corrente", sostituire la riga della milestone con:

```markdown
Milestone in corso: **M2 — GDT**. M1 chiusa: boot Multiboot, VGA text mode,
seriale COM1, kprintf, test host e smoke test in QEMU.
```

- [ ] **Step 5: Commit**

```bash
git add kernel/main.c CLAUDE.md
git commit -m "M1: output unificato su kprintf, milestone chiusa"
```

---

## Definizione di M1 completata

- `make` produce un ELF 32-bit senza warning.
- `make test` esce con 0: test host verdi e tre marker trovati su seriale.
- `make run` mostra testo nella finestra QEMU e sul terminale.
- `make debug` permette di attaccarsi con gdb e mettere un breakpoint su
  `kmain`.
- I quattro self-check del VGA passano dentro la VM.
- Nessun riferimento a GDT, IDT, PIC, timer, tastiera o task nel codice.
