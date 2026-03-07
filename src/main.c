#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/gic.h"

#include "kernel/pmm.h"
#include "kernel/mmu.h"
#include "kernel/heap.h"
#include "kernel/sched.h"
#include "kernel/timer.h"
#include "kernel/lock.h"
#include "kernel/process.h"

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

static void smp_init(void)
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
__attribute__((used)) void secondary_main(void);
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

uint8_t user_stack[4096] __attribute__((aligned(16)));

static inline __attribute__((always_inline)) void sys_write(const char* buf, size_t len)
{
    register const char* _buf asm("x0") = buf;
    register size_t _len asm("x1") = len;
    asm volatile("mov x8, #1\n svc #0" : : "r"(_buf), "r"(_len) : "x8");
}

static inline __attribute__((always_inline)) void sys_exit(void)
{
    asm volatile("mov x8, #2 \n svc #0" : : : "x8");
}

static void user_program_1(void)
{
    while (1)
    {
        char msg[25];
        msg[0] = 'H';
        msg[1] = 'e';
        msg[2] = 'l';
        msg[3] = 'l';
        msg[4] = 'o';
        msg[5] = ' ';
        msg[6] = 'f';
        msg[7] = 'r';
        msg[8] = 'o';
        msg[9] = 'm';
        msg[10] = ' ';
        msg[11] = 'u';
        msg[12] = 's';
        msg[13] = 'e';
        msg[14] = 'r';
        msg[15] = ' ';
        msg[16] = 's';
        msg[17] = 'p';
        msg[18] = 'a';
        msg[19] = 'c';
        msg[20] = 'e';
        msg[21] = '!';
        msg[22] = '\n';
        sys_write(msg, 23);
        for (volatile int i = 0; i < 5000000; i++)
            ;
    }
}

__attribute__((used)) int main(void);
int main()
{
    gpio_init();
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
    // run_scheduler_tests();

    process_init();
    process_create((void*)user_program_1, 1024, 1);
    /* process_create((void*)user_program_2, 1024, 2); */
    /* process_create((void*)user_program_3, 1024, 3); */
    /* process_create((void*)user_program_4, 1024, 4); */
    /* process_create((void*)user_program_5, 1024, 5); */
    // struct cpu_context dummy;
    // switch_context(&dummy, &current_process.context);

    while (1)
        asm volatile("wfe");

    return 0;
}
