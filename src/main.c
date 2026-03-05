#include "driver/uart.h"
#include "driver/gic.h"

#include "kernel/pmm.h"
#include "kernel/mmu.h"
#include "kernel/heap.h"
#include "kernel/sched.h"
#include "kernel/timer.h"
#include "kernel/lock.h"

#include "lib/panic.h"
#include "lib/stdio.h"
#include "lib/string.h"

#include "test/test.h"
#include "kernel/addr.h"

extern void _entry(void);

static spinlock_t console_lock = SPINLOCK_INIT;

#define KERNEL_VERSION "0.1"

static inline unsigned long get_core_id(void)
{
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    return core_id & 3;
}

void smp_init(void)
{
    printf("\n");
    printf("[  SMP ] Bringing up secondary cores...\n");

    unsigned long entry_phys = V2P((unsigned long)_entry);

    volatile unsigned long* spin_cpu1 = (unsigned long*)P2V(0xE0);
    volatile unsigned long* spin_cpu2 = (unsigned long*)P2V(0xE8);
    volatile unsigned long* spin_cpu3 = (unsigned long*)P2V(0xF0);

    *spin_cpu1 = entry_phys;
    *spin_cpu2 = entry_phys;
    *spin_cpu3 = entry_phys;

    // wake up parked cores
    asm volatile("sev");

    // wait for secondary cores to finish init before main continues
    sleep_ms(200);
}

void secondary_main(void)
{
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    core_id &= 3;

    mmu_secondary_init();
    gic_secondary_init();
    timer_interrupt_init();

    unsigned long flags = spin_lock_irqsave(&console_lock);
    printf("[  SMP ] CPU%lu online - MMU active, GIC configured, timer armed\n", core_id);
    spin_unlock_irqrestore(&console_lock, flags);

    sched_secondary_init();
}

static void print_banner(void)
{
    printf("\n");
    printf("  _ __   ___ _ __ ___ _ __ (_) ___ _   _  __ _\n");
    printf(" | '_ \\ / _ \\ '__/ __| '_ \\| |/ __| | | |/ _` |\n");
    printf(" | |_) |  __/ |  \\__ \\ |_) | | (__| |_| | (_| |\n");
    printf(" | .__/ \\___|_|  |___/ .__/|_|\\___|\\__,_|\\__,_| v%s\n", KERNEL_VERSION);
    printf(" |_|                 |_|\n");
    printf("\n");
}

int main()
{
    uart_init();

    print_banner();
    printf("[  0.000] BOOT: perspicua kernel, built " __DATE__ " " __TIME__ " version %s\n", KERNEL_VERSION);
    printf("[  0.000] BOOT: EL1 entry at 0x%lx (higher-half VMA 0x%lx)\n", V2P((unsigned long)main),
           (unsigned long)main);
    printf("[  0.000] BOOT: Board: Raspberry Pi 4B (BCM2711, Cortex-A72 x4)\n");
    printf("[  0.000] BOOT: Architecture: AArch64, 39-bit VA, 4KB granule\n");
    printf("[  0.000] BOOT: Kernel VMA base: 0x%lx\n", KERNEL_VMA);

    uart_enable_interrupts();
    printf("[  0.000] UART: PL011 @ 0xFE201000, 115200 8N1, FIFO enabled\n");

    pmm_init();
    mmu_init();
    heap_init();
    gic_init();
    timer_interrupt_init();
    sched_init();

    smp_init();

    printf("\n");
    printf(" BOOT COMPLETE - all subsystems operational\n");

    run_all_tests();

    enable_interrupts();
    run_scheduler_tests();

    while (1)
        asm volatile("wfe");

    return 0;
}
