#include "kprintf.h"
#include "serial.h"
#include "vga.h"

/* Lo stato del sink che scrive in un buffer invece che su un dispositivo.
 *
 * NON e' una globale, e la storia vale piu' della struct. Fino a M11d lo era,
 * perche' kvprintf riceveva un puntatore a funzione che accettava un carattere e
 * nient'altro: senza un parametro di contesto, il sink deve trovarsi da se' dove
 * scrivere. Un save/restore intorno alla globale rendeva sicuro l'ANNIDAMENTO —
 * una vsnprintf dentro un'altra — ma NON l'INTRECCIO: se un task viene
 * prelazionato a meta' di una snprintf, il successivo gli scrive nel buffer.
 *
 * Adesso kvprintf porta un "void *ctx" fino al sink, e questa struct vive sullo
 * stack di vsnprintf. Non c'e' piu' niente da salvare perche' non c'e' piu'
 * niente di condiviso.
 *
 * Lo stesso ctx ha chiuso il debito di M1 per intero: con un sink che scrive su
 * entrambi i dispositivi, kprintf fa UNA passata su UN va_list invece di due
 * passate con un va_copy. */
struct snbuf { char *p; size_t left; size_t n; };

static void reverse(char *, int);
static void put_uint(void (*)(void *, char), void *, uint32_t, uint32_t);

static void reverse(char *str, int length) {
    int start = 0;
    int end = length - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }
}

static void put_uint(void (*putc)(void *, char), void *ctx,
                     uint32_t value, uint32_t base)
{
    uint16_t i = 0;
    uint8_t is_negative = 0;

    char str[34] = { 0 };

    if (value == 0) {
        putc(ctx, '0');
        return;
    }

     if (((int32_t)value) < 0 && base == 10) {
        is_negative = 1;
        value = -value;
    }

    while (value != 0) {
        int rem = value % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        value = value / base;
    }

    if (is_negative) {
        str[i++] = '-';
    }

    str[i] = '\0';
    reverse(str, i);

    for (i=0; i<32; i++) {
        if (str[i] == '\0') {
            break;
        }

        putc(ctx, str[i]);
    }
}

void kvprintf(void (*putc)(void *ctx, char c), void *ctx,
              const char *fmt, va_list args)
{
    uint32_t v = 0;
    char cc;

    while ((*fmt) != '\0') {
        char c = *fmt;

        if (c == '%') {
            char s = *(fmt+1);
            switch (s) {
                case '%':
                    putc(ctx, '%');
                break;
                case 'd':
                    v =  va_arg(args, int);
                    put_uint(putc, ctx, v, 10);
                break;
                case 'x':
                    v =  va_arg(args, int);
                    put_uint(putc, ctx, v, 16);
                break;
                case 'c':
                    cc = (char)va_arg(args, int);
                    putc(ctx, cc);
                break;
                case 's': {
                    const char *str = va_arg(args, const char*);
                    if (str == ((void*)0)) { 
                        str = "(null)";
                    }


                    while (*str != '\0') {
                        putc(ctx, *str);
                        ++str;
                    }
                } break;
                case '\0':
                    putc(ctx, c);
                break;
                default:
                    putc(ctx, c);
                    putc(ctx, s);
                break;
            }
            if (s != '\0') {
                ++fmt;
            }
        } else {
            putc(ctx, *fmt);
        }
        ++fmt;
    }
}

/* Il sink doppio, ed e' cio' che chiude il debito di M1 PER INTERO.
 *
 * Fino a qui kprintf formattava DUE VOLTE, una per sink. Il va_copy ne ha
 * risolto meta' — il va_list riusato, legale solo su i386 dove e' un puntatore
 * passato per valore — e questa funzione risolve l'altra: una passata sola su un
 * va_list solo, perche' il sink scrive su entrambi i dispositivi.
 *
 * Serve anche a panic, che non puo' passare da kprintf perche' cambia il colore
 * prima e si ferma dopo. Esportarlo e' cio' che gli evita di rifare i propri
 * adattatori — e soprattutto di rifare il bug che aveva, due kvprintf con lo
 * stesso va_list.
 *
 * Nota su cosa cambia: l'ordine dei byte sui due dispositivi si ALTERNA per
 * carattere invece di essere "tutta la stringa su COM1, poi tutta sulla VGA".
 * Gli stessi byte nello stesso ordine su ciascuno dei due, quindi nessuna
 * conseguenza sul contenuto; una piccolissima in post-mortem, perche' una tripla
 * fault a meta' di una kprintf adesso tronca anche la riga sulla seriale.
 *
 * ctx e' ignorato: la console e' una, non c'e' niente da distinguere. */
void kputc_console(void *ctx, char c)
{
    (void)ctx;

    serial_putc(c);
    vga_putc(c);
}

void kprintf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    kvprintf(kputc_console, 0, fmt, args);
    va_end(args);
}

/* Il sink che scrive in un buffer, e ADESSO SENZA NESSUNA GLOBALE: lo stato
   arriva dal ctx, e vive sullo stack di vsnprintf.
   Da qui esce il testo di /proc/N/status. */
static void sn_putc(void *ctx, char c)
{
    struct snbuf *b = (struct snbuf *)ctx;

    b->n++;                         /* conta sempre, anche se non scrive */

    if (b->left > 1) {              /* 1 byte riservato al '\0' */
        *b->p++ = c;
        b->left--;
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    /* SULLO STACK. Era una globale con un save/restore intorno, che rendeva
       sicuro l'annidamento ma non l'INTRECCIO: un task prelazionato a meta' si
       faceva scrivere dentro il buffer dal successivo. Adesso non c'e' niente da
       salvare, perche' non c'e' piu' nessuno stato condiviso. */
    struct snbuf b;

    b.p = buf;
    b.left = size;
    b.n = 0;

    kvprintf(sn_putc, &b, fmt, ap);

    /* Con size > 0 il terminatore c'e' sempre: sn_putc tiene un byte da parte.
       Con size == 0 non si scrive niente, nemmeno quello — ed e' il caso che in
       procfs faceva misurare con strlen un buffer che nessuno aveva chiuso. */
    if (size)
        *b.p = '\0';

    return (int)b.n;                /* semantica C99: lunghezza "voluta" */
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap; int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
