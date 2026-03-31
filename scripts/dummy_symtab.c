#include "panic.h"

const int kernel_symbol_count = 0;
const void* kernel_symbols = 0;

const char* panic_resolve_symbol(unsigned long addr, unsigned long *offset) {
    (void)addr;
    (void)offset;
    return 0;
}
