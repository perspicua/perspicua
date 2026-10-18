/*
 * types.h - The POSIX scalar types a freestanding toolchain lacks, sized for the syscall ABI.
 */

#ifndef PERSPICUA_UAPI_TYPES_H
#define PERSPICUA_UAPI_TYPES_H

#include <stdint.h>

typedef int64_t ssize_t;
typedef int64_t off_t;
typedef uint64_t ino_t;
typedef int64_t time_t;
typedef uint32_t useconds_t;

#endif // PERSPICUA_UAPI_TYPES_H
