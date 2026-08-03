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

/* Il raccoglitore. Da M11d+ il sink di kvprintf riceve un CONTESTO, e questo
   test e' il primo posto dove si vede a cosa serve: prima buf e len dovevano
   essere globali perche' collect non avesse altro modo di raggiungerli, adesso
   arrivano attraverso il ctx.

   Restano globali qui per non riscrivere expect(), ma il gruppo test_ctx piu'
   sotto usa un contesto LOCALE, che e' la proprieta' nuova. */
static char buf[1024];
static size_t len;

struct raccolta { char *p; size_t len; size_t max; };

static void collect(void *ctx, char c)
{
    (void)ctx;

    if (len < sizeof(buf) - 1)
        buf[len++] = c;
}

/* Lo stesso lavoro, ma prendendo il buffer dal contesto invece che da una
   globale. E' la forma che usa vsnprintf dentro il kernel. */
static void collect_ctx(void *ctx, char c)
{
    struct raccolta *r = (struct raccolta *)ctx;

    if (r->len < r->max - 1)
        r->p[r->len++] = c;
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
    kvprintf(collect, 0, fmt, ap);
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

/* ---- il contesto del sink -----------------------------------------------------

   Cinque controlli, e provano una cosa sola: che kvprintf non abbia piu' NESSUNO
   STATO PROPRIO. E' la proprieta' che rende snprintf corretta sotto prelazione,
   e prima non c'era — lo stato del sink era una globale in kprintf.c.

   Quello che conta e' l'intreccio, in test_intreccio: due formattazioni sospese
   a meta' e riprese a turno. E' esattamente cio' che fa la prelazione, ed e' cio'
   che un save/restore intorno a una globale NON copre.

   Un va_list si costruisce SOLO dentro una funzione variadica: passarne uno non
   inizializzato e' comportamento indefinito, non una scorciatoia. Da cui i due
   gusci di tre righe qui sotto. */
static void fmt_ctx(struct raccolta *r, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    kvprintf(collect_ctx, r, fmt, ap);
    va_end(ap);
}

static void fmt_globale(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    kvprintf(collect, 0, fmt, ap);
    va_end(ap);
}

static void test_ctx(void)
{
    char a[32], b[32];
    struct raccolta ra = { a, 0, sizeof(a) };
    struct raccolta rb = { b, 0, sizeof(b) };

    /* Il contesto e' LOCALE a questa funzione: se kvprintf lo ignorasse e il
       sink si affidasse a una globale, questi buffer resterebbero vuoti. */
    fmt_ctx(&ra, "uno");
    a[ra.len] = '\0';

    if (slen(a) == 3 && a[0] == 'u') {
        printf("ok   -- il sink scrive nel buffer che gli arriva dal contesto\n");
    } else {
        printf("FAIL -- il contesto non e' arrivato al sink: \"%s\"\n", a);
        failures++;
    }

    /* Due contesti diversi non si vedono fra loro. */
    ra.len = 0;
    rb.len = 0;
    fmt_ctx(&ra, "AAA");
    fmt_ctx(&rb, "BBB");
    a[ra.len] = '\0';
    b[rb.len] = '\0';

    if (slen(a) == 3 && a[0] == 'A' && slen(b) == 3 && b[0] == 'B') {
        printf("ok   -- due contesti diversi non si mescolano\n");
    } else {
        printf("FAIL -- i due contesti si sono mescolati: \"%s\" / \"%s\"\n", a, b);
        failures++;
    }

    /* Un ctx nullo non deve dare problemi a chi lo ignora: e' il caso di
       kputc_console e del collect globale qui sopra. */
    len = 0;
    fmt_globale("zero");
    buf[len] = '\0';

    if (slen(buf) == 4) {
        printf("ok   -- un sink che ignora il ctx accetta lo zero\n");
    } else {
        printf("FAIL -- il sink senza contesto non ha scritto: \"%s\"\n", buf);
        failures++;
    }
}

/* L'INTRECCIO, che e' il controllo per cui il ctx esiste.
 *
 * Si simula la prelazione: si formatta a mano, un carattere alla volta, in due
 * "flussi" alternati. Con lo stato del sink in una globale il secondo flusso
 * dirotterebbe il primo — e il save/restore di vsnprintf non aiuta, perche'
 * protegge l'annidamento, non l'intreccio.
 *
 * Non si puo' scrivere con due kvprintf, perche' sull'host non c'e' un timer che
 * prelaziona: si chiamano i sink direttamente, che e' esattamente cio' che
 * kvprintf fa e cio' che il gestore del timer interrompe.
 */
static void test_intreccio(void)
{
    char a[32], b[32];
    struct raccolta ra = { a, 0, sizeof(a) };
    struct raccolta rb = { b, 0, sizeof(b) };
    int i;
    const char *sa = "aaaa";
    const char *sb = "bbbb";

    for (i = 0; i < 4; i++) {
        collect_ctx(&ra, sa[i]);
        collect_ctx(&rb, sb[i]);       /* la "prelazione" fra due caratteri */
    }

    a[ra.len] = '\0';
    b[rb.len] = '\0';

    if (slen(a) == 4 && a[0] == 'a' && a[3] == 'a') {
        printf("ok   -- il primo flusso non e' stato dirottato dal secondo\n");
    } else {
        printf("FAIL -- il primo flusso e' stato dirottato: \"%s\"\n", a);
        failures++;
    }

    if (slen(b) == 4 && b[0] == 'b' && b[3] == 'b') {
        printf("ok   -- ne' il secondo dal primo\n");
    } else {
        printf("FAIL -- il secondo flusso e' stato dirottato: \"%s\"\n", b);
        failures++;
    }
}

/* ---- snprintf ----------------------------------------------------------------

   Il chiamante vero e' procfs, che genera /proc/N/status. La semantica e' C99:
   ritorna la lunghezza VOLUTA, non quella scritta. */
static void test_snprintf(void)
{
    char b[8];
    int r;

    r = snprintf(b, sizeof(b), "ok");

    if (r == 2 && slen(b) == 2 && b[0] == 'o') {
        printf("ok   -- snprintf scrive e ritorna la lunghezza\n");
    } else {
        printf("FAIL -- snprintf: r=%d \"%s\"\n", r, b);
        failures++;
    }

    /* Il troncamento. C99 vuole il valore VOLUTO, cioe' 10, e il buffer
       terminato a 7 caratteri. Ritornare 7 farebbe credere a chi controlla
       "r >= size" di essere andato bene. */
    r = snprintf(b, sizeof(b), "0123456789");

    if (r == 10 && slen(b) == 7) {
        printf("ok   -- il troncamento ritorna la lunghezza VOLUTA, non la scritta\n");
    } else {
        printf("FAIL -- troncamento: r=%d, scritti %zu\n", r, slen(b));
        failures++;
    }

    /* size == 0: non si scrive niente, nemmeno il terminatore, e si ritorna
       comunque la lunghezza voluta. E' il caso che in procfs faceva misurare con
       strlen un buffer che nessuno aveva terminato. */
    b[0] = 'Z';
    r = snprintf(b, 0, "ciao");

    if (r == 4 && b[0] == 'Z') {
        printf("ok   -- snprintf con size 0 non tocca il buffer\n");
    } else {
        printf("FAIL -- size 0: r=%d, b[0]='%c'\n", r, b[0]);
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

    test_ctx();
    test_intreccio();
    test_snprintf();

    if (failures == 0) {
        printf("tutti i test del formatter passano\n");
        return 0;
    }
    printf("%d test falliti\n", failures);
    return 1;
}
