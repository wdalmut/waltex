# M2 — GDT: piano di implementazione

> **Nota sulla modalità di lavoro.** I task marcati **[WALTER]** sono
> deliberatamente privi di implementazione: contengono interfaccia, test, dati
> hardware e concetti, ma il codice lo scrive Walter (vedi `CLAUDE.md`). I task
> marcati **[CLAUDE]** sono infrastruttura e vanno implementati come scritto.

**Obiettivo:** sostituire la GDT lasciata dal bootloader con una nostra, e
ricaricare i selettori di segmento perché la CPU la usi davvero.

**Architettura:** una tabella di tre descrittori — null, codice ring 0, dati
ring 0 — tutti con base 0 e limite 4 GiB, cioè un modello di memoria piatto in
cui la segmentazione esiste ma non segmenta nulla. Il caricamento avviene con
`lgdt` seguito da un far jump per ricaricare `cs` e da `mov` per gli altri
selettori. La verifica usa `sgdt` per rileggere dalla CPU quale tabella sta
effettivamente usando, e ne ispeziona i byte.

**Stack:** gcc `-m32 -ffreestanding`, GNU as, QEMU, gdb.

## Vincoli globali

Valgono quelli di M1, invariati, più:

- Nessuna allocazione: la GDT è un array `static` in `.bss`.
- Nessun descrittore ring 3, nessun TSS, nessun segmento con base diversa da 0.
  Servono a user mode e a task switching hardware, entrambi fuori scope.
- Gli interrupt restano disabilitati per tutta M2: l'IDT arriva in M3.

## Il problema di questa milestone: se funziona, non si vede

Il bootloader ci ha già consegnato la CPU in protected mode con segmenti piatti.
La nostra GDT sarà **funzionalmente identica** a quella che sostituisce. Quindi
un `gdt_init` corretto non cambia nulla di osservabile, e un `gdt_init` scritto
"quasi bene" può sembrare corretto per un bel po'.

Da qui la strategia di verifica: non basta che il kernel sopravviva, bisogna
**chiedere alla CPU quale tabella sta usando** e ispezionarne il contenuto.
L'istruzione `sgdt` scrive in memoria base e limite del registro GDTR, e da lì
si rileggono i descrittori byte per byte. È lo stesso principio della rilettura
dei registri del cursore in M1: su hardware muto, l'unica conferma è
riascoltare.

## Struttura dei file al termine di M2

| File | Responsabilità | Chi |
|---|---|---|
| `include/gdt.h` | `gdt_init`, costanti dei selettori | CLAUDE |
| `kernel/gdt.S` | `gdt_flush`: `lgdt`, far jump, ricarica selettori | CLAUDE |
| `kernel/gdt.c` | struct del descrittore, tabella, bit-packing | **WALTER** |
| `kernel/selftest.c` | check via `sgdt` | CLAUDE |
| `kernel/main.c` | chiamata a `gdt_init` | CLAUDE |
| `tests/smoke.sh` | marker aggiornato a `waltex: M2 ok` | CLAUDE |

**Interfacce prodotte da M2:**

```c
#define GDT_SEL_CODE 0x08
#define GDT_SEL_DATA 0x10

void gdt_init(void);        /* riempie la tabella e la carica */
void gdt_flush(void *gdtr); /* in assembly: lgdt + ricarica dei selettori */
```

---

## Task 1 [CLAUDE]: header, `gdt.S`, self-check rossi

**Files:**
- Create: `include/gdt.h`, `kernel/gdt.S`
- Modify: `kernel/main.c`, `kernel/selftest.c`, `tests/smoke.sh`

**Interfaces:**
- Consumes: `kprintf`, `serial_putc`.
- Produces: `gdt_flush`, le costanti dei selettori, e i check che falliscono
  finché `gdt.c` non esiste.

- [ ] **Step 1: `include/gdt.h`**

Le costanti dei selettori servono sia al C sia all'assembly, quindi usiamo il
pattern `__ASSEMBLER__` di cui abbiamo già parlato per `multiboot.h`.

