/*
 * dummy_symtab.c - The empty symbol table the first link pass is built with.
 */

#include "debug/ksym.h"

const unsigned long ksym_count = 0;
const struct ksym ksym_table[1] = {{0, 0}};
