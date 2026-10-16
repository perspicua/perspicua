/*
 * ksym.h - The kernel's text symbols, for naming addresses in backtraces.
 */

#ifndef PERSPICUA_DEBUG_KSYM_H
#define PERSPICUA_DEBUG_KSYM_H

struct ksym {
    unsigned long addr;
    const char *name;
};

// Sorted by address. Data only, so both link passes lay text out identically.
extern const struct ksym ksym_table[];
extern const unsigned long ksym_count;

#endif // PERSPICUA_DEBUG_KSYM_H
