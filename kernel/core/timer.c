/*
 * timer.c - Driver for the ARM Generic Timer and system ticks.
 */

#include "core/timer.h"

#include <stddef.h>
#include <stdint.h>

#include "stdio.h"

#include "arch/cpu.h"
#include "arch/irq.h"
#include "core/lock.h"
#include "panic.h"
#include "devicetree/fdt.h"
#include "driver/device.h"
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

unsigned long timer_get_system_time(void)
{
    unsigned long freq = read_cntfrq();
    unsigned long count = read_cntpct();

    if (freq < 1000) {
        return 0;
    }

    return count / (freq / 1000);
}

void timer_sleep_ms(unsigned long ms)
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
 * Offset of the "Core n timers interrupt control" registers within the QA7
 * block, one 32-bit register per core.
 */
#define QA7_CORE_TIMER_IRQCNTL 0x40

// Resolved once by the boot core; the secondaries only index into it.
static volatile unsigned int *qa7_timer_irq_ctrl = NULL;

static void qa7_timer_routing_init(void)
{
    const uint32_t *node = fdt_find_node_by_compatible("brcm,bcm2836-l1-intc");
    if (!node) {
        PANIC("timer: no ARM-local interrupt controller in the devicetree");
    }

    struct device dev = {
        .name = "arm-local-intc",
        .fdt_node = node,
        .priv = NULL,
    };

    uintptr_t vbase = devm_get_io_base(&dev, 0);
    if (!vbase) {
        PANIC("timer: ARM-local interrupt controller has no usable 'reg'");
    }

    qa7_timer_irq_ctrl = (volatile unsigned int *)(vbase + QA7_CORE_TIMER_IRQCNTL);
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

    if (core == 0) {
        qa7_timer_routing_init();
    }

    // Route physical timer interrupts to this core
    qa7_timer_irq_ctrl[core] = (1 << 1);

    unsigned int freq = read_cntfrq();

    // Set TVAL to fire in 1/100th of a second (10ms)
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq / 100));
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(1));

    if (core == 0) {
        if (request_irq(GIC_TIMER_IRQ, timer_irq_handler, NULL, "timer") != 0) {
            PANIC("timer: the timer interrupt line is already claimed");
        }
        pr_info("timer: generic timer: %u Hz, tick = 100 Hz (10ms)\n", freq);
    }
}

/*
 * timer_get_wall_time - Seconds since the Unix epoch, approximately.
 *
 * A Pi 4 has no battery-backed clock, so there is nothing to read the real
 * date from. This is the time the kernel was built plus how long it has been
 * running: wrong by however long the image sat before booting, but in the
 * right year and ordered correctly, which is what a file timestamp is for.
 * When an RTC or a network time source exists, only this base changes.
 */
static int build_month(const char *m)
{
    static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (int i = 0; i < 12; i++) {
        if (m[0] == names[i * 3] && m[1] == names[i * 3 + 1] && m[2] == names[i * 3 + 2]) {
            return i + 1;
        }
    }
    return 1;
}

static int digits2(const char *p)
{
    int hi = (p[0] == ' ') ? 0 : p[0] - '0';
    return hi * 10 + (p[1] - '0');
}

// Days from 1970-01-01 to the first of the given month, proleptic Gregorian.
static int64_t days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t yoe = year - era * 400;
    int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

time_t timer_civil_to_epoch(int year, int month, int day, int hour, int min, int sec)
{
    return (time_t)(days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec);
}

time_t timer_get_wall_time(void)
{
    static const char date[] = __DATE__;  // "Mmm dd yyyy"
    static const char clock[] = __TIME__; // "hh:mm:ss"

    int year =
        (date[7] - '0') * 1000 + (date[8] - '0') * 100 + (date[9] - '0') * 10 + (date[10] - '0');
    int64_t days = days_from_civil(year, build_month(date), digits2(&date[4]));
    int64_t base =
        days * 86400 + digits2(&clock[0]) * 3600 + digits2(&clock[3]) * 60 + digits2(&clock[6]);

    return (time_t)(base + (int64_t)(timer_get_system_time() / 1000));
}

void timer_interrupt_reset(void)
{
    unsigned int freq = read_cntfrq();
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq / 100));
}
