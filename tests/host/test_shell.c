/* Test delle due funzioni pure di shell.c, col gcc dell'host: lo splitting
   della riga in parole e la lettura di un numero esadecimale.

   Tutto il resto di shell.c — la tabella dei comandi e il ciclo del prompt —
   parla con la tastiera, il timer e lo schermo, e si verifica solo dentro la VM
   con tests/shell.sh. Qui sotto ci sono gli stub inerti che servono a linkare,
   come test_timer.c fa con irq_register e pic_mask. */

#define WALTEX_HOSTED 1

#include <stdio.h>
#include <stdlib.h>

#include "types.h"
#include "shell.h"
#include "lineedit.h"
#include "task.h"
#include "idt.h"

/* ---- stub: ciò che shell.c chiama e che sull'host non esiste ------------- */

void kprintf(const char *fmt, ...)
{
    (void)fmt;
}

uint32_t timer_ticks(void)
{
    return 0;
}

int task_current(void)
{
    return 0;
}

struct task *task_slot(int i)
{
    (void)i;
    return 0;
}

void vga_clear(void)
{
}

void demo_tasks_start(void)
{
}

int keyboard_getchar(void)
{
    return -1;
}

/* panic e' noreturn: uno stub che ritorna farebbe emettere un warning a gcc, e
   il progetto compila senza warning. exit() e' noreturn e chiude il discorso. */
void panic(const char *fmt, ...)
{
    (void)fmt;
    printf("FAIL -- panic chiamata da codice sotto test\n");
    exit(1);
}

void panic_regs(struct regs *r)
{
    (void)r;
    printf("FAIL -- panic_regs chiamata da codice sotto test\n");
    exit(1);
}

/* ---- l'impianto dei test ------------------------------------------------- */

static int failures;

static void check(const char *name, int ok)
{
    printf(ok ? "ok   -- %s\n" : "FAIL -- %s\n", name);
    if (!ok)
        failures++;
}

static int same(const char *a, const char *b)
{
    int i;

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (a[i] != b[i])
            return 0;
    }

    return a[i] == b[i];
}

/* ---- shell_split --------------------------------------------------------- */

static void test_split_normale(void)
{
    /* Un array, non una stringa letterale: shell_split scrive dei NUL dentro
       la riga, e scrivere in un letterale e' un segfault. */
    char line[] = "echo ciao";
    char *argv[SHELL_MAX_ARGS];
    int argc;

    argc = shell_split(line, argv, SHELL_MAX_ARGS);

    check("\"echo ciao\" da' argc 2", argc == 2);
    check("argv[0] e' \"echo\"", argc >= 1 && same(argv[0], "echo"));
    check("argv[1] e' \"ciao\"", argc >= 2 && same(argv[1], "ciao"));

    /* La prova che lo split non copia: il NUL e' stato scritto dentro line, al
       posto dello spazio. E' il modo Unix, e la conseguenza e' che dopo lo
       split la riga non esiste piu' come stringa unica. */
    check("lo split scrive un NUL dentro la riga", line[4] == '\0');
    check("argv punta dentro la riga, non a una copia", argv[0] == line);
}

static void test_split_una_parola(void)
{
    char line[] = "help";
    char *argv[SHELL_MAX_ARGS];

    check("una parola sola da' argc 1",
          shell_split(line, argv, SHELL_MAX_ARGS) == 1);
}

static void test_split_vuoto(void)
{
    char vuota[] = "";
    char spazi[] = "   ";
    char *argv[SHELL_MAX_ARGS];

    check("la riga vuota da' argc 0",
          shell_split(vuota, argv, SHELL_MAX_ARGS) == 0);

    /* Solo spazi non e' una parola vuota: e' nessuna parola. Chi conta i
       separatori invece delle parole qui restituisce 4. */
    check("solo spazi da' argc 0",
          shell_split(spazi, argv, SHELL_MAX_ARGS) == 0);
}

