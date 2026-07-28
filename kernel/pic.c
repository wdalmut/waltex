#include "types.h"
#include "pic.h"
#include "io.h"

void pic_init(void)
{
    outb(PIC_MASTER_CMD, 0x11);
    outb(PIC_SLAVE_CMD, 0x11);
    outb(PIC_MASTER_DATA, 0x20);
    outb(PIC_SLAVE_DATA, 0x28);
    outb(PIC_MASTER_DATA, 0x04);
    outb(PIC_SLAVE_DATA, 0x02);
    outb(PIC_MASTER_DATA, 0x01);
    outb(PIC_SLAVE_DATA, 0x01);
    outb(PIC_MASTER_DATA, 0xFB);
    outb(PIC_SLAVE_DATA, 0xFF);
}

#define PIC_EOI 0x20

void pic_eoi(uint8_t irq)
{
    /* Lo slave non e' collegato alla CPU: parla al master attraverso la linea
       2. Per un IRQ dello slave servono quindi DUE end-of-interrupt, e in
       quest'ordine. Mandarlo al solo slave lascia il master convinto che la
       cascata sia ancora in servizio, e da quel momento blocca tutti gli
       interrupt, non solo quelli dello slave. */
    if (irq >= 8)
        outb(PIC_SLAVE_CMD, PIC_EOI);

    outb(PIC_MASTER_CMD, PIC_EOI);
}

void pic_mask(uint8_t irq, int masked)
{
    uint16_t porta;
    uint8_t  bit;
    uint8_t  maschera;

    /* Ogni chip ha il proprio registro di maschera da 8 bit, quindi l'IRQ 8
       e' il bit 0 dello slave, non il bit 8 di qualcosa. */
    if (irq < 8) {
        porta = PIC_MASTER_DATA;
        bit   = irq;
    } else {
        porta = PIC_SLAVE_DATA;
        bit   = irq - 8;
    }

    /* Leggere prima e' obbligatorio: quel registro contiene lo stato di tutte
       e otto le linee. Scrivendo un byte costruito da zero si azzererebbero
       le altre sette. */
    maschera = inb(porta);

    /* Nel PIC un bit a 1 significa linea DISABILITATA: l'intuizione va
       rovesciata rispetto al nome del parametro. */
    if (masked)
        maschera |= (uint8_t)(1u << bit);
    else
        maschera &= (uint8_t)~(1u << bit);

    outb(porta, maschera);
}