```c
#ifndef WALTEX_GDT_H
#define WALTEX_GDT_H

/* Un selettore di segmento e' indice * 8, piu' due bit di RPL e uno di TI che
   qui valgono zero. Indice 1 = codice, indice 2 = dati. */
#define GDT_SEL_CODE 0x08
#define GDT_SEL_DATA 0x10

#ifndef __ASSEMBLER__

#include "types.h"

/* Costruisce la tabella e la carica. Dopo questa chiamata la CPU non usa piu'
   la GDT del bootloader. */
void gdt_init(void);

/* Definita in gdt.S. Riceve il puntatore alla struttura a 6 byte che lgdt si
   aspetta: 2 byte di limite seguiti da 4 di base. */
void gdt_flush(void *gdtr);

#endif /* __ASSEMBLER__ */
#endif
```

- [ ] **Step 2: `kernel/gdt.S`**

```gas
#include "gdt.h"

/* Carica la GDT e ricarica tutti i selettori di segmento.
   I registri di segmento contengono una copia nascosta del descrittore,
   caricata all'ultimo assegnamento: finche' non li si riscrive, la CPU
   continua a usare i vecchi descrittori anche dopo lgdt. */

.section .text
.global gdt_flush
.type gdt_flush, @function
gdt_flush:
    movl 4(%esp), %eax           /* primo argomento: puntatore al gdtr */
    lgdt (%eax)

    /* I selettori di dati si ricaricano con una mov. */
    movw $GDT_SEL_DATA, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* cs no: non e' scrivibile con mov. L'unico modo di cambiarlo e' un
       trasferimento di controllo che porti con se' un nuovo selettore, cioe'
       un far jump. Il salto e' a due byte piu' avanti, ma passa da $GDT_SEL_CODE
       e questo e' l'unico scopo. */
    ljmp $GDT_SEL_CODE, $1f
1:
    ret

.size gdt_flush, . - gdt_flush

.section .note.GNU-stack, "", @progbits
```

- [ ] **Step 3: i self-check in `kernel/selftest.c`**

Aggiungere `#include "gdt.h"` in testa, e questo blocco prima di
`selftest_run`:

```c
/* Il registro GDTR non e' leggibile direttamente: sgdt lo scrive in memoria,
   in un blocco di 6 byte con lo stesso formato che lgdt si aspetta. */
struct gdtr_image {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static void read_gdtr(struct gdtr_image *out)
{
    __asm__ volatile ("sgdt %0" : "=m"(*out));
}

static uint16_t read_cs(void)
{
    uint16_t v;
    __asm__ volatile ("movw %%cs, %0" : "=r"(v));
    return v;
}

static uint16_t read_ds(void)
{
    uint16_t v;
    __asm__ volatile ("movw %%ds, %0" : "=r"(v));
    return v;
}

static uint16_t read_ss(void)
{
    uint16_t v;
    __asm__ volatile ("movw %%ss, %0" : "=r"(v));
    return v;
}

/* Il bit 0 dell'access byte e' "accessed": lo mette la CPU quando il
   descrittore viene caricato in un registro di segmento. Non e' sotto il
   controllo di chi scrive la tabella, quindi il confronto lo ignora. */
#define ACCESS_A 0x01

static int descrittore_piatto_ok(const uint8_t *d, uint8_t access)
{
    return d[0] == 0xFF &&   /* limite  0-7   */
           d[1] == 0xFF &&   /* limite  8-15  */
           d[2] == 0x00 &&   /* base    0-7   */
           d[3] == 0x00 &&   /* base    8-15  */
           d[4] == 0x00 &&   /* base   16-23  */
           (d[5] | ACCESS_A) == (access | ACCESS_A) &&
           d[6] == 0xCF &&   /* nibble alto: flag; basso: limite 16-19 */
           d[7] == 0x00;     /* base   24-31  */
}

static void check_gdt(void)
{
    struct gdtr_image gdtr;
    const uint8_t *tabella;

    read_gdtr(&gdtr);
    tabella = (const uint8_t *)gdtr.base;

    /* Tre descrittori da 8 byte: il limite e' la dimensione meno uno. */
    report("la GDT caricata ha tre descrittori",
           gdtr.limit == 3 * 8 - 1);

    /* Il descrittore 0 deve essere tutto zero: la CPU lo esige, e un
       selettore che lo referenzia e' un errore per costruzione. */
    report("il descrittore null e' azzerato",
           tabella[0] == 0 && tabella[1] == 0 && tabella[2] == 0 &&
           tabella[3] == 0 && tabella[4] == 0 && tabella[5] == 0 &&
           tabella[6] == 0 && tabella[7] == 0);

    report("il descrittore di codice e' piatto ring 0",
           descrittore_piatto_ok(tabella + 8, 0x9A));

    report("il descrittore di dati e' piatto ring 0",
           descrittore_piatto_ok(tabella + 16, 0x92));

    /* Non basta che la tabella sia giusta: i selettori devono puntarci. */
    report("cs usa il selettore di codice", read_cs() == GDT_SEL_CODE);
    report("ds usa il selettore di dati",   read_ds() == GDT_SEL_DATA);
    report("ss usa il selettore di dati",   read_ss() == GDT_SEL_DATA);
}
```

