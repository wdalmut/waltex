#include "selftest.h"
#include "types.h"
#include "io.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "rtc.h"
#include "keyboard.h"
#include "serial.h"
#include "vga.h"
#include "kprintf.h"

#define VGA_MEM   ((volatile uint16_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25

#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5
#define CRTC_CURSOR_HI 0x0E
#define CRTC_CURSOR_LO 0x0F

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
    vga_putc('0');            /* riga 0: e' quella che lo scroll butta via */
    vga_putc('\n');
    vga_putc('1');            /* riga 1, colonna 0 */
    vga_putc('Z');            /* riga 1, colonna 1 */
    for (i = 0; i < VGA_ROWS - 1; i++)
        vga_putc('\n');

    report("lo scroll fa salire le righe",
           (VGA_MEM[0] & 0xFF) == '1');

    /* Due celle adiacenti con contenuto diverso: un memcpy rotto che replica
       un byte le renderebbe uguali, e passerebbe il check qui sopra. */
    report("lo scroll sposta un blocco, non riempie",
           (VGA_MEM[1] & 0xFF) == 'Z');

    report("lo scroll svuota l'ultima riga",
           (VGA_MEM[(VGA_ROWS - 1) * VGA_COLS] & 0xFF) == ' ' ||
           (VGA_MEM[(VGA_ROWS - 1) * VGA_COLS] & 0xFF) == 0);
}

/* I registri del cursore del CRTC sono leggibili, non solo scrivibili: su
   hardware muto la rilettura e' l'unica conferma che esista. Verificato che
   QEMU la supporta. */
static uint16_t cursor_hw_pos(void)
{
    uint16_t pos;

    outb(VGA_CRTC_INDEX, CRTC_CURSOR_LO);
    pos = inb(VGA_CRTC_DATA);
    outb(VGA_CRTC_INDEX, CRTC_CURSOR_HI);
    pos |= (uint16_t)inb(VGA_CRTC_DATA) << 8;

    return pos;
}

static void check_cursor(void)
{
    int i;

    vga_clear();
    report("vga_clear porta il cursore hardware a (0,0)",
           cursor_hw_pos() == 0);

    vga_putc('A');
    vga_putc('B');
    vga_putc('C');
    report("il cursore hardware segue la scrittura",
           cursor_hw_pos() == 3);

    vga_putc('\n');
    report("il cursore hardware segue il newline",
           cursor_hw_pos() == VGA_COLS);

    /* Riempita l'ultima riga, lo scroll riporta la posizione a inizio
       ultima riga: anche il cursore hardware deve seguirla. */
    vga_clear();
    for (i = 0; i < VGA_ROWS; i++)
        vga_putc('\n');
    report("dopo lo scroll il cursore hardware e' a inizio ultima riga",
           cursor_hw_pos() == (VGA_ROWS - 1) * VGA_COLS);
}

