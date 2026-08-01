#include "types.h"
#include "memory.h"

void *memcpy(void *dest, const void *src, size_t count)
{
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    size_t n = (size_t)count;

    for (size_t i=0; i<n; i++) {
        *pdest = *psrc;
        psrc++; pdest++;
    }

    return dest;
}

void *memset16(uint16_t *dest, uint16_t value, size_t count)
{
    uint16_t *pdest = dest;

    for (size_t i=0; i<count; i++) {
        *pdest = value;
        pdest++;
    }

    return dest;
}

void *memset(void *dest, int ch, size_t n)
{
    uint8_t *pdest = (uint8_t *)dest;

    for (size_t i=0; i<n; i++) {
        *pdest = ch;
        pdest++;
    }

    return dest;
}

size_t strlen(const char *a)
{
    size_t c = 0;
    const char *s = a;

    while (*s != '\0') {
        ++c; ++s;
    }

    return c;
}

int strpos(const char *l, char a)
{
    int r;
    char *p;

    p = strchr(l, (int)a);

    if (p == 0) {
        r = -1;
    } else if (a == '\0') {
        r = -1;
    } else {
        r = p-l;
    }

    return r;
}

char *strchr(const char *s, int c)
{
    char *a = (char *)s;

    for (;;) {
        if (*a == (char)c) {
            break;
        }
        
        if (*a == '\0') {
            a = 0;
            break;
        }

        ++a;
    }

    return a;
}

char tolower(char argument)
{
    char c = argument;

    if (argument >= 65 && argument <= 90) {
        c = argument + 32;
    }

    return c;
}

int strcmp(const char *a, const char *b)
{
    int ret = 0;

    while (1) {
        if ((unsigned char)*a > (unsigned char)*b) {
            ret = 1;
            break;
        } else if ((unsigned char)*a < (unsigned char)*b) {
            ret = -1;
            break;
        }

        if (*a == '\0') {
            break;
        }

        if (*b == '\0') {
            break;
        }

        ++a; ++b;
    }

    return ret;
}