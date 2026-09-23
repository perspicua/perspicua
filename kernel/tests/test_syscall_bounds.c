/*
 * test_syscall_bounds.c - Syscall dispatch driven with arguments no caller sends.
 *
 * A bad pointer is refused twice over, by the range checks and by the
 * unprivileged loads and stores the copy helpers use, so one layer regressing
 * alone does not fail a probe. What does is the kernel acting on a pointer it
 * should have refused, which the kernel-address probe makes visible by
 * pointing at the path of a file that exists.
 */

#include <stddef.h>
#include <stdint.h>

#include "test.h"

#include "string.h"

#include "uapi/errno.h"
#include "uapi/fcntl.h"
#include "uapi/signals.h"
#include "uapi/syscalls.h"

#include "core/syscall.h"
#include "fs/vfs.h"
#include "mm/addr.h"
#include "mm/mmu.h"
#include "sched/process.h"

// Scratch user addresses, mapped into the borrowed pgd below.
#define USER_RW_VA 0x0000000050000000UL
#define USER_RO_VA 0x0000000050001000UL

#define USER_UNMAPPED_VA 0x0000000040000000UL

// Highest number the dispatcher has a slot for.
#define SYSCALL_MAX_NR SYS_SIGALTSTACK

#define SCRATCH_PATH "/sbounds.tmp"

// A kernel address a syscall must never accept as a user buffer.
static char kernel_target[64];

struct ptr_probe {
    const char *name;
    uint64_t nr;
    int ptr_arg;
    int null_ok; // POSIX reads an absent pointer as "no result wanted"
    uint64_t args[6];
};

// Paths and out-parameters only: nothing here is gated behind an fd lookup,
// so the pointer is what decides the result.
static const struct ptr_probe ptr_probes[] = {
    {"open", SYS_OPEN, 0, 0, {0, O_RDONLY, 0, 0, 0, 0}},
    {"chdir", SYS_CHDIR, 0, 0, {0, 0, 0, 0, 0, 0}},
    {"mkdir", SYS_MKDIR, 0, 0, {0, 0, 0, 0, 0, 0}},
    {"rmdir", SYS_RMDIR, 0, 0, {0, 0, 0, 0, 0, 0}},
    {"unlink", SYS_UNLINK, 0, 0, {0, 0, 0, 0, 0, 0}},
    {"truncate", SYS_TRUNCATE, 0, 0, {0, 0, 0, 0, 0, 0}},
    {"stat path", SYS_STAT, 0, 0, {0, USER_RW_VA, 0, 0, 0, 0}},
    {"rename old", SYS_RENAME, 0, 0, {0, USER_RW_VA, 0, 0, 0, 0}},
    {"rename new", SYS_RENAME, 1, 0, {USER_RW_VA, 0, 0, 0, 0, 0}},
    {"getcwd", SYS_GETCWD, 0, 0, {0, 64, 0, 0, 0, 0}},
    {"gettimeofday", SYS_GETTIMEOFDAY, 0, 0, {0, 0, 0, 0, 0, 0}},
    {"clock_gettime", SYS_CLOCK_GETTIME, 1, 0, {0, 0, 0, 0, 0, 0}},
    {"sigpending", SYS_SIGPENDING, 0, 0, {0, 0, 0, 0, 0, 0}},
    {"sigaction", SYS_SIGACTION, 1, 1, {SIGUSR1, 0, 0, 0, 0, 0}},
    {"sigprocmask", SYS_SIGPROCMASK, 1, 1, {0, 0, 0, 0, 0, 0}},
    {"sigaltstack", SYS_SIGALTSTACK, 0, 1, {0, 0, 0, 0, 0, 0}},
};

// The subset above that the kernel writes through, so a read-only mapping has
// to be refused as well as an absent one.
static const struct ptr_probe write_probes[] = {
    {"getcwd", SYS_GETCWD, 0, 0, {0, 64, 0, 0, 0, 0}},
    {"gettimeofday", SYS_GETTIMEOFDAY, 0, 0, {0, 0, 0, 0, 0, 0}},
    {"clock_gettime", SYS_CLOCK_GETTIME, 1, 0, {0, 0, 0, 0, 0, 0}},
    {"sigpending", SYS_SIGPENDING, 0, 0, {0, 0, 0, 0, 0, 0}},
};

// Numbers the suite must not drive, and why. This list is the one place a
// syscall is excused from the sweep, so a reason belongs with every entry.
struct skipped_syscall {
    uint64_t nr;
    const char *reason;
};

