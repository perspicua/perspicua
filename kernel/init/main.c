/*
 * kernel/init/main.c - Kernel entry point and system initialization sequence.
 */

#include "uapi/errors.h"

#include "types.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"

#include "mm/addr.h"
#include "mm/pmm.h"
#include "mm/mmu.h"
#include "mm/heap.h"
#include "mm/slab.h"

#include "core/lock.h"
#include "core/timer.h"
#include "core/tty.h"
#include "core/initrd.h"

#include "sched/sched.h"
#include "sched/process.h"

#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "fs/devfs.h"
#include "fs/procfs.h"
#include "fs/fat32.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/gic.h"
#include "driver/mailbox.h"
#include "driver/fb.h"
#include "driver/fb_console.h"
#include "driver/dashboard.h"
#include "driver/sd.h"
#include "driver/block.h"

#include "devicetree/fdt.h"
#include "test.h"

/* Kernel metadata and versioning */
#define KERNEL_VERSION "0.1"

/* Symbols defined in the linker script and assembly files */
extern void _entry(void);
extern struct tty console_tty;

/**
 * smp_init - Brings up secondary CPU cores.
 *
 * Writes the kernel entry point to the BCM2711 ARM spin tables and signals
 * an event to wake parked cores.
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

    /* Wake up parked cores */
    asm volatile("sev");

    /* Allow time for secondary cores to reach secondary_main */
    sleep_ms(200);
}

/**
 * secondary_main - Entry point for non-primary CPU cores.
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

    /* Secondary cores enter the scheduler or wait for events */
    for (;;) {
        asm volatile("wfe");
    }
}

/**
 * print_banner - Displays the kernel version string.
 */
static void print_banner(void)
{
    pr_info("perspicua kernel v%s (" __DATE__ " " __TIME__ ")\n", KERNEL_VERSION);
}

/**
 * dashboard_task - Background task for system status display.
 */
static void dashboard_task(void)
{
    while (1) {
        dashboard_update();
        sched_sleep_ms(100);
    }
}

/**
 * main - Primary kernel initialization routine.
 */
__attribute__((used)) int main(uintptr_t global_dtb_ptr)
{
    /* Stage 0: Devicetree parser initialization */
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

    /* Re-base DTB pointers to virtual addresses post-MMU */
    fdt_rebase(P2V(global_dtb_ptr));

    remap_framebuffer_pages();
    heap_init();

    /* Stage 3: Interrupts and scheduling */
    gic_init();
    uart_enable_interrupts();
    timer_interrupt_init();
    sched_init();

    /* Start background maintenance tasks */
    sched_create_task(dashboard_task);

    /* Stage 4: Multi-processing and Filesystems */
    smp_init();

    vfs_init();
    process_init();
    devfs_init();
    fb_register_device();
    sd_init();

    /* Root filesystem initialization (FAT32) */
    if (fat32_init("sd0") == PERS_SUCCESS) {
        vfs_mount("/", fat32_get_root_node());
    }

    /* Mount auxiliary filesystems */
    vfs_mount("/dev", devfs_get_root());
    procfs_init();

    enable_interrupts();

    /* Launch primary user-space application */
    if (process_create_from_file("/bin/init.elf", 1) != 0) {
        pr_err("init: Failed to load init\n");
    }

    /* Main thread parks while scheduler handles execution */
    while (1) {
        asm volatile("wfe");
    }

    return 0;
}
