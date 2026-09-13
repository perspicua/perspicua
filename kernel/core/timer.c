/*
 * timer.c - Driver for the ARM Generic Timer and system ticks.
 */

#include "core/timer.h"

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

void timer_interrupt_reset(void)
{
    unsigned int freq = read_cntfrq();
    asm volatile("msr cntp_tval_el0, %0" : : "r"(freq / 100));
}
