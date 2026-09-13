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
