/*
 * timer.c - Implementation of system time and interrupt control.
 *
 * This file handles interaction with the AArch64 generic physical timer
 * and provides primitives for millisecond-precision delays and
 * low-level interrupt state management.
 */

#include "core/timer.h"

#include "stdio.h"

/*
 * read_cntfrq - Internal helper to read the system counter frequency register.
 */
static inline unsigned int read_cntfrq(void)
{
    unsigned int val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

/*
 * read_cntpct - Internal helper to read the physical counter register.
 */
static inline unsigned long read_cntpct(void)
{
    unsigned long val;
    asm volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

/*
 * get_system_time - Returns the uptime in milliseconds.
 */
unsigned long get_system_time(void)
{
    unsigned long freq = read_cntfrq();
    unsigned long count = read_cntpct();

    if (freq < 1000)
    {
        return 0;
    }

    return count / (freq / 1000);
}

/*
 * sleep_ms - Performs a busy-wait delay for the specified duration.
 */
void sleep_ms(unsigned long ms)
{
    unsigned long freq = read_cntfrq();
    if (freq == 0)
    {
        return;
    }

    unsigned long ticks_per_ms = freq / 1000;
    unsigned long current_count = read_cntpct();
    unsigned long target_count = current_count + (ms * ticks_per_ms);

    while (read_cntpct() < target_count)
    {
        asm volatile("yield");
    }
}

/*
 * enable_interrupts - Unmasks IRQs in the current CPU core's DAIF register.
 */
void enable_interrupts(void)
{
    asm volatile("msr daifclr, #2");
}

/*
 * disable_interrupts - Masks IRQs in the current CPU core's DAIF register.
 */
void disable_interrupts(void)
{
    asm volatile("msr daifset, #2");
}

/*
 * timer_interrupt_init - Configures the generic timer for the local CPU core.
 */
void timer_interrupt_init(void)
{
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    core_id &= 3;

    /* The Raspberry Pi 4 local interrupt controller base for core timers */
    unsigned long base_addr = 0xFFFFFF80FF800040 + (core_id * 4);
    volatile unsigned int* core_timer_irq_ctrl = (unsigned int*)base_addr;

    /* Enable physical timer interrupt routing for this core */
    *core_timer_irq_ctrl = (1 << 1);

    unsigned int freq = read_cntfrq();
    /* Set timer to fire at 100Hz (every 10ms) */
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq / 100));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1));

    if (core_id == 0)
    {
        pr_info("timer: generic timer: %u Hz, tick = 100 Hz (10ms)\n", freq);
    }
}

/*
 * timer_interrupt_reset - Reloads the timer value for the next tick.
 */
void timer_interrupt_reset(void)
{
    unsigned int freq = read_cntfrq();
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq / 100));
}

/*
 * irq_save - Disables interrupts and returns the previous state mask.
 */
unsigned long irq_save(void)
{
    unsigned long flags;
    asm volatile("mrs %0, daif" : "=r"(flags));
    asm volatile("msr daifset, #2");
    return flags;
}

/*
 * irq_restore - Restores the core's interrupt state from a saved mask.
 */
void irq_restore(unsigned long flags)
{
    asm volatile("msr daif, %0" : : "r"(flags));
}