static const struct skipped_syscall skipped[] = {
    {SYS_EXIT, "terminates the caller"},
    {SYS_FORK, "would fork the boot task"},
    {SYS_EXEC, "would replace the running image"},
    {SYS_SIGRETURN, "pops a signal frame that was never pushed"},
    {SYS_SIGSUSPEND, "blocks until a signal arrives"},
    {SYS_NANOSLEEP, "sleeps for whatever the argument says"},
    {SYS_WAITPID, "blocks on a child the boot task does not have"},
    {SYS_YIELD, "schedules away mid-suite"},
    {SYS_KILL, "signals whatever process the argument names"},
    {SYS_PIPE, "leaks a pipe and two fds into the kernel slot"},
    {SYS_MMAP, "maps into the kernel slot's address space on success"},
    {SYS_SYNC, "flushes the volume the rest of the suite runs on"},
    {SYS_FSYNC, "flushes the volume the rest of the suite runs on"},
    {SYS_SETSID, "takes no argument to fail on and would resession slot 0"},
    {SYS_SETPGID, "would move slot 0 into another process group"},
    {SYS_TCSETPGRP, "would hand the terminal to whatever group the argument names"},
};

// Pointer values a syscall must refuse whatever else it is handed.
static const uint64_t bad_ptrs[] = {
    0,                     // NULL
    1,                     // too low to be a mapping
    KERNEL_VMA,            // the boundary itself
    KERNEL_VMA + 0x1000,   // inside the kernel map
    USER_VA_LIMIT - 8,     // straddles the top of the user range
    USER_UNMAPPED_VA,      // well formed, no translation
    0xFFFFFFFFFFFFFFF0ULL, // wraps once a length is added
};

struct len_probe {
    const char *name;
    uint64_t nr;
    int on_dir;
    uint64_t unit; // smallest non-zero length the call serves
};

static const struct len_probe len_probes[] = {
    {"read", SYS_READ, 0, 1},
    {"write", SYS_WRITE, 0, 1},
    {"getdents", SYS_GETDENTS, 1, sizeof(struct dirent)},
    {"pread", SYS_PREAD, 0, 1},
    {"pwrite", SYS_PWRITE, 0, 1},
};

// Past the cap the dispatcher enforces, or wrapped: these must be refused.
static const uint64_t illegal_lens[] = {
    SYSCALL_MAX_RW_SIZE + 1,
    (uint64_t)-1,
    0x8000000000000000ULL,
};

static int is_skipped(uint64_t nr)
{
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++) {
        if (skipped[i].nr == nr) {
            return 1;
        }
    }
    return 0;
}

// Slot 0 starts with no cwd, and getcwd fails on that before its pointer.
static void drop_cwd(void)
{
    struct process *kernel = process_table[0];

    unsigned long flags = spin_lock_irqsave(&kernel->fd_lock);
    struct vfs_vnode *cwd = kernel->cwd;
    kernel->cwd = NULL;
    spin_unlock_irqrestore(&kernel->fd_lock, flags);

    if (cwd) {
        vfs_vnode_put(cwd);
    }
}

