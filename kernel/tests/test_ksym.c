/*
 * test_ksym.c - Backtrace symbol lookup against functions whose addresses are known.
 */

#include "test.h"

#include "debug/ksym.h"
#include "panic.h"
#include "string.h"

__attribute__((noinline)) static int ksym_probe(int x)
{
    return x * 3 + 1;
}

static int resolves_to(unsigned long addr, const char *name, unsigned long offset)
{
    unsigned long got = ~0UL;
    const char *sym = panic_resolve_symbol(addr, &got);
    return sym && strcmp(sym, name) == 0 && got == offset;
}

void test_ksym(void)
{
    TEST_SUITE_BEGIN("Symbols");

    TEST_ASSERT("table is not empty", ksym_count > 0);
    int sorted = 1;
    for (unsigned long i = 1; i < ksym_count; i++) {
        sorted &= ksym_table[i - 1].addr <= ksym_table[i].addr;
    }
    TEST_ASSERT("table is sorted by address", sorted);

    TEST_ASSERT("ksym_probe resolves to itself",
                resolves_to((unsigned long)ksym_probe, "ksym_probe", 0));
    TEST_ASSERT("an address inside ksym_probe keeps its name",
                resolves_to((unsigned long)ksym_probe + 4, "ksym_probe", 4));
    TEST_ASSERT("test_ksym resolves to itself",
                resolves_to((unsigned long)test_ksym, "test_ksym", 0));
    TEST_ASSERT("the resolver resolves to itself",
                resolves_to((unsigned long)panic_resolve_symbol, "panic_resolve_symbol", 0));

    unsigned long off = 0;
    TEST_ASSERT("data is not named as a function",
                panic_resolve_symbol((unsigned long)&ksym_count, &off) == NULL);
    TEST_ASSERT_EQ("ksym_probe still runs", ksym_probe(2), 7);

    TEST_SUITE_END("Symbols");
}
