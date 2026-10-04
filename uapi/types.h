/*
 * types.h - POSIX scalar types that cross the syscall boundary.
 *
 * The fixed-width integers come from the compiler's <stdint.h>; these are the
 * few POSIX spellings a freestanding toolchain does not supply, and both sides
 * of a syscall have to agree on their width.
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