static void check_color(void)
{
    int i;
    uint16_t atteso = (VGA_RED << 4) | VGA_WHITE;

    vga_set_color(VGA_WHITE, VGA_RED);
    vga_clear();
    vga_putc('E');

    report("vga_putc usa il colore corrente",
           (VGA_MEM[0] >> 8) == atteso);

    report("vga_clear riempie di spazi con il colore corrente",
           (VGA_MEM[1] & 0xFF) == ' ' && (VGA_MEM[1] >> 8) == atteso);

    /* La riga svuotata dallo scroll e' l'altro posto dove il colore corrente
       va applicato, ed e' quello che si dimentica. */
    vga_clear();
    for (i = 0; i < VGA_ROWS; i++)
        vga_putc('\n');
    report("la riga svuotata dallo scroll usa il colore corrente",
           (VGA_MEM[(VGA_ROWS - 1) * VGA_COLS] >> 8) == atteso);

    /* ...e lo spazio, come vga_clear: una riga svuotata dallo scroll e una
       svuotata da clear devono contenere la stessa cosa. */
    report("la riga svuotata dallo scroll contiene spazi",
           (VGA_MEM[(VGA_ROWS - 1) * VGA_COLS] & 0xFF) == ' ');

    /* Uno sfondo fuori dai 16 colori non deve accendere il bit 7, che non e'
       intensita' ma lampeggio. */
    vga_set_color(VGA_WHITE, VGA_YELLOW);
    vga_clear();
    vga_putc('E');
    report("uno sfondo fuori intervallo non accende il lampeggio",
           (VGA_MEM[0] & 0x8000) == 0);

    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

/* --- M2: la GDT ---------------------------------------------------------
   Una GDT corretta non produce nessun effetto visibile, perche' sostituisce
   quella del bootloader con una funzionalmente identica. Quindi non chiediamo
   "il kernel e' sopravvissuto?" ma "quale tabella sta usando la CPU?", e ne
   ispezioniamo i byte. */

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

/* I byte attesi di un descrittore piatto ring 0, base 0 e limite 4 GiB.
   L'unica differenza fra codice e dati e' il byte di access.

   Il bit 0 dell'access byte e' "accessed": lo mette la CPU quando il
   descrittore viene caricato in un registro di segmento, quindi non e' sotto
   il controllo di chi scrive la tabella. Confrontarlo renderebbe il test
   dipendente dal momento in cui gira e dall'emulazione. Lo ignoriamo. */
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
    int i;
    int null_azzerato = 1;

    read_gdtr(&gdtr);
    tabella = (const uint8_t *)gdtr.base;

    /* Tre descrittori da 8 byte: il limite e' la dimensione meno uno. */
    report("la GDT caricata ha tre descrittori",
           gdtr.limit == 3 * 8 - 1);

    /* Il descrittore 0 deve essere tutto zero: la CPU lo esige, e un
       selettore che lo referenzia e' un errore per costruzione. */
    for (i = 0; i < 8; i++)
        if (tabella[i] != 0)
            null_azzerato = 0;
    report("il descrittore null e' azzerato", null_azzerato);

    report("il descrittore di codice e' piatto ring 0",
           descrittore_piatto_ok(tabella + 8, 0x9A));

    report("il descrittore di dati e' piatto ring 0",
           descrittore_piatto_ok(tabella + 16, 0x92));

    /* Non basta che la tabella sia giusta: i registri di segmento tengono una
       copia nascosta del descrittore e vanno riscritti perche' la rileggano. */
    report("cs usa il selettore di codice", read_cs() == GDT_SEL_CODE);
    report("ds usa il selettore di dati",   read_ds() == GDT_SEL_DATA);
    report("ss usa il selettore di dati",   read_ss() == GDT_SEL_DATA);
}

/* --- M3: IDT, dispatch, PIC ---------------------------------------------- */

