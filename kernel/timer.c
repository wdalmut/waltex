#include "timer.h"
#include "io.h"
#include "types.h"
#include "idt.h"
#include "pic.h"
#include "kprintf.h"

static volatile uint32_t ticks;

static void timer_handler(struct regs *r)
{
    (void)r;        /* il gestore del timer non guarda lo stato salvato */
    ticks++;
}

uint16_t pit_divisor(uint32_t hz)
{
    if (hz < PIT_HZ_MIN) {
        return PIT_DIVISOR_MAX;
    } else if (hz >= PIT_BASE_FREQ) {
        return 1;
    } else {
        return (uint16_t)(PIT_BASE_FREQ / hz);
    }
}

void timer_init(uint32_t hz)
{
    uint16_t divisor = pit_divisor(hz);

    outb(PIT_CMD, PIT_MODE3_CH0);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0x00FF));
    outb(PIT_CH0_DATA, (uint8_t)((divisor  & 0xFF00) >> 8));

    irq_register(0, timer_handler);
    pic_mask(0, 0);
}

uint32_t timer_ticks(void)
{
    return ticks;
}