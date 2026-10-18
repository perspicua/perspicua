/*
 * test_timer.c - The system timer, busy-wait sleeps, and the build-day wall clock.
 */

#include "test.h"

#include "arch/irq.h"

#include "core/timer.h"

void test_timer(void)
{
    TEST_SUITE_BEGIN("Timer");

    // timer_get_system_time returns a value
    {
        unsigned long t = timer_get_system_time();
        TEST_ASSERT("system time non-zero", t > 0);
    }

    // monotonicity
    {
        unsigned long t1 = timer_get_system_time();
        for (volatile int i = 0; i < 10000; i++) {}
        unsigned long t2 = timer_get_system_time();
        TEST_ASSERT("time monotonic", t2 >= t1);
    }

    // timer_sleep_ms accuracy (rough check)
    {
        unsigned long before = timer_get_system_time();
        timer_sleep_ms(50);
        unsigned long after = timer_get_system_time();
        unsigned long elapsed = after - before;
        TEST_ASSERT("sleep 50ms lower bound", elapsed >= 40);
        TEST_ASSERT("sleep 50ms upper bound", elapsed < 200);
    }

    // multiple sleeps accumulate
    {
        unsigned long before = timer_get_system_time();
        timer_sleep_ms(20);
        timer_sleep_ms(20);
        unsigned long after = timer_get_system_time();
        unsigned long elapsed = after - before;
        TEST_ASSERT("double sleep lower", elapsed >= 30);
        TEST_ASSERT("double sleep upper", elapsed < 200);
    }

    // sleep 0 ms shouldn't hang and should return quickly
    {
        unsigned long before = timer_get_system_time();
        timer_sleep_ms(0);
        unsigned long after = timer_get_system_time();
        TEST_ASSERT("sleep 0ms returns", (after - before) < 50);
    }

    // civil dates convert to epoch seconds, across leap rules and before 1970
    {
        TEST_ASSERT_EQ("civil: the epoch", timer_civil_to_epoch(1970, 1, 1, 0, 0, 0), 0);
        TEST_ASSERT_EQ("civil: the second before it",
                       timer_civil_to_epoch(1969, 12, 31, 23, 59, 59), -1);
        TEST_ASSERT_EQ("civil: 2000 is a leap year", timer_civil_to_epoch(2000, 3, 1, 0, 0, 0),
                       951868800L);
        TEST_ASSERT_EQ("civil: a leap day with a time",
                       timer_civil_to_epoch(2024, 2, 29, 12, 34, 56), 1709210096L);
        TEST_ASSERT_EQ("civil: 2100 is not", timer_civil_to_epoch(2100, 3, 1, 0, 0, 0),
                       4107542400L);
        TEST_ASSERT_EQ("civil: 1900 is not either", timer_civil_to_epoch(1900, 3, 1, 0, 0, 0),
                       -2203891200L);
    }

    // the wall clock is the build date plus uptime, so it is past 2026 and never runs backwards
    {
        time_t a = timer_get_wall_time();
        time_t b = timer_get_wall_time();
        TEST_ASSERT("wall time is after the tree's first build year",
                    a >= timer_civil_to_epoch(2026, 1, 1, 0, 0, 0));
        TEST_ASSERT("wall time does not run backwards", b >= a);
    }

    // a span of counter ticks converts to the milliseconds it took
    {
        uint64_t before = timer_ticks();
        timer_sleep_ms(50);
        uint64_t ms = timer_ticks_to_ms(timer_ticks() - before);
        TEST_ASSERT("ticks to ms: 50ms lower bound", ms >= 40);
        TEST_ASSERT("ticks to ms: 50ms upper bound", ms < 200);
    }

    // irq_save / irq_restore
    {
        unsigned long flags_before;
        asm volatile("mrs %0, daif" : "=r"(flags_before));

        unsigned long saved = irq_save();
        unsigned long flags_masked;
        asm volatile("mrs %0, daif" : "=r"(flags_masked));
        TEST_ASSERT("irq masked", (flags_masked & (1 << 7)) != 0);

        irq_restore(saved);

        unsigned long flags_after;
        asm volatile("mrs %0, daif" : "=r"(flags_after));
        TEST_ASSERT_EQ("irq restored", flags_after, flags_before);
    }

    TEST_SUITE_END("Timer");
}
