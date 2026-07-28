#include "idt.h"

/* Solo dati: i nomi che l'Intel da' alle prime 32 eccezioni, per rendere
   leggibile il dump di panic. "Exception 13" e "General Protection" sono la
   stessa informazione, ma solo una delle due si riconosce a colpo d'occhio
   alle due di notte. */
static const char *const names[32] = {
    "Divide Error",                 /*  0 */
    "Debug",                        /*  1 */
    "NMI",                          /*  2 */
    "Breakpoint",                   /*  3 */
    "Overflow",                     /*  4 */
    "BOUND Range Exceeded",         /*  5 */
    "Invalid Opcode",               /*  6 */
    "Device Not Available",         /*  7 */
    "Double Fault",                 /*  8 */
    "Coprocessor Segment Overrun",  /*  9 */
    "Invalid TSS",                  /* 10 */
    "Segment Not Present",          /* 11 */
    "Stack-Segment Fault",          /* 12 */
    "General Protection",           /* 13 */
    "Page Fault",                   /* 14 */
    "riservata",                    /* 15 */
    "x87 Floating-Point",           /* 16 */
    "Alignment Check",              /* 17 */
    "Machine Check",                /* 18 */
    "SIMD Floating-Point",          /* 19 */
    "Virtualization",               /* 20 */
    "Control Protection",           /* 21 */
    "riservata",                    /* 22 */
    "riservata",                    /* 23 */
    "riservata",                    /* 24 */
    "riservata",                    /* 25 */
    "riservata",                    /* 26 */
    "riservata",                    /* 27 */
    "Hypervisor Injection",         /* 28 */
    "VMM Communication",            /* 29 */
    "Security",                     /* 30 */
    "riservata"                     /* 31 */
};

const char *exception_name(uint32_t vec)
{
    if (vec < 32)
        return names[vec];
    return "non e' un'eccezione";
}
