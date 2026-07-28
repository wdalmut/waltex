#ifndef WALTEX_IDT_H
#define WALTEX_IDT_H

#include "types.h"

/* Lo stato della CPU al momento dell'interruzione, nell'ordine esatto in cui
   gli stub di isr.S lo impilano. Il primo campo sta all'indirizzo piu' basso,
   cioe' e' l'ultimo a essere stato impilato.

   ATTENZIONE: cambiare l'ordine dei campi qui senza cambiarlo in isr.S non
   produce nessun errore di compilazione. Produce un dump plausibile e
   sbagliato, che e' il modo peggiore di rompersi. */
struct regs {
    uint32_t ds;                                       /* salvato dallo stub  */
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;   /* pusha               */
    uint32_t vec, err;                                 /* impilati dallo stub */
    uint32_t eip, cs, eflags;                          /* impilati dalla CPU  */
};

#define IDT_ENTRIES 256

/* Primo vettore usato dagli IRQ hardware dopo il rimappaggio del PIC.
   Sotto il 32 ci sono le eccezioni della CPU. */
#define IRQ_BASE 32

/* I vettori che incontrerai piu' spesso. L'elenco completo e' nel manuale
   Intel, volume 3A, capitolo 6. */
#define EXC_DIVIDE_ERROR        0
#define EXC_DEBUG               1
#define EXC_NMI                 2
#define EXC_BREAKPOINT          3
#define EXC_OVERFLOW            4
#define EXC_INVALID_OPCODE      6
#define EXC_DOUBLE_FAULT        8
#define EXC_INVALID_TSS        10
#define EXC_SEGMENT_NOT_PRESENT 11
#define EXC_STACK_SEGMENT      12
#define EXC_GENERAL_PROTECTION 13
#define EXC_PAGE_FAULT         14

/* Riempie i 256 gate e carica la tabella con lidt. */
void idt_init(void);

/* Chiamata dagli stub di isr.S con il puntatore allo stato salvato.
   La definisci in idt.c: e' il punto in cui si decide se un vettore e'
   un'eccezione, un IRQ o un int software. */
void isr_handler(struct regs *r);

/* Registra il gestore di una linea IRQ (0-15). Grazie a questo, timer.c in M4
   e keyboard.c in M5 non sapranno nulla dell'IDT. */
void irq_register(uint8_t irq, void (*handler)(struct regs *));

/* Registra il gestore di un'eccezione (0-31). Passare 0 come handler
   ripristina il comportamento predefinito, che e' il panic: un'eccezione che
   nessuno si aspetta e' un bug, non un evento da gestire.

   Serve per le poche eccezioni che invece sono deliberate — il breakpoint
   int $3 e' l'esempio — e per i test. */
void exception_register(uint8_t vec, void (*handler)(struct regs *));

/* Nome leggibile di un'eccezione, per il dump di panic. */
const char *exception_name(uint32_t vec);

#endif