static void test_split_spazi_di_troppo(void)
{
    /* I tre casi che fanno inciampare un ciclo che assume "uno spazio separa
       due parole": spazi iniziali, multipli e finali, tutti insieme. Sintomo
       tipico se sbagliato: argv[0] diventa "" e nessun comando corrisponde. */
    char line[] = "  echo   ciao  ";
    char *argv[SHELL_MAX_ARGS];
    int argc;

    argc = shell_split(line, argv, SHELL_MAX_ARGS);

    check("gli spazi di troppo non producono parole vuote", argc == 2);
    check("con spazi iniziali argv[0] e' ancora \"echo\"",
          argc >= 1 && same(argv[0], "echo"));
    check("con spazi multipli argv[1] e' ancora \"ciao\"",
          argc >= 2 && same(argv[1], "ciao"));
}

static void test_split_troppe_parole(void)
{
    char line[] = "a b c d e f g h i j";
    char *argv[SHELL_MAX_ARGS];

    /* Dieci parole in un array da SHELL_MAX_ARGS: si fermano a max, e non si
       scrive oltre la fine di argv. */
    check("piu' parole di max danno argc == max",
          shell_split(line, argv, SHELL_MAX_ARGS) == SHELL_MAX_ARGS);
}

/* ---- shell_parse_hex ----------------------------------------------------- */

static void hex_ok(const char *s, uint32_t want)
{
    uint32_t got = 0xDEADBEEF;
    char name[64];

    snprintf(name, sizeof(name), "\"%s\" vale 0x%x", s, want);
    check(name, shell_parse_hex(s, &got) == 1 && got == want);
}

static void hex_ko(const char *s)
{
    uint32_t got = 0xDEADBEEF;
    char name[64];

    snprintf(name, sizeof(name), "\"%s\" e' rifiutato", s);

    /* Due condizioni: deve fallire E non toccare *out. Chi chiama deve poter
       tenere il valore che aveva. */
    check(name, shell_parse_hex(s, &got) == 0 && got == 0xDEADBEEF);
}

static void test_parse_hex(void)
{
    /* La base e' sempre 16, il prefisso e' facoltativo: "1000" vale 0x1000 e
       non mille. E' la convenzione di peek, ed e' documentata nell'header. */
    hex_ok("1000", 0x1000);
    hex_ok("0x1000", 0x1000);
    hex_ok("0X1000", 0x1000);
    hex_ok("ff", 0xFF);
    hex_ok("FF", 0xFF);
    hex_ok("0", 0);
    hex_ok("ffffffff", 0xFFFFFFFF);
    hex_ok("b8000", 0xB8000);        /* il framebuffer: il caso d'uso vero */

    /* La stringa vuota e "0x" da solo sono lo stesso errore: nessuna cifra
       letta. Un accumulatore inizializzato a zero che non conta le cifre
       restituisce 0 e sembra riuscito — e allora "peek" senza argomenti
       leggerebbe l'indirizzo 0 invece di lamentarsi. */
    hex_ko("");
    hex_ko("0x");
    hex_ko("0X");

    hex_ko("xyz");
    hex_ko("12g4");                  /* cifra non valida in mezzo, non una fine */
    hex_ko(" 10");                   /* lo spazio non e' una cifra */

    /* Nove cifre non ci starebbero in 32 bit: troncare in silenzio darebbe un
       indirizzo plausibile e sbagliato, che e' il peggiore dei due esiti. */
    hex_ko("123456789");
}

int main(void)
{
    test_split_normale();
    test_split_una_parola();
    test_split_vuoto();
    test_split_spazi_di_troppo();
    test_split_troppe_parole();
    test_parse_hex();

    if (failures == 0) {
        printf("tutti i test di shell_split e shell_parse_hex passano\n");
        return 0;
    }

    printf("%d test falliti\n", failures);
    return 1;
}
