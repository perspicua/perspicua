/*
 * timer.c - Driver for the ARM Generic Timer and system ticks.
 */

#include "core/timer.h"

#include "stdio.h"

#include "arch/cpu.h"
#include "arch/irq.h"
#include "core/lock.h"
#include "driver/gic.h"

static inline unsigned int read_cntfrq(void)
{
    unsigned int val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

static inline unsigned long read_cntpct(void)
{
    unsigned long val;
    asm volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

unsigned long get_system_time(void)
{
    unsigned long freq = read_cntfrq();
    unsigned long count = read_cntpct();

    if (freq < 1000) {
        return 0;
    }

    return count / (freq / 1000);
}

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

static irq_result_t timer_irq_handler(void *ctx)
{
    (void)ctx;
    timer_interrupt_reset();

    if (preempt_active()) {
        return IRQ_HANDLED;
    }
    return IRQ_HANDLED_RESCHED;
}

void timer_interrupt_init(void)
{
    int core = cpu_id();

    // Base address for RPi4 local interrupt routing (QA7)
    unsigned long base_addr = 0xFFFFFF80FF800040 + ((unsigned long)core * 4);
    volatile unsigned int *core_timer_irq_ctrl = (unsigned int *)base_addr;

    // Route physical timer interrupts to this core
    *core_timer_irq_ctrl = (1 << 1);

    unsigned int freq = read_cntfrq();

    // Set TVAL to fire in 1/100th of a second (10ms)
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq / 100));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1));

    if (core == 0) {
        request_irq(GIC_TIMER_IRQ, timer_irq_handler, NULL, "timer");
        pr_info("timer: generic timer: %u Hz, tick = 100 Hz (10ms)\n", freq);
    }
}

void timer_interrupt_reset(void)
{
    unsigned int freq = read_cntfrq();
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq / 100));
}
