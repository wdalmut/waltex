/* Test dell'editor di riga, col gcc dell'host.

   E' logica pura: un buffer, una lunghezza e quattro casi. Non tocca hardware,
   quindi non serve QEMU e il ciclo di prova e' di millisecondi.

   La parte che rende questo file possibile e' il sink di eco. lineedit non
   stampa da se': riceve una funzione a cui consegnare un carattere alla volta,
   esattamente come kvprintf. Qui gli passiamo una funzione che accoda in un
   buffer, e cosi' possiamo verificare COSA e' stato echeggiato byte per byte —
   compresa la sequenza di tre caratteri con cui si cancella, che altrimenti si
   potrebbe solo guardare a schermo e sperare. */

#define WALTEX_HOSTED 1

#include <stdio.h>

#include "types.h"
#include "lineedit.h"

static int failures;

static void check(const char *name, int ok)
{
    printf(ok ? "ok   -- %s\n" : "FAIL -- %s\n", name);
    if (!ok)
        failures++;
}

/* ---- il sink di eco ------------------------------------------------------ */

#define ECHO_MAX 1024

static char echoed[ECHO_MAX];
static int  echoed_len;

static void echo_sink(char c)
{
    if (echoed_len < ECHO_MAX - 1)
        echoed[echoed_len++] = c;
    echoed[echoed_len] = '\0';
}

static void echo_clear(void)
{
    echoed_len = 0;
    echoed[0] = '\0';
}

/* Confronto fatto in casa invece di strcmp, e ATTENZIONE: confronta anche la
   lunghezza. same(buf, len, "abc") e' falso sia se il contenuto differisce sia
   se len non e' 3, quindi i controlli che la usano si chiamano "e' esattamente"
   e non "contiene" — un nome che promette meno di quello che verifica manda a
   cercare il guasto nel posto sbagliato.

   Il confronto sulla lunghezza c'e' per la lezione di M1, dove un expect() che
   guardava solo le stringhe lasciava passare un NUL di troppo seguito da byte
   in eccesso. */
static int same(const char *a, int alen, const char *b)
{
    int i;

    for (i = 0; b[i] != '\0'; i++) {
        if (i >= alen || a[i] != b[i])
            return 0;
    }

    return i == alen;
}

static int echo_is(const char *want)
{
    return same(echoed, echoed_len, want);
}

/* Infila una stringa carattere per carattere. Ritorna l'ultimo valore
   restituito da lineedit_putc. */
static int feed(struct lineedit *le, const char *s)
{
    int r = 0;

    while (*s)
        r = lineedit_putc(le, *s++);

    return r;
}

/* ---- i controlli --------------------------------------------------------- */

static void test_riga_normale(void)
{
    struct lineedit le;

    lineedit_init(&le, echo_sink);
    echo_clear();

    check("un carattere su riga nuova ritorna 0",
          lineedit_putc(&le, 'a') == 0);

    lineedit_putc(&le, 'b');
    lineedit_putc(&le, 'c');

    check("Invio chiude la riga e ritorna 1",
          lineedit_putc(&le, '\n') == 1);

    check("la riga completa e' esattamente \"abc\", len compreso",
          same(le.buf, le.len, "abc"));

    check("len e' 3", le.len == 3);

    /* L'eco comprende il ritorno a capo: e' cio' che manda il cursore a inizio
       riga prima che il comando stampi la sua risposta. */
    check("l'eco ricevuto e' esattamente \"abc\\n\"", echo_is("abc\n"));

    check("la riga completa e' terminata da NUL", le.buf[le.len] == '\0');
}

static void test_riga_vuota(void)
{
    struct lineedit le;

    lineedit_init(&le, echo_sink);
    echo_clear();

    /* Premere Invio senza scrivere niente e' legittimo: produce un prompt
       nuovo. Chi tratta la riga vuota come un errore fa una shell irritante. */
    check("Invio su riga vuota chiude comunque la riga",
          lineedit_putc(&le, '\n') == 1 && le.len == 0 && le.buf[0] == '\0');
}

