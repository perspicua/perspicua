#include "timer.h"

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

    return (count) / (freq / 1000);
}

void sleep_ms(unsigned int ms)
{
    unsigned int freq = read_cntfrq();
    unsigned int ticks_per_ms = freq / 1000;

    unsigned int current_count = read_cntpct();
    unsigned int target_count = current_count + (ms * ticks_per_ms);

    while (read_cntpct() < target_count)
        asm volatile("yield");
}
