#ifndef WALTEX_TIMER_H
#define WALTEX_TIMER_H

#include "types.h"

/* Il PIT 8253/8254 conta all'indietro a una frequenza fissa. Il numero viene
   dal cristallo dei primi PC: 14.31818 MHz diviso 12. Il 14.31818 e' quattro
   volte la sottoportante colore NTSC, perche' nel 1981 conveniva usare un
   cristallo prodotto in massa per i televisori. */
#define PIT_BASE_FREQ 1193182u

#define PIT_CH0_DATA 0x40
#define PIT_CH1_DATA 0x41       /* rinfresco DRAM, obsoleto */
#define PIT_CH2_DATA 0x42       /* altoparlante del PC */
#define PIT_CMD      0x43

/* 0x36 = 00 11 011 0
     bit 7-6  canale 0
     bit 5-4  accesso: prima il byte basso, poi l'alto
     bit 3-1  mode 3, onda quadra
     bit 0    formato binario                                            */
#define PIT_MODE3_CH0 0x36

/* Il divisore e' a 16 bit, quindi la frequenza ottenibile e' limitata:
   da 1193182/65535 = 18.21 Hz fino a 1193182 Hz. */
#define PIT_DIVISOR_MAX 65535u
#define PIT_HZ_MIN 19u

/* Divisore da caricare per ottenere hz interrupt al secondo.
   Funzione pura, nessun effetto sull'hardware: e' testabile sull'host, ed e'
   il punto dove si sbaglia l'aritmetica.

   Contratto sugli estremi: hz fuori dall'intervallo utile viene limitato, non
   propagato. Un divisore che non ci sta in 16 bit, troncato, produrrebbe una
   frequenza che non c'entra niente con quella richiesta — e un divisore 0 il
   PIT lo interpreta come 65536, cioe' la frequenza piu' bassa possibile
   invece della piu' alta. */
uint16_t pit_divisor(uint32_t hz);

/* Programma il canale 0, registra il gestore dell'IRQ 0, smaschera la linea. */
void timer_init(uint32_t hz);

/* Quanti tick dall'avvio. */
uint32_t timer_ticks(void);

#endif
