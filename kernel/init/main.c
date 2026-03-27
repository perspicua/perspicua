/*
 * main.c - The kernel entry point and system initialization sequence.
 *
 * This file contains the primary 'main' function which orchestrates the
 * sequential bring-up of all kernel subsystems, memory management,
 * drivers, and the multi-core scheduler.
 */

#include "fs/procfs.h"
#include "types.h"
#include "mm/addr.h"
#include "core/lock.h"
#include "panic.h"
#include "stdio.h"
#include "string.h"

#include "mm/pmm.h"
#include "mm/mmu.h"
#include "mm/heap.h"
#include "mm/slab.h"
#include "core/timer.h"
#include "sched/sched.h"
#include "sched/process.h"
#include "fs/vfs.h"
#include "core/tty.h"

#include "core/initrd.h"
#include "fs/ramfs.h"
#include "fs/devfs.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/gic.h"
#include "driver/mailbox.h"
#include "driver/fb.h"
#include "driver/fb_console.h"
#include "driver/dashboard.h"
#include "driver/sd.h"
#include "driver/block.h"
#include "uapi/errors.h"

#include "devicetree/fdt.h"
#include "fs/fat32.h"
#include "test.h"

/* Kernel metadata and versioning */
#define KERNEL_VERSION "0.1"

/* Symbols defined in the linker script and assembly files */
extern void _entry(void);
extern struct tty console_tty;

/* Global synchronization for early boot messages */
// static spinlock_t console_lock = SPINLOCK_INIT; // Removed in favor of printk's internal lock

/*
 * smp_init - Brings up the secondary CPU cores by writing the kernel
 * entry point to their respective spin tables.
 */
static void smp_init(void)
{
    pr_info("smp: Bringing up secondary cores...\n");

    unsigned long entry_phys = V2P((unsigned long)_entry);

    /* BCM2711 ARM spin table addresses */
    volatile unsigned long *spin_cpu1 = (unsigned long *)P2V(0xE0);
    volatile unsigned long *spin_cpu2 = (unsigned long *)P2V(0xE8);
    volatile unsigned long *spin_cpu3 = (unsigned long *)P2V(0xF0);

    *spin_cpu1 = entry_phys;
    *spin_cpu2 = entry_phys;
    *spin_cpu3 = entry_phys;

    /* Ensure memory writes are visible before signaling event */
    asm volatile("dc cvac, %0" : : "r"(spin_cpu1));
    asm volatile("dc cvac, %0" : : "r"(spin_cpu2));
    asm volatile("dc cvac, %0" : : "r"(spin_cpu3));
    asm volatile("dsb sy");

    /* Send event to wake up parked cores */
    asm volatile("sev");

    /* Allow time for secondary cores to reach secondary_main */
    sleep_ms(200);
}

/*
 * secondary_main - Entry point for non-primary CPU cores.
 * Initializes per-core MMU, interrupt controller, and local timers.
 */
__attribute__((used)) void secondary_main(void)
{
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    core_id &= 3;

    mmu_secondary_init();
    gic_secondary_init();
    timer_interrupt_init();

    pr_info("smp: CPU%lu online\n", core_id);

    sched_secondary_init();

    /* Fallback loop: secondary cores should enter the scheduler */
    for (;;) {
        asm volatile("wfe");
    }
}

/*
 * print_banner - Displays the kernel version string.
 */
static void print_banner(void)
{
    pr_info("perspicua kernel v%s (" __DATE__ " " __TIME__ ")\n", KERNEL_VERSION);
}

/*
 * dashboard_task - Background task responsible for refreshing the
 * system status display.
 */
static void dashboard_task(void)
{
    while (1) {
        dashboard_update();
        sched_sleep_ms(100);
    }
}

/*
 * main - The primary kernel initialization routine.
 */
__attribute__((used)) int main(uintptr_t global_dtb_ptr)
{
    /* Stage 0: initialize devicetree parser*/
    fdt_init(global_dtb_ptr);

    /* Stage 1: Basic hardware and console bring-up */
    gpio_init();
    uart_init();
    mbox_init();
    fb_init();
    fb_console_init();
    tty_init(&console_tty);

    print_banner();

    /* Stage 2: Memory management initialization */
    fdt_parse_memory_reservations();

    pmm_init();
    mmu_init();

    // Re-base DTB pointers to virtual addresses post-MMU
    fdt_rebase(P2V(global_dtb_ptr));

    remap_framebuffer_pages();
    heap_init();

    /* Stage 3: Interrupts and scheduling */
    gic_init();
    uart_enable_interrupts();
    timer_interrupt_init();
    sched_init();

    /* Start the graphical dashboard update task */
    sched_create_task(dashboard_task);

    /* Stage 4: Multi-processing and Filesystem */
    smp_init();

    vfs_init();
    process_init();
    devfs_init();
    fb_register_device();
    sd_init();

    /* Initialize and mount FAT32 as the root filesystem */
    if (fat32_init("sd0") == PERS_SUCCESS) {
        vfs_mount("/", fat32_get_root_node());
    }

    /* Mount devfs over the FAT32 root */
    vfs_mount("/dev", devfs_get_root());

    /* Initialize and mount procfs */
    procfs_init();

    run_all_tests();
    enable_interrupts();
    run_scheduler_tests();

    /* Load and execute the primary user-space application from the SD card */
    if (process_create_from_file("/init.elf", 1) != 0) {
        pr_err("init: Failed to load /init.elf\n");
    }

    /* The main thread remains parked while the scheduler handles execution */
    while (1) {
        asm volatile("wfe");
    }

    return 0;
}
