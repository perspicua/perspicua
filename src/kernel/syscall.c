#include "kernel/syscall.h"
#include "driver/uart.h"
#include "kernel/process.h"
#include "kernel/mmu.h"
#include "kernel/addr.h"
#include "kernel/heap.h"
#include "lib/stdio.h"
#include "arch/uaccess.h"

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
        unsigned long current_flags;
        if (!mmu_user_query(pgd, curr, NULL, &current_flags))
        {
            return 0;
        }

        if (!(current_flags & MMU_AP_USER)) // MMU_AP_USER implies PTE_USER_READ
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
    struct task* curr = sched_get_current();
    uint32_t pid = curr->pid;

    switch (syscall_nr)
    {
    case 1: // sys_write(int fd, const char* buf, size_t len)
    {
        int fd = (int)(tf->x[0]);
        const char* buf = (const char*)(tf->x[1]);
        size_t len = (size_t)(tf->x[2]);

        if (!validate_user_buffer(buf, len))
        {
            tf->x[0] = (uint64_t)-1; // -EFAULT
            break;
        }

        char* kbuf = kmalloc(len);
        if (!kbuf)
        {
            tf->x[0] = (uint64_t)-1; // -ENOMEM
            break;
        }

        if (copy_from_user(kbuf, buf, len) != 0)
        {
            kfree(kbuf);
            tf->x[0] = (uint64_t)-1; // -EFAULT
            break;
        }

        int bytes = vfs_write(fd, kbuf, len);
        kfree(kbuf);
        tf->x[0] = (uint64_t)bytes;
        break;
    }
    case 2: // sys_exit()
    {
        curr->state = TASK_DEAD;
        schedule();
        break;
    }
    case 3: // sys_getpid()
    {
        tf->x[0] = (uint64_t)pid;
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