e la chiamata in `selftest_run`, **prima** di `check_putc`:

```c
    check_gdt();
```

- [ ] **Step 4: chiamare `gdt_init` in `kernel/main.c`**

Aggiungere `#include "gdt.h"` e, subito dopo la riga
`kprintf("waltex: multiboot ok\n");`:

```c
    gdt_init();
    kprintf("waltex: gdt caricata\n");
```

La `kprintf` dopo la chiamata non è decorativa: se la GDT fosse malformata, la
CPU non arriverebbe a eseguirla. È il primo segnale di vita.

- [ ] **Step 5: aggiornare il marker in `tests/smoke.sh`**

```bash
LAST_MARKER="waltex: M2 ok"
MARKERS=("waltex: booting" "waltex: multiboot ok" "waltex: gdt caricata" "$LAST_MARKER")
```

e in `kernel/main.c`, l'ultima riga di `kmain` diventa:

```c
    kprintf("waltex: M2 ok\n");
```

- [ ] **Step 6: verificare che sia rosso**

Run: `make`
Atteso: `undefined reference to 'gdt_init'`. È il test rosso.

- [ ] **Step 7: Commit**

```bash
git add include/gdt.h kernel/gdt.S kernel/selftest.c kernel/main.c tests/smoke.sh
git commit -m "M2: infrastruttura GDT e self-check via sgdt"
```

---

## Task 2 [WALTER]: `kernel/gdt.c`

**Files:**
- Create: `kernel/gdt.c`

**Interfaces:**
- Consumes: `gdt_flush` da `gdt.h`.
- Produces: `gdt_init(void)`.

- [ ] **Step 1: capire cosa c'è dentro un descrittore**

Otto byte che descrivono un segmento. Base e limite sono **spezzati in pezzi
non contigui**, per ragioni di compatibilità con il 286 che aveva descrittori
da 6 byte: i campi aggiunti dal 386 sono stati infilati negli spazi liberi.

| Byte | Contenuto |
|---|---|
| 0-1 | limite, bit 0-15 |
| 2-3 | base, bit 0-15 |
| 4 | base, bit 16-23 |
| 5 | **access byte** |
| 6 | nibble basso: limite bit 16-19 — nibble alto: **flag** |
| 7 | base, bit 24-31 |

L'**access byte**, bit per bit:

| Bit | Nome | Valore per noi |
|---|---|---|
| 7 | P — presente | 1 |
| 6-5 | DPL — livello di privilegio | 00, ring 0 |
| 4 | S — 1 se codice/dati, 0 se di sistema | 1 |
| 3 | E — eseguibile | 1 per codice, 0 per dati |
| 2 | DC — direzione / conforming | 0 |
| 1 | RW — leggibile (codice) / scrivibile (dati) | 1 |
| 0 | A — accessed, la scrive la CPU | 0 |

I **flag**, nel nibble alto del byte 6:

| Bit | Nome | Valore per noi |
|---|---|---|
| 3 | G — granularità: 0 = byte, 1 = pagine da 4 KiB | 1 |
| 2 | D/B — 0 = 16 bit, 1 = 32 bit | 1 |
| 1 | L — long mode | 0 |
| 0 | AVL — libero per il sistema operativo | 0 |

Con `G = 1` il limite si misura in pagine da 4 KiB, quindi `0xFFFFF` significa
`(0xFFFFF + 1) × 4096` byte, cioè esattamente 4 GiB: tutto lo spazio
indirizzabile. Base 0 e limite 4 GiB su ogni segmento è ciò che si chiama
**modello piatto**: la segmentazione resta accesa perché la CPU la impone, ma
non segmenta niente.

- [ ] **Step 2: scrivere `kernel/gdt.c`**

Ti servono:

