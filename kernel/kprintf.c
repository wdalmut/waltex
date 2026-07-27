#include "kprintf.h"
#include "serial.h"
#include "vga.h"

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

    for (uint16_t i=0; i<32; i++) {
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
                case 's':
                    const char *str = va_arg(args, const char*);
                    if (str == ((void*)0)) { 
                        str = "(null)";
                    }


                    while (*str != '\0') {
                        putc(*str);
                        ++str;
                    }
                break;
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
    va_list args;

    va_start(args, fmt);
    kvprintf(serial_putc, fmt, args);
    kvprintf(vga_putc, fmt, args);
    va_end(args);
}