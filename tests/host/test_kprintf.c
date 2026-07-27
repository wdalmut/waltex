/* Compilato col gcc dell'host, senza QEMU: ciclo di prova da millisecondi.
   WALTEX_HOSTED (definito nel Makefile) fa arrivare i tipi da stdint.h
   invece che da types.h, così non collidono con quelli di glibc. */

#include <stdio.h>

#include "kprintf.h"

/* Niente <string.h>: il progetto ha un proprio include/string.h che, essendo
   raggiunto via -I, oscura quello di glibc. Il confronto ce lo scriviamo,
   sono quattro righe, e il test resta immune a qualunque header il kernel
   deciderà di chiamare come uno di sistema. */
static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* kprintf.c fa riferimento ai due sink del kernel: qui non esistono, quindi
   li rimpiazziamo con stub inerti. Solo kvprintf è sotto test. */
void vga_putc(char c) { (void)c; }
void serial_putc(char c) { (void)c; }

static char buf[1024];
static size_t len;

static void collect(char c)
{
    if (len < sizeof(buf) - 1)
        buf[len++] = c;
}

static int failures;

/* Confronta il numero di byte emessi, non solo la stringa: un formatter che
   emette un NUL di troppo, o che continua a leggere oltre il terminatore del
   formato, produce un output che "sembra" giusto a un confronto stile strcmp
   perché il confronto si ferma al primo zero. Qui no. */
static void expect(const char *want, const char *fmt, ...)
{
    va_list ap;
    size_t want_len = slen(want);
    int ok;
    size_t i;

    len = 0;
    va_start(ap, fmt);
    kvprintf(collect, fmt, ap);
    va_end(ap);
    buf[len] = '\0';

    ok = (len == want_len);
    for (i = 0; ok && i < want_len; i++)
        if (buf[i] != want[i])
            ok = 0;

    if (ok) {
        printf("ok   -- \"%s\" -> \"%s\"\n", fmt, buf);
    } else {
        printf("FAIL -- \"%s\": atteso \"%s\" (%zu byte), "
               "ottenuto \"%s\" (%zu byte)\n",
               fmt, want, want_len, buf, len);
        failures++;
    }
}

int main(void)
{
    /* testo letterale */
    expect("", "");
    expect("ciao", "ciao");
    expect("a\nb", "a\nb");

    /* %d */
    expect("0", "%d", 0);
    expect("42", "%d", 42);
    expect("-42", "%d", -42);
    expect("2147483647", "%d", 2147483647);
    /* il minimo int32: negarlo va in overflow, è l'errore classico */
    expect("-2147483648", "%d", (int)(-2147483647 - 1));

    /* %x, minuscolo e senza zeri iniziali */
    expect("0", "%x", 0u);
    expect("ff", "%x", 255u);
    expect("2badb002", "%x", 0x2BADB002u);
    expect("ffffffff", "%x", 0xFFFFFFFFu);

    /* %s e %c */
    expect("waltex", "%s", "waltex");
    expect("", "%s", "");
    expect("A", "%c", 'A');

    /* %% e specificatore ignoto */
    expect("100%", "100%%");
    expect("%q", "%q");

    /* '%' come ultimo carattere: chi avanza sempre di due caratteri legge
       oltre il terminatore della stringa. */
    expect("ab%", "ab%");
    expect("%", "%");

    /* %s con puntatore nullo: succede nel panic handler, e lì il formatter
       non deve essere la causa del secondo crash. */
    expect("(null)", "%s", (const char *)0);

    /* combinazioni */
    expect("tick=100 addr=b8000", "tick=%d addr=%x", 100, 0xB8000u);
    expect("[waltex] M1 ok", "[%s] M%d ok", "waltex", 1);

    if (failures == 0) {
        printf("tutti i test del formatter passano\n");
        return 0;
    }
    printf("%d test falliti\n", failures);
    return 1;
}