void test_syscall_bounds(void)
{
    TEST_SUITE_BEGIN("Syscall Bounds");

    // a number with no handler must be reported, never dispatched
    {
        uint64_t args[6] = {0};

        TEST_ASSERT_EQ("syscall 0 refused", test_syscall(0, args), -ENOSYS);
        TEST_ASSERT_EQ("gap in the table refused", test_syscall(5, args), -ENOSYS);
        TEST_ASSERT_EQ("one past the table refused", test_syscall(SYSCALL_MAX_NR + 1, args),
                       -ENOSYS);
        TEST_ASSERT_EQ("far out of range refused", test_syscall(4096, args), -ENOSYS);
        TEST_ASSERT_EQ("a number that indexes nothing refused", test_syscall((uint64_t)-1, args),
                       -ENOSYS);
    }

    // a probe that drives an excluded syscall would defeat the skip list
    {
        int conflict = 0;

        for (size_t i = 0; i < sizeof(ptr_probes) / sizeof(ptr_probes[0]); i++) {
            if (is_skipped(ptr_probes[i].nr)) {
                pr_err("test: %s is both driven and skipped [FAILED]\n", ptr_probes[i].name);
                conflict = 1;
            }
        }

        TEST_ASSERT("no probe drives a syscall the skip list excludes", !conflict);
    }

    unsigned long *pgd = test_borrow_user_pgd();
    char *rw = pgd ? test_map_user_page(pgd, USER_RW_VA, MMU_PAGE_USER_DATA) : NULL;
    void *ro = rw ? test_map_user_page(pgd, USER_RO_VA, MMU_PAGE_USER_RODATA) : NULL;
    int scratch_fd = vfs_open(SCRATCH_PATH, O_RDWR | O_CREAT);
    int cwd_set = vfs_chdir("/") == 0;

    TEST_ASSERT("scratch address space borrowed", ro != NULL);
    TEST_ASSERT("scratch file created", scratch_fd >= 0);
    TEST_ASSERT("scratch cwd set", cwd_set);

    if (!ro || scratch_fd < 0 || !cwd_set) {
        drop_cwd();
        if (scratch_fd >= 0) {
            vfs_close(scratch_fd);
            vfs_unlink(SCRATCH_PATH);
        }
        if (pgd) {
            test_release_user_pgd(pgd);
        }
        TEST_SUITE_END("Syscall Bounds");
        return;
    }

    strcpy(rw, SCRATCH_PATH);

    // a bad pointer is refused whichever syscall is handed it
    {
        int ptr_ok = 1;

        for (size_t p = 0; p < sizeof(ptr_probes) / sizeof(ptr_probes[0]); p++) {
            const struct ptr_probe *probe = &ptr_probes[p];

            for (size_t b = 0; b < sizeof(bad_ptrs) / sizeof(bad_ptrs[0]); b++) {
                if (bad_ptrs[b] == 0 && probe->null_ok) {
                    continue;
                }

                uint64_t args[6];
                memcpy(args, probe->args, sizeof(args));
                args[probe->ptr_arg] = bad_ptrs[b];

                if (test_syscall(probe->nr, args) >= 0) {
                    pr_err("test: %s accepted pointer %lx [FAILED]\n", probe->name, bad_ptrs[b]);
                    ptr_ok = 0;
                }
            }
        }

        TEST_ASSERT("every bad pointer is refused", ptr_ok);
    }

    // a read-only mapping is not somewhere the kernel may write a result
    {
        int rodata_ok = 1;

        for (size_t p = 0; p < sizeof(write_probes) / sizeof(write_probes[0]); p++) {
            const struct ptr_probe *probe = &write_probes[p];

            uint64_t args[6];
            memcpy(args, probe->args, sizeof(args));
            args[probe->ptr_arg] = USER_RO_VA;

            if (test_syscall(probe->nr, args) >= 0) {
                pr_err("test: %s wrote to a read-only mapping [FAILED]\n", probe->name);
                rodata_ok = 0;
            }
        }

        TEST_ASSERT("a read-only mapping is refused for output", rodata_ok);
    }

    // the same calls with a writable pointer succeed, so the refusals above
    // were about the pointer and not the call
    {
        int rw_ok = 1;

        for (size_t p = 0; p < sizeof(write_probes) / sizeof(write_probes[0]); p++) {
            const struct ptr_probe *probe = &write_probes[p];

            uint64_t args[6];
            memcpy(args, probe->args, sizeof(args));
            args[probe->ptr_arg] = USER_RW_VA;

            if (test_syscall(probe->nr, args) < 0) {
                pr_err("test: %s refused a writable pointer [FAILED]\n", probe->name);
                rw_ok = 0;
            }
        }

        TEST_ASSERT("a writable mapping is accepted for output", rw_ok);

        uint64_t args[6] = {USER_RW_VA, 64, 0, 0, 0, 0};
        TEST_ASSERT("getcwd writes the cwd",
                    test_syscall(SYS_GETCWD, args) >= 0 && strcmp(rw, "/") == 0);
    }

    // a length past the cap is refused, and one inside the buffer is served
    {
        int dir_fd = vfs_open("/", O_RDONLY);
        int len_ok = 1;
        int legal_ok = 1;

        TEST_ASSERT("directory opened for getdents", dir_fd >= 0);

        for (size_t p = 0; dir_fd >= 0 && p < sizeof(len_probes) / sizeof(len_probes[0]); p++) {
            const struct len_probe *probe = &len_probes[p];
            uint64_t fd = (uint64_t)(probe->on_dir ? dir_fd : scratch_fd);

            for (size_t l = 0; l < sizeof(illegal_lens) / sizeof(illegal_lens[0]); l++) {
                uint64_t args[6] = {fd, USER_RW_VA, illegal_lens[l], 0, 0, 0};

                int64_t ret = test_syscall(probe->nr, args);
                if (ret >= 0) {
                    pr_err("test: %s accepted length %lu, returned %ld [FAILED]\n", probe->name,
                           illegal_lens[l], (long)ret);
                    len_ok = 0;
                }
            }

            // Each fits the one mapped page.
            const uint64_t legal_lens[] = {0, probe->unit, PAGE_SIZE};

            for (size_t l = 0; l < sizeof(legal_lens) / sizeof(legal_lens[0]); l++) {
                uint64_t args[6] = {fd, USER_RW_VA, legal_lens[l], 0, 0, 0};

                int64_t ret = test_syscall(probe->nr, args);
                if (ret < 0) {
                    pr_err("test: %s refused length %lu, returned %ld [FAILED]\n", probe->name,
                           legal_lens[l], (long)ret);
                    legal_ok = 0;
                }
            }
        }

        TEST_ASSERT("no length past the cap is accepted", len_ok);
        TEST_ASSERT("a length inside the buffer is served", legal_ok);

        if (dir_fd >= 0) {
            vfs_close(dir_fd);
        }
    }

    // The sweep below closes fd 0 and truncates whatever it names.
    vfs_close(scratch_fd);

    // an fd is an index into a table, and every number outside it is an error
    {
        static const int64_t bad_fds[] = {-1, -2147483648LL, 64, 65535, 0x7FFFFFFFFFFFFFFFLL};
        int fd_ok = 1;

        for (size_t f = 0; f < sizeof(bad_fds) / sizeof(bad_fds[0]); f++) {
            uint64_t fd = (uint64_t)bad_fds[f];

            uint64_t read_args[6] = {fd, USER_RW_VA, 16, 0, 0, 0};
            uint64_t close_args[6] = {fd, 0, 0, 0, 0, 0};
            uint64_t lseek_args[6] = {fd, 0, 0, 0, 0, 0};
            uint64_t fstat_args[6] = {fd, USER_RW_VA, 0, 0, 0, 0};
            uint64_t dup2_args[6] = {fd, fd, 0, 0, 0, 0};
            uint64_t ftruncate_args[6] = {fd, 0, 0, 0, 0, 0};

            if (test_syscall(SYS_READ, read_args) >= 0 || test_syscall(SYS_CLOSE, close_args) >= 0
                || test_syscall(SYS_LSEEK, lseek_args) >= 0
                || test_syscall(SYS_FSTAT, fstat_args) >= 0
                || test_syscall(SYS_DUP2, dup2_args) >= 0
                || test_syscall(SYS_FTRUNCATE, ftruncate_args) >= 0) {
                fd_ok = 0;
            }
        }

        TEST_ASSERT("no out-of-table fd is accepted", fd_ok);
    }

    // a kernel address is never a user buffer, whichever argument carries it
    {
        uint64_t addr = (uint64_t)(uintptr_t)kernel_target;
        char seed[sizeof(kernel_target)];
        int kaddr_ok = 1;

        memset(kernel_target, 0, sizeof(kernel_target));
        strcpy(kernel_target, SCRATCH_PATH);
        memcpy(seed, kernel_target, sizeof(seed));
        strcpy(rw, SCRATCH_PATH);

        for (size_t p = 0; p < sizeof(ptr_probes) / sizeof(ptr_probes[0]); p++) {
            uint64_t args[6];
            memcpy(args, ptr_probes[p].args, sizeof(args));
            args[ptr_probes[p].ptr_arg] = addr;

            if (test_syscall(ptr_probes[p].nr, args) >= 0) {
                pr_err("test: %s accepted a kernel address [FAILED]\n", ptr_probes[p].name);
                kaddr_ok = 0;
            }
        }

        if (memcmp(kernel_target, seed, sizeof(seed)) != 0) {
            kaddr_ok = 0;
        }

        TEST_ASSERT("a kernel address is refused and left untouched", kaddr_ok);
    }

    // everything the table does not name, driven once with zeroed arguments:
    // a number that faults never reaches the assertion
    {
        int swept = 0;

        for (uint64_t nr = 1; nr <= SYSCALL_MAX_NR; nr++) {
            if (is_skipped(nr)) {
                continue;
            }

            uint64_t args[6] = {0};
            test_syscall(nr, args);
            swept++;
        }

        TEST_ASSERT("every unskipped syscall survives zeroed arguments", swept > 0);
    }

    drop_cwd();
    vfs_unlink(SCRATCH_PATH);
    test_release_user_pgd(pgd);

    TEST_SUITE_END("Syscall Bounds");
}
