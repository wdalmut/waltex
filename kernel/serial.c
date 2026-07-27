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
    outb(REG_INT_ENABLE, 0x00);   /* nessun interrupt: scriviamo in polling  */
    outb(REG_LINE_CTRL,  0x80);   /* DLAB=1: i primi due registri diventano
                                     il divisore del baud rate              */
    outb(REG_DATA,       0x03);   /* divisore 3 -> 38400 baud, byte basso    */
    outb(REG_INT_ENABLE, 0x00);   /* byte alto del divisore                  */
    outb(REG_LINE_CTRL,  0x03);   /* DLAB=0, 8 bit, no parità, 1 stop bit    */
    outb(REG_FIFO_CTRL,  0xC7);   /* FIFO on, svuotate, soglia 14 byte       */
    outb(REG_MODEM_CTRL, 0x0B);   /* DTR e RTS attivi, OUT2 abilitato        */
}

void serial_putc(char c)
{
    while ((inb(REG_LINE_STATUS) & LSR_TX_EMPTY) == 0)
        ;
    outb(REG_DATA, (uint8_t)c);
}
