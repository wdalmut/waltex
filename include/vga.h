#ifndef WALTEX_VGA_H
#define WALTEX_VGA_H

#include "types.h"

/* La palette a 16 colori del text mode, fissa nel firmware dal 1981.
   I valori 8-15 sono i corrispettivi 0-7 con il bit di intensita' acceso. */
enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE,
    VGA_GREEN,
    VGA_CYAN,
    VGA_RED,
    VGA_MAGENTA,
    VGA_BROWN,
    VGA_LIGHT_GREY,
    VGA_DARK_GREY,
    VGA_LIGHT_BLUE,
    VGA_LIGHT_GREEN,
    VGA_LIGHT_CYAN,
    VGA_LIGHT_RED,
    VGA_LIGHT_MAGENTA,
    VGA_YELLOW,
    VGA_WHITE
};

/* Text mode 80x25. vga_init lascia lo schermo pulito, il cursore a (0,0) e il
   colore corrente su grigio chiaro su nero. */
void vga_init(void);

/* Riempie lo schermo di spazi con il colore corrente e riporta il cursore a
   (0,0). Non cambia il colore corrente. */
void vga_clear(void);

/* Scrive un carattere alla posizione corrente, con il colore corrente, e
   avanza il cursore. '\n' va a inizio riga successiva. Raggiunto il fondo, il
   contenuto scorre di una riga verso l'alto e l'ultima riga resta vuota, con
   il colore corrente. */
void vga_putc(char c);

/* Cambia il colore usato dalle scritture successive. Non ridipinge quanto
   gia' presente a schermo.

   Lo sfondo ha solo 8 colori utilizzabili: il quarto bit dell'attributo alto
   e' il bit di lampeggio, non l'intensita'. Un valore di bg fuori da 0-7 va
   mascherato, non propagato: accendere il lampeggio per sbaglio produce uno
   schermo che pulsa e nessun indizio sul perche'. */
void vga_set_color(uint8_t fg, uint8_t bg);

#endif