- una `struct` da 8 byte per il descrittore, con `__attribute__((packed))`.
  Verificato: con i campi disposti come `uint16, uint16, uint8, uint8, uint8,
  uint8` non c'e' padding nemmeno senza, perche' ognuno cade gia' su un
  multiplo del proprio allineamento. Mettilo comunque, per la stessa ragione
  della struct Multiboot: dichiara che il layout e' imposto dall'esterno, e ti
  copre se un giorno riordini i campi o ne cambi un tipo;
- una `struct` da 6 byte per il puntatore che `lgdt` legge: `uint16_t` di
  limite seguito da `uint32_t` di base, anch'essa `packed`;
- un array `static` di 3 descrittori;
- una funzione che riempie un descrittore dati base, limite, access e flag,
  distribuendo i pezzi nei campi giusti;
- `gdt_init()` che riempie i tre, prepara il puntatore e chiama `gdt_flush`.

Il limite nel puntatore è la **dimensione meno uno**: `sizeof(gdt) - 1`. È il
valore dell'ultimo byte valido, non la lunghezza — sbagliarlo di uno significa
che l'ultimo descrittore è fuori tabella.

Verifica di coerenza che puoi fare a mente prima di compilare: il descrittore
di codice deve venire fuori come i byte `FF FF 00 00 00 9A CF 00`, quello di
dati identico tranne `92` al posto di `9A`. Se il tuo bit-packing produce altro,
il problema è lì e non altrove.

- [ ] **Step 3: eseguire i self-check**

Run: `make && make test`
Atteso: i sette check di `check_gdt` verdi, più i sedici di M1.

- [ ] **Step 4: Commit**

```bash
git add kernel/gdt.c
git commit -m "M2: descrittori GDT e caricamento"
```

---

## Task 3 [CLAUDE]: chiudere M2

- [ ] **Step 1: eseguire tutta la suite da albero pulito**

Run: `make clean && make test`
Atteso: exit 0, 40 test host, 23 self-check, 4 marker.

- [ ] **Step 2: verificare a mano con gdb che la GDT sia la nostra**

```bash
make debug
# da un altro terminale:
gdb -q build/waltex.elf -ex 'target remote :1234' \
    -ex 'break gdt_flush' -ex 'continue' -ex 'info registers cs ds ss'
```

Poi, dopo il `ret`, gli stessi registri devono mostrare `0x08` e `0x10`.

- [ ] **Step 3: aggiornare lo stato in `CLAUDE.md` e proporre il commit**

---

## Lettura di accompagnamento

`boot/head.s` di Linux 0.01, la parte finale: costruisce la sua GDT e la carica
con la stessa sequenza `lgdt` più far jump, scritta interamente in assembly.
La macro `_set_seg_desc` in `include/asm/system.h` è lo stesso bit-packing che
scriverai in C, espresso come sequenza di assegnazioni a byte.

Vale la pena confrontare le due tabelle: quella di Linus ha quattro voci invece
di tre, perché include già un descrittore per lo spazio utente. E il limite dei
suoi segmenti non è 4 GiB ma 8 MiB — la memoria massima che quel kernel
supportava.

## Quando la VM riparte in silenzio

Da questa milestone un errore non produce un test rosso ma una tripla fault.
In ordine di utilità:

**`-d int,cpu_reset`.** Aggiungi il flag a `QEMUFLAGS` e rilancia: QEMU stampa
lo stato della CPU a ogni reset, compreso l'`EIP` dell'istruzione che ha
scatenato la fault. Ti dice *dove*, il che di solito basta.

**Breakpoint prima della `lgdt`.** Con `make debug`, `break gdt_flush` e poi
`x/24xb $eax` — dove `eax` contiene il puntatore al gdtr — ti mostra i 24 byte
della tabella così come li vedrà la CPU. Confrontali con
`00×8, FF FF 00 00 00 9A CF 00, FF FF 00 00 00 92 CF 00`. Un byte diverso è il
tuo bug, e lo vedi prima che la CPU lo esegua.

**I tre sospetti abituali**, in ordine di frequenza:

1. il limite nel puntatore calcolato come `sizeof(gdt)` invece di
   `sizeof(gdt) - 1`;
2. la `struct` del descrittore senza `packed`, quindi con padding che sposta
   tutti i campi;
3. il far jump dimenticato: `ds` è ricaricato ma `cs` contiene ancora il
   selettore del bootloader, che punta a un descrittore della vecchia tabella
   ormai non più in memoria. Questo di solito non fa fault subito — fa fault
   più tardi, nel posto sbagliato.
