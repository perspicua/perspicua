#include "kernel/syscall.h"
#include "driver/uart.h"
#include "kernel/process.h"
#include "kernel/mmu.h"
#include "kernel/addr.h"
#include "lib/stdio.h"

static int validate_user_buffer(const void* ptr, size_t len)
{
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + len;

    if (end < start || end > KERNEL_VMA)
    {
        return 0;
    }

    int pid = process_find_current();
    if (pid < 0)
    {
        return 0;
    }
    unsigned long* pgd = process_table[pid].user_pgd;

    uintptr_t curr = start & ~0xFFFUL;
    while (curr < end)
    {
        if (!mmu_user_query(pgd, curr, NULL, NULL))
        {
            return 0;
        }
        curr += 4096;
    }

    return 1;
}

void handle_syscall(struct trap_frame* tf)
{
    uint64_t syscall_nr = tf->x[8];

    switch (syscall_nr)
    {
    case 1: // sys_write(const char* buf, size_t len)
    {
        const char* buf = (const char*)(tf->x[0]);
        size_t len = (size_t)(tf->x[1]);

        if (!validate_user_buffer(buf, len))
        {
            printf("[SYSCALL] PID %d passed invalid buffer to sys_write! Killing process.\n", process_find_current());
            process_exit();
            while (1)
                asm volatile("wfe");
        }

        uart_write(buf, len);
        break;
    }
    case 2: // sys_exit()
    {
        process_exit();
        while (1)
            asm volatile("wfe");
        break;
    }
    case 3: // sys_getpid()
    {
        tf->x[0] = (uint64_t)process_find_current();
        break;
    }
    case 4: // sys_yield()
    {
        schedule();
        break;
    }
    case 5: // sys_sleep(ms)
    {
        unsigned long ms = tf->x[0];
        sched_sleep_ms(ms);
        break;
    }
    default:
    {
        printf("Unknown syscall: %lu\n", syscall_nr);
        break;
    }
    }
}
