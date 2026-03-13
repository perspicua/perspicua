#include "syscall.h"
#include "driver/uart.h"
#include "process.h"
#include "mmu.h"
#include "addr.h"
#include "heap.h"
#include "stdio.h"
#include "arch/uaccess.h"
#include "uapi/errors.h"
#include <uapi/syscalls.h>

static int validate_user_buffer(const void* ptr, size_t len, int writable)
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

        if (!(current_flags & MMU_AP_USER))
        {
            return 0;
        }

        if (writable && (current_flags & (1ULL << 7))) // AP[2] bit 7 is RO
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
    case SYS_WRITE: // sys_write(int fd, const char* buf, size_t len)
    {
        int fd = (int)(tf->x[0]);
        const char* buf = (const char*)(tf->x[1]);
        size_t len = (size_t)(tf->x[2]);

        if (!validate_user_buffer(buf, len, 0))
        {
            // TODO: maybe put better error codes here
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        char* kbuf = kmalloc(len);
        if (!kbuf)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        if (copy_from_user(kbuf, buf, len) != 0)
        {
            kfree(kbuf);

            // TODO: maybe put better error codes here
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        int bytes = vfs_write(fd, kbuf, len);
        kfree(kbuf);
        tf->x[0] = (uint64_t)bytes;
        break;
    }
    case SYS_EXIT: // sys_exit()
    {
        curr->state = TASK_DEAD;
        schedule();
        break;
    }
    case SYS_GETPID: // sys_getpid()
    {
        tf->x[0] = (uint64_t)pid;
        break;
    }
    case SYS_YIELD: // sys_yield()
    {
        schedule();
        break;
    }
    case SYS_SLEEP: // sys_sleep(ms)
    {
        unsigned long ms = tf->x[0];
        sched_sleep_ms(ms);
        break;
    }
    case SYS_OPEN: // sys_open(const char* path, int flags)
    {
        const char* path = (const char*)(tf->x[0]);
        int flags = (int)(tf->x[1]);

        size_t path_len = 0;
        const char* p = path;
        while (path_len < MAX_PATH_LEN)
        {
            unsigned char c;
            if (copy_from_user(&c, p++, 1) != 0)
            {
                // TODO: maybe put better error codes here
                path_len = (size_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }
            if (c == '\0')
                break;
            path_len++;
        }

        // TODO: maybe put better error codes here
        if (path_len == (size_t)-PERS_ERR_OUT_OF_MEMORY || path_len >= MAX_PATH_LEN)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        char* kpath = kmalloc(path_len + 1);
        copy_from_user(kpath, path, path_len + 1);

        int fd = vfs_open(kpath, flags);
        kfree(kpath);
        tf->x[0] = (uint64_t)fd;
        break;
    }
    case SYS_READ: // sys_read(int fd, char* buf, size_t len)
    {
        int fd = (int)(tf->x[0]);
        char* buf = (char*)(tf->x[1]);
        size_t len = (size_t)(tf->x[2]);

        if (!validate_user_buffer(buf, len, 1))
        {
            // TODO: maybe put better error codes here
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        char* kbuf = kmalloc(len);
        int bytes = vfs_read(fd, kbuf, len);
        if (bytes > 0)
        {
            copy_to_user(buf, kbuf, (size_t)bytes);
        }
        kfree(kbuf);
        tf->x[0] = (uint64_t)bytes;
        break;
    }
    case SYS_CLOSE: // sys_close(int fd)
    {
        int fd = (int)(tf->x[0]);
        tf->x[0] = (uint64_t)vfs_close(fd);
        break;
    }
    case SYS_EXEC: // sys_exec(const char* path)
    {
        const char* path = (const char*)(tf->x[0]);

        size_t path_len = 0;
        const char* p = path;
        while (path_len < MAX_PATH_LEN)
        {
            unsigned char c;
            if (copy_from_user(&c, p++, 1) != 0)
            {
                // TODO: maybe put better error codes here
                path_len = (size_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }
            if (c == '\0')
                break;
            path_len++;
        }

        // TODO: maybe put better error codes here
        if (path_len == (size_t)-PERS_ERR_OUT_OF_MEMORY || path_len >= MAX_PATH_LEN)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        char* kpath = kmalloc(path_len + 1);
        copy_from_user(kpath, path, path_len + 1);

        int res = process_exec(kpath);
        kfree(kpath);

        if (res < 0)
        {
            tf->x[0] = (uint64_t)-res;
        }
        break;
    }
    default:
    {
        printf("Unknown syscall: %lu\n", syscall_nr);
        break;
    }
    }
}
