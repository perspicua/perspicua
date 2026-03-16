/*
 * main.c - The kernel entry point and system initialization sequence.
 *
 * This file contains the primary 'main' function which orchestrates the
 * sequential bring-up of all kernel subsystems, memory management,
 * drivers, and the multi-core scheduler.
 */

#include "types.h"
#include "addr.h"
#include "lock.h"
#include "panic.h"
#include "stdio.h"
#include "string.h"

#include "pmm.h"
#include "mmu.h"
#include "heap.h"
#include "slab.h"
#include "timer.h"
#include "sched.h"
#include "process.h"
#include "vfs.h"
#include "tty.h"

#include "initrd.h"
#include "ramfs.h"
#include "devfs.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/gic.h"
#include "driver/mailbox.h"
#include "driver/fb.h"
#include "driver/fb_console.h"
#include "driver/dashboard.h"
#include "driver/sd.h"
#include "driver/block.h"
#include "heap.h"
#include "uapi/errors.h"

#include "devicetree/fdt.h"

#include "test.h"

/* Kernel metadata and versioning */
#define KERNEL_VERSION "0.1"

/* Symbols defined in the linker script and assembly files */
extern void _entry(void);
extern struct tty console_tty;

/* Global synchronization for early boot messages */
static spinlock_t console_lock = SPINLOCK_INIT;

/*
 * smp_init - Brings up the secondary CPU cores by writing the kernel
 * entry point to their respective spin tables.
 */
static void smp_init(void)
{
    printf("\n");
    printf("[  SMP ] Bringing up secondary cores...\n");

    unsigned long entry_phys = V2P((unsigned long)_entry);

    /* BCM2711 ARM spin table addresses */
    volatile unsigned long* spin_cpu1 = (unsigned long*)P2V(0xE0);
    volatile unsigned long* spin_cpu2 = (unsigned long*)P2V(0xE8);
    volatile unsigned long* spin_cpu3 = (unsigned long*)P2V(0xF0);

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

    unsigned long flags = spin_lock_irqsave(&console_lock);
    printf("[  SMP ] CPU%lu online - MMU active, GIC configured, timer armed\n", core_id);
    spin_unlock_irqrestore(&console_lock, flags);

    sched_secondary_init();

    /* Fallback loop: secondary cores should enter the scheduler */
    for (;;)
    {
        asm volatile("wfe");
    }
}

/*
 * print_banner - Displays the kernel ASCII logo and version string.
 */
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

/*
 * dashboard_task - Background task responsible for refreshing the
 * system status display.
 */
static void dashboard_task(void)
{
    while (1)
    {
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
    printf("[  DTB ] Initializing Flattened Device Tree parser...\n");
    fdt_init(global_dtb_ptr);
    printf("[  DTB ] DTB parsed successfully, root node: \n");
    /* Stage 1: Basic hardware and console bring-up */
    gpio_init();
    uart_init();
    mbox_init();
    fb_init();
    fb_console_init();
    tty_init(&console_tty);

    print_banner();
    printf("[  0.000] BOOT: perspicua kernel v%s, built " __DATE__ " " __TIME__ "\n", KERNEL_VERSION);
    printf("[  0.000] BOOT: Board: Raspberry Pi 4B (BCM2711, Cortex-A72 x4)\n");
    printf("[  0.000] BOOT: Architecture: AArch64, 39-bit VA, 4KB granule\n");

    /* Stage 2: Memory management initialization */
    // fdt_parse_memory_reservations(); // already called if needed
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

    /* Stage 4: Multi-processing and testing */
    smp_init();
    printf("\n BOOT COMPLETE - all subsystems operational\n");

    /* Stage 5: Filesystem and User-space bring-up */
    vfs_init();
    process_init();
    ramfs_init();
    devfs_init();
    vfs_mount("/dev", devfs_get_root());
    sd_init();

    /* Load the Initial RAM Disk from the SD card */
    struct block_device* sd_dev = block_device_lookup("sd0");
    if (sd_dev)
    {
        // Allocate a 2MB buffer for the initrd
        size_t initrd_max_size = 2 * 1024 * 1024;
        void* initrd_buf = heap_malloc(initrd_max_size);
        if (initrd_buf)
        {
            size_t blocks_to_read = initrd_max_size / sd_dev->block_size;
            if (sd_dev->read_blocks(sd_dev, initrd_buf, 0, blocks_to_read) == PERS_SUCCESS)
            {
                initrd_init(initrd_buf);
            }
            else
            {
                printf("[  BOOT ] Error: Failed to read initrd from SD card\n");
            }
        }
        else
        {
            printf("[  BOOT ] Error: Failed to allocate memory for initrd\n");
        }
    }
    else
    {
        printf("[  BOOT ] Error: SD card device not found\n");
    }

    run_all_tests();
    enable_interrupts();
    run_scheduler_tests();

    /* Demonstration of VFS functionality */
    int fd = vfs_open("/hello.txt", VFS_O_RDONLY);
    if (fd >= 0)
    {
        char buf[100];
        int bytes = vfs_read(fd, buf, sizeof(buf) - 1);
        if (bytes >= 0)
        {
            buf[bytes] = '\0';
            printf("Read from VFS: %s", buf);
        }
        else
        {
            printf("[  VFS ] Error: failed to read /hello.txt\n");
        }
        vfs_close(fd);
    }
    else
    {
        printf("[  VFS ] Error: could not open /hello.txt\n");
    }

    /* Load and execute the primary user-space application */
    if (process_create_from_file("/init.elf", 1) != 0)
    {
        printf("[  ELF ] Error: failed to load /init.elf\n");
    }

    /* The main thread remains parked while the scheduler handles execution */
    while (1)
    {
        asm volatile("wfe");
    }

    return 0;
}
