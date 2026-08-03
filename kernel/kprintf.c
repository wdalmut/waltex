#include "kprintf.h"
#include "serial.h"
#include "vga.h"

/* Lo stato del sink che scrive in un buffer invece che su un dispositivo.
 *
 * Lo "static" va sulla VARIABILE, non sulla definizione del tipo: quella con lo
 * static davanti e' una dichiarazione vuota con una classe di memoria, e gcc la
 * segnala a ogni build — un avviso permanente e giusto, cioe' un avviso che si
 * smette di leggere.
 *
 * PERCHE' E' UNA GLOBALE, e cosa questo costa. kvprintf prende un puntatore a
 * funzione che riceve un carattere e nient'altro: non c'e' un parametro di
 * contesto, quindi il sink deve trovare da se' dove scrivere.
 *
 * Il save/restore dentro vsnprintf rende sicuro l'ANNIDAMENTO — una vsnprintf
 * dentro un'altra — ma NON l'INTRECCIO. Se il task B viene prelazionato a meta'
 * e A riprende, A trova lo stato di B e scrive nel suo buffer. Oggi e' innocuo
 * perche' il chiamante e' uno: procfs, dal task della shell. E kprintf non
 * c'entra, perche' tocca questo stato solo attraverso sn_putc.
 *
 * La cura vera e' un "void *ctx" nel sink di kvprintf, che togliendo la globale
 * risolverebbe anche il debito di M1 — kprintf che formatta due volte — e non
 * solo mezzo, come ha fatto il va_copy qui sotto. E' un cambio piu' grande di
 * quello che M11d voleva. */
struct snbuf { char *p; size_t left; size_t n; };
static struct snbuf sn_st;

static void reverse(char *, int);
static void put_uint(void (*)(char), uint32_t, uint32_t);

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

static void put_uint(void (*putc)(char), uint32_t value, uint32_t base)
{
    uint16_t i = 0;
    uint8_t is_negative = 0;

    char str[34] = { 0 };

    if (value == 0) {
        putc('0');
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

        putc(str[i]);
    }
}

void kvprintf(void (*putc)(char), const char *fmt, va_list args)
{
    uint32_t v = 0;
    char cc;

    while ((*fmt) != '\0') {
        char c = *fmt;

        if (c == '%') {
            char s = *(fmt+1);
            switch (s) {
                case '%':
                    putc('%');
                break;
                case 'd':
                    v =  va_arg(args, int);
                    put_uint(putc, v, 10);
                break;
                case 'x':
                    v =  va_arg(args, int);
                    put_uint(putc, v, 16);
                break;
                case 'c':
                    cc = (char)va_arg(args, int);
                    putc(cc);
                break;
                case 's': {
                    const char *str = va_arg(args, const char*);
                    if (str == ((void*)0)) { 
                        str = "(null)";
                    }


                    while (*str != '\0') {
                        putc(*str);
                        ++str;
                    }
                } break;
                case '\0':
                    putc(c);
                break;
                default:
                    putc(c);
                    putc(s);
                break;
            }
            if (s != '\0') {
                ++fmt;
            }
        } else {
            putc(*fmt);
        }
        ++fmt;
    }
}

void kprintf(const char *fmt, ...)
{
    va_list args, args2;

    va_start(args, fmt);
    va_copy(args2, args);

    kvprintf(serial_putc, fmt, args);
    kvprintf(vga_putc, fmt, args2);

    va_end(args2);
    va_end(args);
}

static void sn_putc(char c)
{
    sn_st.n++;                      /* conta sempre, anche se non scrive */
    if (sn_st.left > 1) {           /* 1 byte riservato al '\0' */
        *sn_st.p++ = c;
        sn_st.left--;
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct snbuf save = sn_st;      /* permette la ricorsione */
    sn_st.p = buf; sn_st.left = size; sn_st.n = 0;

    kvprintf(sn_putc, fmt, ap);

    if (size) *sn_st.p = '\0';
    int r = (int)sn_st.n;
    sn_st = save;
    return r;                       /* semantica C99: lunghezza "voluta" */
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap; int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
