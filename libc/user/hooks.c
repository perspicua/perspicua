#include "types.h"
#include "syscall.h"

void __libc_write(const char* buf, size_t len)
{
    sys_write(1, buf, len);
}
