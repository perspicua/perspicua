#ifndef _TYPES_H_
#define _TYPES_H_

// ---------------------------------------------------------------------------
// NULL
// ---------------------------------------------------------------------------
#define NULL ((void*)0)

// ---------------------------------------------------------------------------
// Fixed-width integer types
// ---------------------------------------------------------------------------
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long int64_t;
typedef unsigned long uint64_t;

// ---------------------------------------------------------------------------
// Pointer-width types  (AArch64: 64-bit)
// ---------------------------------------------------------------------------
typedef unsigned long size_t;
typedef signed long ssize_t;
typedef signed long ptrdiff_t;
typedef unsigned long uintptr_t;
typedef signed long intptr_t;

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
#define INT8_MIN (-128)
#define INT8_MAX (127)
#define UINT8_MAX (255U)

#define INT16_MIN (-32768)
#define INT16_MAX (32767)
#define UINT16_MAX (65535U)

#define INT32_MIN (-2147483648)
#define INT32_MAX (2147483647)
#define UINT32_MAX (4294967295U)

#define INT64_MIN (-9223372036854775808LL)
#define INT64_MAX (9223372036854775807LL)
#define UINT64_MAX (18446744073709551615ULL)

#define SIZE_MAX UINT64_MAX

#endif // _TYPES_H_
