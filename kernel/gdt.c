#include "types.h"
# include "gdt.h"

struct gdt_entry {
    uint16_t limit_low;         /* limite 0-15                        */
    uint16_t base_low;          /* base   0-15                        */
    uint8_t  base_mid;          /* base  16-23                        */
    uint8_t  access;
    uint8_t  limit_high_flags;  /* nibble basso: limite 16-19; alto: flag */
    uint8_t  base_high;         /* base  24-31                        */
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[3];
static struct gdt_ptr   gdtr;

static void gdt_set(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    gdt[index].limit_low = limit & 0x0000FFFF;
    gdt[index].base_low = base & 0x0000FFFF;
    gdt[index].base_mid = (base & 0x00FF0000) >> 16;
    gdt[index].base_high = (base & 0xFF000000) >> 24;
    gdt[index].access = access;
    gdt[index].limit_high_flags = ((flags & 0x0F) << 4) | ((limit & 0x000F0000) >> 16);
}

void gdt_init(void)
{
    gdt_set(0, 0, 0x00000, 0x00, 0x0);   /* null   */
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xC);   /* codice */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xC);   /* dati   */

    gdtr.limit = sizeof(gdt) - 1;        /* 23, non 24 */
    gdtr.base  = (uint32_t)&gdt;

    gdt_flush(&gdtr);
}