/* Come il GDTR, l'IDTR si legge solo scrivendolo in memoria. */
struct idtr_image {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static void read_idtr(struct idtr_image *out)
{
    __asm__ volatile ("sidt %0" : "=m"(*out));
}

/* Gli stub sono simboli globali: possiamo confrontare l'indirizzo scritto nel
   gate con quello vero, invece di fidarci. */
extern void isr0(void);
extern void irq0(void);

/* L'offset in un gate e' spezzato in due pezzi, come la base nella GDT. */
static uint32_t gate_offset(const uint8_t *g)
{
    return  (uint32_t)g[0]        |
           ((uint32_t)g[1] <<  8) |
           ((uint32_t)g[6] << 16) |
           ((uint32_t)g[7] << 24);
}

static uint16_t gate_selector(const uint8_t *g)
{
    return (uint16_t)g[2] | ((uint16_t)g[3] << 8);
}

static void check_idt(void)
{
    struct idtr_image idtr;
    const uint8_t *tab;

    read_idtr(&idtr);
    tab = (const uint8_t *)idtr.base;

    report("l'IDT caricata ha 256 gate",
           idtr.limit == IDT_ENTRIES * 8 - 1);

    report("il gate 0 punta a isr0",
           gate_offset(tab) == (uint32_t)isr0);

    report("il gate 32 punta a irq0",
           gate_offset(tab + IRQ_BASE * 8) == (uint32_t)irq0);

    report("il gate 0 usa il selettore di codice",
           gate_selector(tab) == GDT_SEL_CODE);

    /* 0x8E: presente, DPL 0, descrittore di sistema, interrupt gate a 32 bit.
       Il byte 4 deve essere zero, e' riservato. */
    report("il gate 0 e' un interrupt gate a 32 bit, DPL 0",
           tab[5] == 0x8E && tab[4] == 0x00);
}

/* Il PIC risponde in lettura sulla porta dati con la maschera corrente:
   un bit a 1 significa linea disabilitata. */
static void check_pic(void)
{
    /* Non si confronta il byte intero: i driver smascherano legittimamente la
       propria linea, e da M4 il timer accende il bit 0. Un check che pretenda
       0xFB diventerebbe rosso a ogni driver aggiunto — e la tentazione
       sarebbe aggiornare la costante invece di chiedersi cosa si sta
       verificando. Qui restano solo le due proprieta' che devono valere
       sempre. */
    report("la cascata sul master resta smascherata",
           (inb(PIC_MASTER_DATA) & 0x04) == 0);

    /* Nessun dispositivo sullo slave, per ora: se un bit si accendesse
       vorrebbe dire che qualcuno ha smascherato una linea che nessuno serve. */
    report("lo slave ha tutto mascherato",
           inb(PIC_SLAVE_DATA) == 0xFF);
}

/* L'unico modo di provare la catena completa senza hardware: un interrupt
   software. Il breakpoint e' l'eccezione giusta perche' e' deliberata e
   riprende dall'istruzione successiva. */
static volatile int bp_calls;
static volatile uint32_t bp_vec, bp_cs, bp_err;

static void bp_handler(struct regs *r)
{
    bp_calls++;
    bp_vec = r->vec;
    bp_cs  = r->cs;
    bp_err = r->err;
}

static void check_dispatch(void)
{
    exception_register(EXC_BREAKPOINT, bp_handler);

    bp_calls = 0;
    __asm__ volatile ("int $3");

    /* Se siamo arrivati qui, iret ha funzionato: la CPU ha ripreso
       l'esecuzione dopo l'int invece di ripartire. */
    report("int $3 viene gestito e l'esecuzione riprende", bp_calls == 1);
    report("il dispatcher riceve il vettore giusto", bp_vec == EXC_BREAKPOINT);
    report("struct regs riporta cs corretto", bp_cs == GDT_SEL_CODE);
    report("il codice d'errore fittizio e' zero", bp_err == 0);

    exception_register(EXC_BREAKPOINT, 0);
}

/* --- M4: il timer ---------------------------------------------------------
   Qui non si verifica una tabella ma un comportamento nel tempo, e serve un
   riferimento esterno: il timer non puo' misurare se stesso. */

static void check_timer(void)
{
    uint32_t t0, t1, misurati;

    /* Il driver deve aver smascherato la propria linea, altrimenti il chip
       genera interrupt che il PIC non presenta alla CPU. */
    report("timer_init smaschera l'IRQ 0",
           (inb(PIC_MASTER_DATA) & 0x01) == 0);

    /* Che i tick avanzino dice che l'interrupt arriva e che l'EOI e' corretto:
       con un EOI mancante il contatore si fermerebbe a 1. */
    t0 = timer_ticks();
    if (!rtc_wait_second_change()) {
        report("l'RTC risponde (serve come riferimento)", 0);
        return;
    }
    report("i tick avanzano dopo la sti", timer_ticks() > t0);

    /* La misura vera. Partiti da un confine di secondo appena attraversato,
       si contano i tick fino al confine successivo. */
    t0 = timer_ticks();
    if (!rtc_wait_second_change()) {
        report("l'RTC risponde per la seconda misura", 0);
        return;
    }
    t1 = timer_ticks();
    misurati = t1 - t0;

    /* 100 Hz nominali. La tolleranza copre il troncamento del divisore
       (100.007 Hz reali) e il fatto che i due confini di secondo non cadono
       esattamente dove li campioniamo. */
    report("la frequenza misurata e' 100 Hz entro la tolleranza",
           misurati >= 95 && misurati <= 105);

    kprintf("selftest:      (tick contati in un secondo: %d)\n",
            (int)misurati);

    /* Un contatore che va all'indietro significherebbe letture non atomiche o
       un gestore che lo azzera. */
    t0 = timer_ticks();
    t1 = timer_ticks();
    report("il contatore non torna indietro", t1 >= t0);
}

/* --- M5: la tastiera --------------------------------------------------------
   La decodifica e il buffer sono coperti dai test host, che sono istantanei.
   Qui restano le due cose che esistono solo dentro la VM. */

static void check_keyboard(void)
{
    report("keyboard_init smaschera l'IRQ 1",
           (inb(PIC_MASTER_DATA) & 0x02) == 0);

    /* Nessuno ha ancora digitato: il buffer deve essere vuoto. Se restituisse
       un carattere, il gestore avrebbe accodato spazzatura all'avvio. */
    report("nessun carattere in attesa all'avvio",
           keyboard_getchar() == -1);
}

int selftest_run(void)
{
    failures = 0;

    check_gdt();
    check_idt();
    check_pic();
    check_dispatch();
    check_timer();
    check_keyboard();
    check_putc();
    check_clear();
    check_newline();
    check_scroll();
    check_cursor();
    check_color();

    vga_clear();
    return failures;
}
