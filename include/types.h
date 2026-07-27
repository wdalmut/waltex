#ifndef WALTEX_TYPES_H
#define WALTEX_TYPES_H

/* I test host compilano gli stessi sorgenti con la libc disponibile: in quel
   caso i tipi devono venire da lì, altrimenti i typedef collidono con quelli
   di glibc. Nel kernel invece non esiste nessuna libc e li definiamo noi. */
#ifdef WALTEX_HOSTED
#include <stdint.h>
#include <stddef.h>
#else

typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;

typedef uint32_t size_t;
typedef uint32_t uintptr_t;

#define NULL ((void *)0)

#endif /* WALTEX_HOSTED */
#endif /* WALTEX_TYPES_H */
