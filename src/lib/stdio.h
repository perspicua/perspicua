#ifndef _STDIO_H_
#define _STDIO_H_

#include "types.h"

// Formatted print to UART
// Supported format specifiers:
//   %d   - signed int (decimal)
//   %u   - unsigned int (decimal)
//   %x   - unsigned int (hex, lowercase)
//   %ld  - signed long / int64_t (decimal)
//   %lu  - unsigned long / uint64_t / size_t (decimal)
//   %lx  - unsigned long (hex, lowercase)
//   %p   - pointer (hex with 0x prefix)
//   %s   - null-terminated string
//   %c   - single character
//   %%   - literal '%'
void printf(const char* fmt, ...);

#endif // _STDIO_H_
