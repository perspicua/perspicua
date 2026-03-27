/*
 * timer.c - Implementation of system time and interrupt control.
 *
 * This module handles the AArch64 generic physical timer and provides
 * delay primitives and low-level interrupt state management.
 */

#include "core/timer.h"

#include "stdio.h"

/*
 * read_cntfrq - Reads the system counter frequency register (CNTFRQ_EL0).
 */
static inline unsigned int read_cntfrq(void)
{
    unsigned int val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

/*
 * read_cntpct - Reads the physical counter register (CNTPCT_EL0).
 */
static inline unsigned long read_cntpct(void)
{
    unsigned long val;
    asm volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

/*
 * get_system_time - Converts raw counter ticks to milliseconds of uptime.
 */
unsigned long get_system_time(void)
{
    unsigned long freq = read_cntfrq();
    unsigned long count = read_cntpct();

    if (freq < 1000) {
        return 0;
    }

    return count / (freq / 1000);
}

/*
 * sleep_ms - Blocks the current core for a specific duration.
 */
void sleep_ms(unsigned long ms)
{
    unsigned long freq = read_cntfrq();
    if (freq == 0) {
        return;
    }

    unsigned long ticks_per_ms = freq / 1000;
    unsigned long current_count = read_cntpct();
    unsigned long target_count = current_count + (ms * ticks_per_ms);

    while (read_cntpct() < target_count) {
        asm volatile("yield");
    }
}

/*
 * enable_interrupts - Unmasks IRQs (DAIF bit 2).
 */
void enable_interrupts(void)
{
    asm volatile("msr daifclr, #2");
}

/*
 * disable_interrupts - Masks IRQs (DAIF bit 2).
 */
void disable_interrupts(void)
{
    asm volatile("msr daifset, #2");
}

/*
 * timer_interrupt_init - Configures the generic timer for 100Hz periodic ticks.
 */
void timer_interrupt_init(void)
{
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    core_id &= 3;

    /* Base address for RPi4 local interrupt routing (QA7) */
    unsigned long base_addr = 0xFFFFFF80FF800040 + (core_id * 4);
    volatile unsigned int *core_timer_irq_ctrl = (unsigned int *)base_addr;

    /* Route physical timer interrupts to this core */
    *core_timer_irq_ctrl = (1 << 1);

    unsigned int freq = read_cntfrq();

    /* Set TVAL to fire in 1/100th of a second (10ms) */
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq / 100));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1));

    if (core_id == 0) {
        pr_info("timer: generic timer: %u Hz, tick = 100 Hz (10ms)\n", freq);
    }
}

/*
 * timer_interrupt_reset - Reloads TVAL for the next periodic tick.
 */
void timer_interrupt_reset(void)
{
    unsigned int freq = read_cntfrq();
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq / 100));
}

/*
 * irq_save - Atomically reads and masks IRQs.
 */
unsigned long irq_save(void)
{
    unsigned long flags;
    asm volatile("mrs %0, daif" : "=r"(flags));
    asm volatile("msr daifset, #2");
    return flags;
}

/*
 * irq_restore - Restores DAIF to a previous state.
 */
void irq_restore(unsigned long flags)
{
    asm volatile("msr daif, %0" : : "r"(flags));
}
