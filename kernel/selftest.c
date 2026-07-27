#include "selftest.h"
#include "types.h"
#include "io.h"
#include "serial.h"
#include "vga.h"

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

int selftest_run(void)
{
    failures = 0;

    check_putc();
    check_clear();
    check_newline();
    check_scroll();
    check_cursor();
    check_color();

    vga_clear();
    return failures;
}
