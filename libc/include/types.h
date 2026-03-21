/*
 * types.h - Standard fixed-width integer and fundamental types.
 *
 * This file defines the platform-specific integer types, pointer-width
 * types, and common limits used throughout the kernel and userspace.
 */

#ifndef PERSPICUA_LIBC_TYPES_H
#define PERSPICUA_LIBC_TYPES_H

/* NULL pointer definition */
#define NULL ((void*)0)

/* Fixed-width integer types */
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long int64_t;
typedef unsigned long uint64_t;

/* Pointer-width types (AArch64 is 64-bit) */
typedef unsigned long size_t;
typedef signed long ssize_t;
typedef signed long ptrdiff_t;
typedef unsigned long uintptr_t;
typedef signed long intptr_t;

typedef int64_t off_t;
/* Integer type limits */
#define INT8_MIN  (-128)
#define INT8_MAX  (127)
#define UINT8_MAX (255U)

#define INT16_MIN  (-32768)
#define INT16_MAX  (32767)
#define UINT16_MAX (65535U)

#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  (2147483647)
#define UINT32_MAX (4294967295U)

#define INT64_MIN  (-9223372036854775807LL - 1)
#define INT64_MAX  (9223372036854775807LL)
#define UINT64_MAX (18446744073709551615ULL)

#define SIZE_MAX UINT64_MAX

/*
 * atomic_t - Structure for atomic integer operations.
 */
typedef struct
{
    volatile int counter;
} atomic_t;

#endif /* PERSPICUA_LIBC_TYPES_H */
