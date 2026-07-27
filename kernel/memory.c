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