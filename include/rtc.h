#ifndef WALTEX_RTC_H
#define WALTEX_RTC_H

#include "types.h"

/* Orologio in tempo reale del CMOS. Nel progetto serve a una cosa sola: dare
   ai test un riferimento temporale INDIPENDENTE dal timer che stanno
   misurando. Senza, potremmo verificare che i tick avanzano ma non a quale
   velocita' — e un timer programmato al doppio della frequenza voluta
   passerebbe il test. */
#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define RTC_REG_SECONDS  0x00
#define RTC_REG_STATUS_A 0x0A
#define RTC_STATUS_A_UIP 0x80    /* aggiornamento in corso */

/* Il byte grezzo del registro dei secondi. Non lo interpretiamo: a noi
   interessa solo se cambia, e cosi' non ci riguarda se il chip sia
   configurato in BCD o in binario. */
uint8_t rtc_seconds_raw(void);

/* Attende che il secondo cambi. Ritorna 1 se e' cambiato, 0 se e' scaduto il
   limite di tentativi: un RTC che non risponde deve far fallire un check, non
   appendere il kernel per sempre. */
int rtc_wait_second_change(void);

#endif
