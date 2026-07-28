#include "rtc.h"
#include "io.h"

/* Quanti tentativi prima di dichiarare l'RTC non rispondente. Il valore e'
   tarato su una misura reale: sotto QEMU un secondo di attesa costa circa
   2.0 milioni di poll, quindi 8 milioni danno un margine di 4x prima di
   dichiarare il chip non rispondente. */
#define RTC_MAX_POLLS 8000000u

static uint8_t cmos_read(uint8_t reg)
{
    /* Il bit 7 della porta indice controlla il mascheramento degli NMI.
       Scrivendo l'indice con quel bit a zero li lasciamo abilitati, che e' lo
       stato in cui ci troviamo comunque. */
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void)
{
    return (cmos_read(RTC_REG_STATUS_A) & RTC_STATUS_A_UIP) != 0;
}

uint8_t rtc_seconds_raw(void)
{
    /* Leggere durante un aggiornamento puo' restituire un valore incoerente,
       e a noi basterebbe per credere che il secondo sia cambiato. */
    while (update_in_progress())
        ;

    return cmos_read(RTC_REG_SECONDS);
}

int rtc_wait_second_change(void)
{
    uint8_t start = rtc_seconds_raw();
    uint32_t polls;

    for (polls = 0; polls < RTC_MAX_POLLS; polls++)
        if (rtc_seconds_raw() != start)
            return 1;

    return 0;
}
