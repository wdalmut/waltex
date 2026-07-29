#include "timer.h"
#include "io.h"
#include "types.h"
#include "idt.h"
#include "pic.h"
#include "kprintf.h"
#include "task.h"

static volatile uint32_t ticks;

static void timer_handler(struct regs *r)
{
    (void)r;        /* il gestore del timer non guarda lo stato salvato */
    ticks++;

    /* La riga che rende il sistema preemptive: il quanto di tempo e' un tick,
       e allo scadere il controllo viene TOLTO al task corrente senza che lui
       abbia chiesto niente.

       Qui gli interrupt sono spenti dal gate, quindi si chiama schedule()
       direttamente e non task_yield, che li spegnerebbe una seconda volta.

       schedule() non ritorna subito: ritornera' quando qualcuno cedera' il
       controllo a questo task. E allora isr_handler completera' la gestione
       di QUESTO interrupt, sullo stesso stack su cui e' stato preso. */
    schedule();
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