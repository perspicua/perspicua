#include "test.h"
#include "core/lock.h"
#include "core/timer.h"
#include "string.h"

void test_spinlock(void)
{
    TEST_SUITE_BEGIN("Spinlock");

    // init state
    {
        spinlock_t lock = SPINLOCK_INIT;
        TEST_ASSERT_EQ("init unlocked", lock.locked, 0);
    }
    TEST_PASS("spinlock init");

    // spinlock_init zeroes the struct
    {
        spinlock_t lock = SPINLOCK_INIT;
        TEST_ASSERT_EQ("init struct size", sizeof(lock.locked), sizeof(unsigned int));
        TEST_ASSERT_EQ("init value zero", lock.locked, 0);
    }
    TEST_PASS("spinlock struct layout");

    // basic lock / unlock
    {
        spinlock_t lock = SPINLOCK_INIT;
        spin_lock(&lock);
        TEST_ASSERT_EQ("locked", lock.locked, 1);
        spin_unlock(&lock);
        TEST_ASSERT_EQ("unlocked", lock.locked, 0);
    }
    TEST_PASS("spinlock lock/unlock");

    // irqsave / irqrestore
    {
        spinlock_t lock = SPINLOCK_INIT;
        unsigned long flags = spin_lock_irqsave(&lock);
        TEST_ASSERT_EQ("irqsave locked", lock.locked, 1);
        spin_unlock_irqrestore(&lock, flags);
        TEST_ASSERT_EQ("irqrestore unlocked", lock.locked, 0);
    }
    TEST_PASS("spinlock irqsave/irqrestore");

    // irqs are actually masked while holding lock
    {
        spinlock_t lock = SPINLOCK_INIT;
        unsigned long flags = spin_lock_irqsave(&lock);

        unsigned long daif;
        asm volatile("mrs %0, daif" : "=r"(daif));
        TEST_ASSERT("irqs masked while held", (daif & (1 << 7)) != 0);

        spin_unlock_irqrestore(&lock, flags);
    }
    TEST_PASS("spinlock irqs masked while held");

    // irq flags preserved across lock/unlock
    {
        spinlock_t lock = SPINLOCK_INIT;

        unsigned long before_flags;
        asm volatile("mrs %0, daif" : "=r"(before_flags));

        unsigned long f = spin_lock_irqsave(&lock);
        spin_unlock_irqrestore(&lock, f);

        unsigned long after_flags;
        asm volatile("mrs %0, daif" : "=r"(after_flags));

        TEST_ASSERT_EQ("irq flags preserved", after_flags, before_flags);
    }
    TEST_PASS("spinlock irq flags preservation");

    // irqsave with irqs already disabled
    {
        spinlock_t lock = SPINLOCK_INIT;

        // disable IRQs first, then take the lock
        unsigned long outer = irq_save();

        unsigned long daif_disabled;
        asm volatile("mrs %0, daif" : "=r"(daif_disabled));
        TEST_ASSERT("pre-disabled irqs", (daif_disabled & (1 << 7)) != 0);

        unsigned long inner = spin_lock_irqsave(&lock);
        spin_unlock_irqrestore(&lock, inner);

        // IRQs should still be disabled (outer save hasn't been restored)
        unsigned long daif_after;
        asm volatile("mrs %0, daif" : "=r"(daif_after));
        TEST_ASSERT("irqs still disabled after inner", (daif_after & (1 << 7)) != 0);

        irq_restore(outer);
    }
    TEST_PASS("spinlock nested irq disable");

    // re-acquire after unlock
    {
        spinlock_t lock = SPINLOCK_INIT;
        spin_lock(&lock);
        spin_unlock(&lock);
        spin_lock(&lock);
        spin_unlock(&lock);
        TEST_ASSERT_EQ("re-acquire clean", lock.locked, 0);
    }
    TEST_PASS("spinlock re-acquire");

    // lock protects a shared counter (single-core)
    {
        spinlock_t lock = SPINLOCK_INIT;
        volatile int counter = 0;

        for (int i = 0; i < 500; i++)
        {
            spin_lock(&lock);
            counter++;
            spin_unlock(&lock);
        }
        TEST_ASSERT_EQ("protected counter", counter, 500);
    }
    TEST_PASS("spinlock protected counter");

    // lock protects a shared counter (irqsave variant)
    {
        spinlock_t lock = SPINLOCK_INIT;
        volatile int counter = 0;

        for (int i = 0; i < 500; i++)
        {
            unsigned long f = spin_lock_irqsave(&lock);
            counter++;
            spin_unlock_irqrestore(&lock, f);
        }
        TEST_ASSERT_EQ("irqsave counter", counter, 500);
    }
    TEST_PASS("spinlock irqsave counter");

    // many sequential lock/unlock cycles
    {
        spinlock_t stress_lock = SPINLOCK_INIT;
        for (int i = 0; i < 10000; i++)
        {
            spin_lock(&stress_lock);
            spin_unlock(&stress_lock);
        }
        TEST_ASSERT_EQ("stress unlocked", stress_lock.locked, 0);
    }
    TEST_PASS("spinlock 10000x lock/unlock");

    // irqsave stress
    {
        spinlock_t irq_lock = SPINLOCK_INIT;
        for (int i = 0; i < 1000; i++)
        {
            unsigned long f = spin_lock_irqsave(&irq_lock);
            spin_unlock_irqrestore(&irq_lock, f);
        }
        TEST_ASSERT_EQ("irq stress unlocked", irq_lock.locked, 0);
    }
    TEST_PASS("spinlock 1000x irqsave/restore");

    // multiple independent locks
    {
        spinlock_t lock_a = SPINLOCK_INIT;
        spinlock_t lock_b = SPINLOCK_INIT;

        spin_lock(&lock_a);
        spin_lock(&lock_b);
        TEST_ASSERT_EQ("lock_a held", lock_a.locked, 1);
        TEST_ASSERT_EQ("lock_b held", lock_b.locked, 1);
        spin_unlock(&lock_b);
        spin_unlock(&lock_a);
        TEST_ASSERT_EQ("lock_a released", lock_a.locked, 0);
        TEST_ASSERT_EQ("lock_b released", lock_b.locked, 0);
    }
    TEST_PASS("spinlock multiple independent");

    // three-lock nesting (ordered acquire/release)
    {
        spinlock_t la = SPINLOCK_INIT;
        spinlock_t lb = SPINLOCK_INIT;
        spinlock_t lc = SPINLOCK_INIT;

        spin_lock(&la);
        spin_lock(&lb);
        spin_lock(&lc);
        TEST_ASSERT_EQ("3-lock a held", la.locked, 1);
        TEST_ASSERT_EQ("3-lock b held", lb.locked, 1);
        TEST_ASSERT_EQ("3-lock c held", lc.locked, 1);
        spin_unlock(&lc);
        spin_unlock(&lb);
        spin_unlock(&la);
        TEST_ASSERT_EQ("3-lock a free", la.locked, 0);
        TEST_ASSERT_EQ("3-lock b free", lb.locked, 0);
        TEST_ASSERT_EQ("3-lock c free", lc.locked, 0);
    }
    TEST_PASS("spinlock 3-lock nesting");

    // irqsave nesting with multiple locks
    {
        spinlock_t la = SPINLOCK_INIT;
        spinlock_t lb = SPINLOCK_INIT;

        unsigned long flags_before;
        asm volatile("mrs %0, daif" : "=r"(flags_before));

        unsigned long fa = spin_lock_irqsave(&la);
        unsigned long fb = spin_lock_irqsave(&lb);

        // both must be held
        TEST_ASSERT_EQ("nested irq a held", la.locked, 1);
        TEST_ASSERT_EQ("nested irq b held", lb.locked, 1);

        spin_unlock_irqrestore(&lb, fb);
        spin_unlock_irqrestore(&la, fa);

        unsigned long flags_after;
        asm volatile("mrs %0, daif" : "=r"(flags_after));
        TEST_ASSERT_EQ("nested irq flags restored", flags_after, flags_before);
    }
    TEST_PASS("spinlock nested irqsave multi-lock");

    // memory barrier semantics: data visible after unlock
    {
        spinlock_t lock = SPINLOCK_INIT;
        volatile unsigned long shared = 0;

        spin_lock(&lock);
        shared = 0xDEADBEEFCAFEBABEUL;
        spin_unlock(&lock);

        // after release, the value should be globally visible
        spin_lock(&lock);
        TEST_ASSERT("barrier: data visible", shared == 0xDEADBEEFCAFEBABEUL);
        spin_unlock(&lock);
    }
    TEST_PASS("spinlock memory barrier");

    // lock protecting a buffer write
    {
        spinlock_t lock = SPINLOCK_INIT;
        char buf[32];

        spin_lock(&lock);
        memset(buf, 0, sizeof(buf));
        for (int i = 0; i < 32; i++)
            buf[i] = (char)(i + 1);
        spin_unlock(&lock);

        spin_lock(&lock);
        int ok = 1;
        for (int i = 0; i < 32; i++)
            if (buf[i] != (char)(i + 1))
            {
                ok = 0;
                break;
            }
        TEST_ASSERT("buffer intact under lock", ok);
        spin_unlock(&lock);
    }
    TEST_PASS("spinlock buffer protection");

    // alternating lock/unlock doesn't corrupt state
    {
        spinlock_t lock = SPINLOCK_INIT;
        volatile unsigned long val = 0;

        for (int i = 0; i < 200; i++)
        {
            spin_lock(&lock);
            val = (unsigned long)i;
            TEST_ASSERT("alt value correct", val == (unsigned long)i);
            spin_unlock(&lock);
        }
        TEST_ASSERT_EQ("alt final value", val, 199);
    }
    TEST_PASS("spinlock alternating state");

    // lock/unlock timing: doesn't hang
    {
        spinlock_t lock = SPINLOCK_INIT;
        unsigned long t1 = get_system_time();

        for (int i = 0; i < 5000; i++)
        {
            spin_lock(&lock);
            spin_unlock(&lock);
        }

        unsigned long t2 = get_system_time();
        unsigned long elapsed = t2 - t1;
        // 5000 uncontended lock/unlock cycles should complete in <1s
        TEST_ASSERT("timing: not hung", elapsed < 1000);
    }
    TEST_PASS("spinlock timing sanity");

    TEST_SUITE_END("Spinlock");
}
