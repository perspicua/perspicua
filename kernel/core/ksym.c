/*
 * ksym.c - Resolves kernel text addresses against the generated symbol table.
 */

#include "debug/ksym.h"

#include <stddef.h>

#include "panic.h"

extern char __text_start[], __text_end[];

const char *panic_resolve_symbol(unsigned long addr, unsigned long *offset)
{
    if (addr < (unsigned long)__text_start || addr >= (unsigned long)__text_end) {
        return NULL;
    }

    long low = 0;
    long high = (long)ksym_count - 1;
    long best = -1;
    while (low <= high) {
        long mid = low + (high - low) / 2;
        if (ksym_table[mid].addr <= addr) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    if (best < 0) {
        return NULL;
    }
    *offset = addr - ksym_table[best].addr;
    return ksym_table[best].name;
}
