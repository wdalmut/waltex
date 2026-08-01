#ifndef WALTEX_SHELL_H
#define WALTEX_SHELL_H

#include "types.h"

/* Quante parole al massimo in una riga di comando, nome compreso. */
#define SHELL_MAX_ARGS 8

/* Il prompt e' una costante di questo header e non una stringa sparsa in
   shell.c, perche' tests/shell.sh la cerca: se cambiasse in un solo posto il
   test mentirebbe. */
#define SHELL_PROMPT "waltex> "

struct shell_cmd {
    const char *name;
    void (*fn)(int argc, char **argv);
    const char *help;
};

/* Spezza line in parole, scrivendo dei NUL dentro line, e riempie argv con
   puntatori dentro line stessa. Ritorna quante parole ha registrato, al
   massimo max.

   Non copia niente: e' il modo Unix di farlo, e ha una conseguenza da tenere a
   mente — dopo lo split la riga non esiste piu' come stringa unica. Un comando
   che volesse il resto della riga deve prenderselo da argv.

   Pura: nessuno stato, nessun hardware, nessuna allocazione. Testabile
   sull'host. */
int shell_split(char *line, char **argv, int max);

/* Legge un numero esadecimale, con "0x" o "0X" facoltativo. La base e' SEMPRE
   16: "1000" vale 0x1000, non mille.

   Ritorna 1 e scrive *out se la stringa e' un numero valido, 0 altrimenti
   lasciando *out intatto.

   Convenzione 1/0 come ring_push, non -EINVAL: errno.h arriva in M14 e
   anticiparlo violerebbe la disciplina delle milestone. */
int shell_parse_hex(const char *s, uint32_t *out);

/* Come shell_parse_hex, ma in base 10, e con lo stesso contratto: 1 e *out
   scritto se ci riesce, 0 e *out INTATTO se no.

   Esiste da M10 perche' rdsect e wrsect prendono un numero di SETTORE, e i
   numeri di settore non si scrivono in esadecimale: con parse_hex, "rdsect 10"
   leggerebbe il settore 16. In M11 i numeri di blocco arriveranno dal
   superblocco minix, e li si legge in decimale. */
int shell_parse_dec(const char *s, uint32_t *out);

/* Esegue una riga: split, ricerca nella tabella, chiamata. Riga vuota: niente.
   Comando sconosciuto: lo dice, perche' un messaggio muto costa una
   ricompilazione per capire se hai sbagliato a digitare. */
void shell_exec(char *line);

/* Collega il sink di eco all'editor di riga. */
void shell_init(void);

/* Il task del prompt. Non ritorna.

   Nessun hlt nel ciclo di attesa: funzionerebbe oggi, ma hlt e' privilegiata e
   in M14 questo stesso ciclo finisce in ring 3, dove prende un #GP. Scriverlo
   adesso senza hlt significa non riscriverlo allora.

   Lo spin su keyboard_getchar e' deliberato: manca il blocking I/O, sta nello
   spec sotto "fuori scope", e il punto di decisione e' M9. */
void shell_task(void);

#endif
