#include "idt.h"
#include "gdt.h"
#include "pic.h"
#include "panic.h"
#include "types.h"
#include "kprintf.h"

/* Un gate dell'IDT: otto byte come un descrittore della GDT, ma con campi
   diversi. L'offset del gestore e' spezzato in due pezzi non contigui, con il
   selettore e gli attributi in mezzo — stessa logica di retrocompatibilita'
   che ha spezzato la base nella GDT.

     byte 0-1   offset del gestore, bit 0-15
     byte 2-3   selettore di segmento: il nostro codice
     byte 4     riservato, deve valere zero
     byte 5     tipo e attributi
     byte 6-7   offset del gestore, bit 16-31                              */
struct idt_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attr;
    uint16_t offset_high;
} __attribute__((packed));

/* Il puntatore che lidt legge, identico per forma a quello della GDT.
   Qui packed serve davvero: senza, il compilatore allineerebbe il uint32_t a
   4 byte inserendo due byte di padding, e lidt leggerebbe la base dal posto
   sbagliato. */
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* 0x8E = 1000 1110
     bit 7    P = 1      presente
     bit 6-5  DPL = 00   invocabile solo da ring 0
     bit 4    S = 0      descrittore di SISTEMA, non codice/dati
     bit 3-0  tipo 1110  interrupt gate a 32 bit

   Un trap gate sarebbe 0x8F: l'unica differenza e' che non azzera il flag di
   interrupt entrando. Noi usiamo solo interrupt gate, cosi' un gestore non
   viene interrotto da un altro interrupt mentre lavora. */
#define GATE_INTERRUPT32 0x8E

static struct idt_gate idt[IDT_ENTRIES];
static struct idt_ptr  idtr;

/* Le due tabelle di smistamento. Un puntatore nullo significa "nessuno se ne
   occupa", e la conseguenza e' opposta nei due casi: per un'eccezione e' un
   bug e si va in panic, per un IRQ e' normale e si ignora. */
static void (*exc_handlers[32])(struct regs *);
static void (*irq_handlers[16])(struct regs *);

/* I 48 punti d'ingresso definiti in isr.S. */
extern void isr0(void),  isr1(void),  isr2(void),  isr3(void);
extern void isr4(void),  isr5(void),  isr6(void),  isr7(void);
extern void isr8(void),  isr9(void),  isr10(void), isr11(void);
extern void isr12(void), isr13(void), isr14(void), isr15(void);
extern void isr16(void), isr17(void), isr18(void), isr19(void);
extern void isr20(void), isr21(void), isr22(void), isr23(void);
extern void isr24(void), isr25(void), isr26(void), isr27(void);
extern void isr28(void), isr29(void), isr30(void), isr31(void);

extern void irq0(void),  irq1(void),  irq2(void),  irq3(void);
extern void irq4(void),  irq5(void),  irq6(void),  irq7(void);
extern void irq8(void),  irq9(void),  irq10(void), irq11(void);
extern void irq12(void), irq13(void), irq14(void), irq15(void);

/* L'ordine e' l'ordine dei vettori: 0-31 le eccezioni, 32-47 gli IRQ
   rimappati. */
static void (*const stub[IRQ_BASE + 16])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
    irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15
};

static void idt_set_gate(int vec, void (*handler)(void), uint8_t type_attr)
{
    uint32_t off = (uint32_t)handler;

    idt[vec].offset_low  = off & 0xFFFF;
    idt[vec].selector    = GDT_SEL_CODE;
    idt[vec].zero        = 0;
    idt[vec].type_attr   = type_attr;
    idt[vec].offset_high = (off >> 16) & 0xFFFF;
}

void idt_init(void)
{
    int i;

    /* Prima tutti non presenti: type_attr a zero azzera anche il bit P.
       Un vettore che scattasse senza gestore produce un #NP, che a sua volta
       un gestore ce l'ha e finisce in panic con un messaggio — invece che in
       una tripla fault muta. */
    for (i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(i, 0, 0);

    for (i = 0; i < (int)(sizeof(stub) / sizeof(stub[0])); i++)
        idt_set_gate(i, stub[i], GATE_INTERRUPT32);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint32_t)&idt;

    /* Nessun far jump da fare, a differenza della GDT: i registri di segmento
       non tengono nessuna copia nascosta dell'IDT, la CPU la consulta al
       volo a ogni interruzione. */
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

void exception_register(uint8_t vec, void (*handler)(struct regs *))
{
    assert(vec < 32);
    exc_handlers[vec] = handler;
}

void irq_register(uint8_t irq, void (*handler)(struct regs *))
{
    assert(irq < 16);
    irq_handlers[irq] = handler;
}

/* Il punto in cui tutti i 48 stub convergono. Tre categorie di vettore, tre
   politiche diverse. */
void isr_handler(struct regs *r)
{
    void (*handler)(struct regs *);

    /* 0-31: eccezione della CPU. */
    if (r->vec < 32) {
        handler = exc_handlers[r->vec];
        if (handler == 0)
            panic_regs(r);      /* nessuno se l'aspettava: e' un bug */

        handler(r);

        /* Attenzione a cosa succede tornando da qui. Le eccezioni si dividono
           in trap e fault: da una trap (breakpoint, overflow) iret riprende
           dall'istruzione SUCCESSIVA, da una fault (divisione per zero, page
           fault) riprende dalla STESSA, che rifara' fault. Un gestore di
           fault deve rimuoverne la causa, altrimenti si cicla per sempre. */
        return;
    }

    /* 32-47: IRQ hardware. */
    if (r->vec >= IRQ_BASE && r->vec < IRQ_BASE + 16) {
        uint8_t irq = (uint8_t)(r->vec - IRQ_BASE);

        handler = irq_handlers[irq];
        if (handler != 0)
            handler(r);

        /* L'EOI si manda comunque, anche senza gestore. Saltarlo perche'
           nessuno era interessato lascia il PIC convinto che l'interrupt sia
           ancora in servizio, e quella linea non ne presenta mai piu' uno. */
        pic_eoi(irq);
        return;
    }

    /* Tutto il resto: un int software su un vettore che non abbiamo
       assegnato. Non e' un guasto della macchina, e' codice nostro che ha
       sbagliato numero. */
    kprintf("waltex: interrupt software non gestito, vettore %d\n",
            (int)r->vec);
}
