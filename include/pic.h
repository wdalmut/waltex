#ifndef WALTEX_PIC_H
#define WALTEX_PIC_H

#include "types.h"

/* Due 8259 in cascata: il master serve gli IRQ 0-7, lo slave gli 8-15 ed e'
   collegato alla linea 2 del master. Ecco perche' l'IRQ2 non esiste come
   sorgente e resta sempre smascherato. */
#define PIC_MASTER_CMD  0x20
#define PIC_MASTER_DATA 0x21
#define PIC_SLAVE_CMD   0xA0
#define PIC_SLAVE_DATA  0xA1

/* Rimappa gli IRQ sui vettori 32-47 e maschera tutte le linee tranne la
   cascata. Le singole linee si accendono quando arriva il driver che le usa:
   il timer in M4, la tastiera in M5. */
void pic_init(void);

/* End Of Interrupt: dice al chip che l'interrupt e' stato servito e che puo'
   presentarne un altro. Per gli IRQ 8-15 va mandato a entrambi i chip. */
void pic_eoi(uint8_t irq);

/* masked != 0 disabilita la linea, 0 la abilita. */
void pic_mask(uint8_t irq, int masked);

#endif
