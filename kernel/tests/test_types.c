/*
 * test_types.c - Integer and pointer widths, limits and signedness under LP64, checked at build
 * time.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uapi/types.h"

_Static_assert(sizeof(uint8_t) == 1, "uint8_t");
_Static_assert(sizeof(uint16_t) == 2, "uint16_t");
_Static_assert(sizeof(uint32_t) == 4, "uint32_t");
_Static_assert(sizeof(uint64_t) == 8, "uint64_t");
_Static_assert(sizeof(int8_t) == 1, "int8_t");
_Static_assert(sizeof(int16_t) == 2, "int16_t");
_Static_assert(sizeof(int32_t) == 4, "int32_t");
_Static_assert(sizeof(int64_t) == 8, "int64_t");

_Static_assert(sizeof(size_t) == 8, "size_t");
_Static_assert(sizeof(ssize_t) == 8, "ssize_t");
_Static_assert(sizeof(uintptr_t) == 8, "uintptr_t");
_Static_assert(sizeof(ptrdiff_t) == 8, "ptrdiff_t");
_Static_assert(sizeof(void *) == 8, "pointers are 64-bit");

_Static_assert((uintptr_t)NULL == 0, "NULL is zero");

_Static_assert(UINT8_MAX == 255, "UINT8_MAX");
_Static_assert(UINT16_MAX == 65535, "UINT16_MAX");
_Static_assert(UINT32_MAX == 4294967295UL, "UINT32_MAX");
_Static_assert(INT8_MIN == -128 && INT8_MAX == 127, "int8_t range");
_Static_assert(INT16_MIN == -32768 && INT16_MAX == 32767, "int16_t range");

_Static_assert((uint8_t)255 + 1 == 256, "uint8_t promotes before it can wrap");
_Static_assert((int8_t)-1 < 0, "int8_t is signed");
