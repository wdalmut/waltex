#include "vga.h"
#include "io.h"
#include "memory.h"
#include "irq.h"

#define VGA_MEM ((volatile uint16_t *)0xB8000)

#define VGA_CURSOR_INDEX (uint16_t)0x3D4
#define VGA_CURSOR_DATA (uint16_t)0x3D5

#define VGA_COLS  80
#define VGA_ROWS  25

static uint8_t color = 0x07;
static uint16_t cursor = 0x00;

static void set_cursor(uint16_t cursor) {
    outb(VGA_CURSOR_INDEX, 0x0F);
    outb(VGA_CURSOR_DATA, (uint8_t)cursor);
    outb(VGA_CURSOR_INDEX, 0x0E);
    outb(VGA_CURSOR_DATA, (uint8_t)(cursor >> 8));
}

void vga_init(void) {
    vga_clear();
}

void vga_clear(void) {
    cursor = 0x00;

    memset16((uint16_t *)VGA_MEM, (color << 8) | 0x0000 | ' ' , VGA_COLS*VGA_ROWS);

    set_cursor(cursor);
}

void vga_putc(char c) {
    uint32_t flags;
    uint16_t pos;

    /* Da M6b questa funzione ha bisogno di una sezione critica, ed e' la prima
       del progetto.

       cursor e color sono static di questo file, nati quando esisteva un solo
       flusso di esecuzione. Ora il timer puo' interrompere fra la scrittura
       nella cella e l'incremento della posizione: un altro task leggerebbe lo
       stesso cursor, scriverebbe nella stessa cella, e al ritorno il primo
       incrementerebbe un valore gia' cambiato. Un carattere perso.

       E' lo stesso problema del count++ nel ring buffer. La' si e' risolto con
       la struttura — un solo scrittore per indice — qui non si puo': tutti i
       task scrivono a schermo, e la posizione e' condivisa per definizione.

       irq_save e non cli: questa funzione viene chiamata anche da panic_regs,
       dove gli interrupt sono gia' spenti deliberatamente, e un sti finale
       farebbe interrompere il dump a metà. */
    flags = irq_save();

    if (c >= 32) {
        VGA_MEM[cursor] = (color << 8) | c;

        cursor += 1;
    } else if (c == '\n') {
        uint16_t remaining_cols = VGA_COLS - (cursor % VGA_COLS);
        cursor += remaining_cols;
    }

    if (cursor >= VGA_COLS*VGA_ROWS) {
        memcpy((void *)VGA_MEM, (void *)(VGA_MEM+VGA_COLS), (VGA_ROWS-1)*VGA_COLS*sizeof(uint16_t));
        memset16((uint16_t *)(VGA_MEM+((VGA_ROWS-1)*VGA_COLS)), (color << 8) | 0x0000 | ' ', VGA_COLS);
        cursor = ((VGA_ROWS-1)*VGA_COLS);
    }

    pos = cursor;

    irq_restore(flags);

    /* set_cursor sta FUORI dalla sezione critica, e la ragione e' quantitativa.
       Sono quattro outb, e sotto QEMU un accesso a porta costa molto piu' di un
       accesso in memoria: tenendole dentro, con i task che stampano
       continuamente, gli interrupt resterebbero spenti per una frazione
       significativa del tempo e il timer perderebbe tick — cosa che il
       self-check di M4 sulla frequenza noterebbe.

       Il prezzo e' che due task che si accavallano possono lasciare il trattino
       lampeggiante indietro di un carattere. Si corregge al successivo. */
    set_cursor(pos);
}

void vga_set_color(uint8_t fg, uint8_t bg)
{
    color = (fg & 0x0F) | ((bg & 0x07) << 4);
}