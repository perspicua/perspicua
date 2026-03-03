#include "timer.h"
#include "../lib/stdio.h"

// read timer frequency
static inline unsigned int read_cntfrq(void)
{
    unsigned int val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

// read current physical counter value
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

    if (freq == 0)
        return 0;

    return count / (freq / 1000);
}

void sleep_ms(unsigned long ms)
{
    unsigned long freq = read_cntfrq();
    if (freq == 0)
        return;

    unsigned long ticks_per_ms = freq / 1000;

    unsigned long current_count = read_cntpct();
    unsigned long target_count = current_count + (ms * ticks_per_ms);

    while (read_cntpct() < target_count)
        asm volatile("yield");
}

void enable_interrupts(void)
{
    asm volatile("msr daifclr, #2");
}

void disable_interrupts(void)
{
    asm volatile("msr daifset, #2");
}

void timer_interrupt_init(void)
{
    volatile unsigned int* core0_timer_irq_ctrl = (unsigned int*)0xFFFFFF80FF800040;
    *core0_timer_irq_ctrl = (1 << 1);

    unsigned int freq = read_cntfrq();

    // tick 100 times per second for scheduling
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq / 100));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1));
}

void timer_interrupt_reset(void)
{
    unsigned int freq = read_cntfrq();
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq / 100));
}

unsigned long irq_save(void)
{
    unsigned long flags;
    asm volatile("mrs %0, daif" : "=r"(flags));
    asm volatile("msr daifset, #2");
    return flags;
}

void irq_restore(unsigned long flags)
{
    asm volatile("msr daif, %0" : : "r"(flags));
}