static void test_backspace(void)
{
    struct lineedit le;

    /* Il caso che si scopre solo guardando lo schermo: cancellare quando non
       c'e' niente da cancellare. Se l'eco parte comunque, il cursore arretra
       sopra il prompt e se lo mangia — dopo otto backspace il prompt non c'e'
       piu'. */
    lineedit_init(&le, echo_sink);
    echo_clear();

    check("backspace su riga vuota ritorna 0 e lascia len a 0",
          lineedit_putc(&le, '\b') == 0 && le.len == 0);

    check("backspace su riga vuota non produce NESSUN eco", echoed_len == 0);

    lineedit_init(&le, echo_sink);
    echo_clear();
    feed(&le, "ab");
    lineedit_putc(&le, '\b');

    check("backspace accorcia la riga di uno", le.len == 1);
    check("dopo il backspace la riga e' esattamente \"a\", len compreso",
          same(le.buf, le.len, "a"));

    /* Tre caratteri, non uno: indietro, sovrascrivi, indietro. Mandando solo
       '\b' il cursore torna ma il carattere resta visibile. */
    check("l'eco del backspace e' \"\\b \\b\"", echo_is("ab\b \b"));

    lineedit_putc(&le, '\b');
    check("due backspace consecutivi svuotano la riga", le.len == 0);

    check("un terzo backspace non va sotto zero",
          lineedit_putc(&le, '\b') == 0 && le.len == 0);

    /* La riga chiusa DOPO un backspace deve essere una stringa C valida, non
       solo avere il len giusto.
       Il resto dei controlli confronta al massimo le.len byte, quindi un
       carattere cancellato che resta oltre il terminatore non si vedrebbe. Ma
       shell_exec riceve le->buf come stringa C e legge fino al primo NUL: se il
       terminatore non c'e', digitare "lsx", cancellare la 'x' e premere Invio
       esegue "lsx". */
    lineedit_init(&le, echo_sink);
    feed(&le, "lsx");
    lineedit_putc(&le, '\b');
    lineedit_putc(&le, '\n');

    check("la riga chiusa dopo un backspace e' una stringa C valida",
          le.len == 2 && le.buf[le.len] == '\0');
}

static void test_buffer_pieno(void)
{
    struct lineedit le;
    int i;

    /* La capacita' utile e' LINE_MAX - 1: serve un posto per il NUL finale. */
    lineedit_init(&le, echo_sink);
    echo_clear();

    for (i = 0; i < LINE_MAX - 1; i++)
        lineedit_putc(&le, 'x');

    check("entrano LINE_MAX - 1 caratteri", le.len == LINE_MAX - 1);
    check("tutti sono stati echeggiati", echoed_len == LINE_MAX - 1);

    echo_clear();
    check("il carattere di troppo e' scartato",
          lineedit_putc(&le, 'y') == 0 && le.len == LINE_MAX - 1);

    /* Niente eco per cio' che non e' entrato: chi digita deve vedere che non
       entra piu' nulla, invece di vedere un carattere che poi non c'e'. */
    check("il carattere scartato non produce eco", echoed_len == 0);

    check("a buffer pieno Invio chiude comunque la riga",
          lineedit_putc(&le, '\n') == 1);
    check("la riga piena e' terminata da NUL",
          le.buf[LINE_MAX - 1] == '\0');
}

static void test_caratteri_scartati(void)
{
    struct lineedit le;

    lineedit_init(&le, echo_sink);
    echo_clear();
    feed(&le, "ab");

    /* Il tab arriva davvero: keyboard.c ha [0x0F] = '\t'. Accodarlo renderebbe
       shell_split imprevedibile, perche' lo split separa sugli spazi. */
    lineedit_putc(&le, '\t');
    check("il tab e' scartato, len invariato", le.len == 2);
    check("il tab non produce eco", echo_is("ab"));

    /* ESC arriva anche lui: [0x01] = 27. Echeggiarlo scriverebbe una sequenza
       di escape a caso sul terminale. */
    lineedit_putc(&le, (char)27);
    check("ESC e' scartato", le.len == 2);
}

static void test_reset(void)
{
    struct lineedit le;

    lineedit_init(&le, echo_sink);
    feed(&le, "abc");
    lineedit_putc(&le, '\n');

    lineedit_reset(&le);
    check("lineedit_reset azzera len", le.len == 0);

    /* Il sink deve sopravvivere al reset: e' la ragione per cui reset esiste
       invece di richiamare lineedit_init. */
    echo_clear();
    lineedit_putc(&le, 'z');
    check("lineedit_reset conserva il sink", echo_is("z"));
}

static void test_sink_nullo(void)
{
    struct lineedit le;

    /* Un sink nullo e' lecito e serve: la shell potrebbe volere una riga senza
       eco, e comunque un puntatore a funzione non controllato e' un salto a
       zero. */
    lineedit_init(&le, 0);

    check("con sink nullo il carattere entra comunque",
          lineedit_putc(&le, 'a') == 0 && le.len == 1);
    check("con sink nullo il backspace non salta a zero",
          lineedit_putc(&le, '\b') == 0 && le.len == 0);
    check("con sink nullo Invio chiude la riga",
          lineedit_putc(&le, '\n') == 1);
}

int main(void)
{
    test_riga_normale();
    test_riga_vuota();
    test_backspace();
    test_buffer_pieno();
    test_caratteri_scartati();
    test_reset();
    test_sink_nullo();

    if (failures == 0) {
        printf("tutti i test dell'editor di riga passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
