#ifndef WALTEX_SELFTEST_H
#define WALTEX_SELFTEST_H

/* Esegue i self-check e riporta ogni esito su seriale nella forma
   "selftest: ok   -- <nome>" oppure "selftest: FAIL -- <nome>".
   Ritorna il numero di check falliti. */
int selftest_run(void);

#endif
