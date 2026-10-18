/*
 * test_spinlock.c - Spinlocks: IRQ masking, nesting, and mutual exclusion across cores.
 */

#include "test.h"

#include "string.h"

#include "arch/cpu.h"
#include "arch/irq.h"

#include "core/lock.h"
#include "core/timer.h"
#include "sched/sched.h"

void test_spinlock(void)
{
    TEST_SUITE_BEGIN("Spinlock");

    // init state
    {
        spinlock_t lock = SPINLOCK_INIT;
        TEST_ASSERT_EQ("init unlocked", lock.locked, 0);
    }

    // SPINLOCK_INIT is a single zeroed word
    {
        spinlock_t lock = SPINLOCK_INIT;
        TEST_ASSERT_EQ("init struct size", sizeof(lock.locked), sizeof(unsigned int));
        TEST_ASSERT_EQ("init value zero", lock.locked, 0);
    }

    // basic lock / unlock
    {
        spinlock_t lock = SPINLOCK_INIT;
        spin_lock(&lock);
        TEST_ASSERT_EQ("locked", lock.locked, 1);
        spin_unlock(&lock);
        TEST_ASSERT_EQ("unlocked", lock.locked, 0);
    }

    // irqsave / irqrestore
    {
        spinlock_t lock = SPINLOCK_INIT;
        unsigned long flags = spin_lock_irqsave(&lock);
        TEST_ASSERT_EQ("irqsave locked", lock.locked, 1);
        spin_unlock_irqrestore(&lock, flags);
        TEST_ASSERT_EQ("irqrestore unlocked", lock.locked, 0);
    }

    // irqs are actually masked while holding lock
    {
        spinlock_t lock = SPINLOCK_INIT;
        unsigned long flags = spin_lock_irqsave(&lock);

        unsigned long daif;
        asm volatile("mrs %0, daif" : "=r"(daif));
        TEST_ASSERT("irqs masked while held", (daif & (1 << 7)) != 0);

        spin_unlock_irqrestore(&lock, flags);
    }

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

    // irqsave with irqs already disabled
    {
        spinlock_t lock = SPINLOCK_INIT;

        unsigned long outer = irq_save();

        unsigned long daif_disabled;
        asm volatile("mrs %0, daif" : "=r"(daif_disabled));
        TEST_ASSERT("pre-disabled irqs", (daif_disabled & (1 << 7)) != 0);

        unsigned long inner = spin_lock_irqsave(&lock);
        spin_unlock_irqrestore(&lock, inner);

        // the inner restore returns to the outer save's masked state
        unsigned long daif_after;
        asm volatile("mrs %0, daif" : "=r"(daif_after));
        TEST_ASSERT("irqs still disabled after inner", (daif_after & (1 << 7)) != 0);

        irq_restore(outer);
    }

    // re-acquire after unlock
    {
        spinlock_t lock = SPINLOCK_INIT;
        spin_lock(&lock);
        spin_unlock(&lock);
        spin_lock(&lock);
        spin_unlock(&lock);
        TEST_ASSERT_EQ("re-acquire clean", lock.locked, 0);
    }

    // lock protects a shared counter (single-core)
    {
        spinlock_t lock = SPINLOCK_INIT;
        volatile int counter = 0;

        for (int i = 0; i < 500; i++) {
            spin_lock(&lock);
            counter++;
            spin_unlock(&lock);
        }
        TEST_ASSERT_EQ("protected counter", counter, 500);
    }

    // lock protects a shared counter (irqsave variant)
    {
        spinlock_t lock = SPINLOCK_INIT;
        volatile int counter = 0;

        for (int i = 0; i < 500; i++) {
            unsigned long f = spin_lock_irqsave(&lock);
            counter++;
            spin_unlock_irqrestore(&lock, f);
        }
        TEST_ASSERT_EQ("irqsave counter", counter, 500);
    }

    // many sequential lock/unlock cycles
    {
        spinlock_t stress_lock = SPINLOCK_INIT;
        for (int i = 0; i < 10000; i++) {
            spin_lock(&stress_lock);
            spin_unlock(&stress_lock);
        }
        TEST_ASSERT_EQ("stress unlocked", stress_lock.locked, 0);
    }

    // irqsave stress
    {
        spinlock_t irq_lock = SPINLOCK_INIT;
        for (int i = 0; i < 1000; i++) {
            unsigned long f = spin_lock_irqsave(&irq_lock);
            spin_unlock_irqrestore(&irq_lock, f);
        }
        TEST_ASSERT_EQ("irq stress unlocked", irq_lock.locked, 0);
    }

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

    // irqsave nesting with multiple locks
    {
        spinlock_t la = SPINLOCK_INIT;
        spinlock_t lb = SPINLOCK_INIT;

        unsigned long flags_before;
        asm volatile("mrs %0, daif" : "=r"(flags_before));

        unsigned long fa = spin_lock_irqsave(&la);
        unsigned long fb = spin_lock_irqsave(&lb);

        TEST_ASSERT_EQ("nested irq a held", la.locked, 1);
        TEST_ASSERT_EQ("nested irq b held", lb.locked, 1);

        spin_unlock_irqrestore(&lb, fb);
        spin_unlock_irqrestore(&la, fa);

        unsigned long flags_after;
        asm volatile("mrs %0, daif" : "=r"(flags_after));
        TEST_ASSERT_EQ("nested irq flags restored", flags_after, flags_before);
    }

    // memory barrier semantics: data visible after unlock
    {
        spinlock_t lock = SPINLOCK_INIT;
        volatile unsigned long shared = 0;

        spin_lock(&lock);
        shared = 0xDEADBEEFCAFEBABEUL;
        spin_unlock(&lock);

        spin_lock(&lock);
        TEST_ASSERT("barrier: data visible", shared == 0xDEADBEEFCAFEBABEUL);
        spin_unlock(&lock);
    }

    // lock protecting a buffer write
    {
        spinlock_t lock = SPINLOCK_INIT;
        char buf[32];

        spin_lock(&lock);
        memset(buf, 0, sizeof(buf));
        for (int i = 0; i < 32; i++) {
            buf[i] = (char)(i + 1);
        }
        spin_unlock(&lock);

        spin_lock(&lock);
        int ok = 1;
        for (int i = 0; i < 32; i++) {
            if (buf[i] != (char)(i + 1)) {
                ok = 0;
                break;
            }
        }
        TEST_ASSERT("buffer intact under lock", ok);
        spin_unlock(&lock);
    }

    // alternating lock/unlock doesn't corrupt state
    {
        spinlock_t lock = SPINLOCK_INIT;
        volatile unsigned long val = 0;

        for (int i = 0; i < 200; i++) {
            spin_lock(&lock);
            val = (unsigned long)i;
            TEST_ASSERT("alt value correct", val == (unsigned long)i);
            spin_unlock(&lock);
        }
        TEST_ASSERT_EQ("alt final value", val, 199);
    }

    // lock/unlock timing: doesn't hang
    {
        spinlock_t lock = SPINLOCK_INIT;
        unsigned long t1 = timer_get_system_time();

        for (int i = 0; i < 5000; i++) {
            spin_lock(&lock);
            spin_unlock(&lock);
        }

        unsigned long t2 = timer_get_system_time();
        unsigned long elapsed = t2 - t1;
        TEST_ASSERT("timing: not hung", elapsed < 1000);
    }

    TEST_SUITE_END("Spinlock");
}

