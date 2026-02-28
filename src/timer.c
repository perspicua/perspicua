#include "timer.h"
#include "stdio.h"

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
    unsigned int freq = read_cntfrq();
    unsigned int count = read_cntpct();

    if (freq == 0)
        return 0;

    return (count) / (freq / 1000);
}

void sleep_ms(unsigned int ms)
{
    unsigned int freq = read_cntfrq();
    if (freq == 0)
        return;

    unsigned int ticks_per_ms = freq / 1000;

    unsigned int current_count = read_cntpct();
    unsigned int target_count = current_count + (ms * ticks_per_ms);

    while (read_cntpct() < target_count)
        asm volatile("yield");
}

void enable_interrupts(void)
{
    asm volatile("msr daifclr, #2");
}

void timer_interrupt_init(void)
{
    volatile unsigned int* core0_timer_irq_ctrl = (unsigned int*)0xFF800040;
    *core0_timer_irq_ctrl = (1 << 1);

    unsigned int freq = read_cntfrq();

    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1));
}

void timer_interrupt_reset(void)
{
    unsigned int freq = read_cntfrq();
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq));
}