#define SPIN_STRESS_WORKERS 4
#define SPIN_STRESS_ROUNDS  20000

static spinlock_t spin_stress_lock = SPINLOCK_INIT;
static unsigned long spin_stress_count;
static int spin_stress_done;
static unsigned int spin_stress_cores;

// A plain read-modify-write, so only the lock keeps the count whole; sleeps spread the workers.
static void spin_stress_worker(void)
{
    for (int i = 0; i < SPIN_STRESS_ROUNDS; i++) {
        spin_lock(&spin_stress_lock);
        unsigned long v = *(volatile unsigned long *)&spin_stress_count;
        for (volatile int d = 0; d < 8; d++) {}
        *(volatile unsigned long *)&spin_stress_count = v + 1;
        __atomic_fetch_or(&spin_stress_cores, 1u << cpu_id(), __ATOMIC_RELAXED);
        spin_unlock(&spin_stress_lock);

        if (i % 2000 == 1999) {
            sched_sleep_ms(1);
        }
    }
    __atomic_fetch_add(&spin_stress_done, 1, __ATOMIC_SEQ_CST);
}

void test_spinlock_scheduler(void)
{
    TEST_SUITE_BEGIN("Spinlock-MultiTask");

    spin_stress_count = 0;
    spin_stress_done = 0;
    spin_stress_cores = 0;

    int created = 0;
    for (int i = 0; i < SPIN_STRESS_WORKERS; i++) {
        created += sched_create_task(spin_stress_worker) == 0;
    }
    for (int n = 0; n < 1000 && __atomic_load_n(&spin_stress_done, __ATOMIC_SEQ_CST) < created;
         n++) {
        sched_sleep_ms(10);
    }

    TEST_ASSERT_EQ("spinlock stress: every worker created", created, SPIN_STRESS_WORKERS);
    TEST_ASSERT_EQ("spinlock stress: every worker finished", spin_stress_done, created);
    TEST_ASSERT_EQ("spinlock stress: no increment lost", spin_stress_count,
                   (long)created * SPIN_STRESS_ROUNDS);
    int cores = 0;
    for (unsigned int m = spin_stress_cores; m; m &= m - 1) {
        cores++;
    }
    pr_info("test: spinlock stress ran on %d cores\n", cores);

    TEST_SUITE_END("Spinlock-MultiTask");
}
