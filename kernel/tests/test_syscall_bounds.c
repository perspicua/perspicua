/*
 * test_syscall_bounds.c - Syscall dispatch driven with arguments no caller sends.
 *
 * Slot 0 is the kernel PCB and has no address space, so the suite lends it a
 * scratch one: without that, a pointer is refused for want of a pgd rather
 * than on its own merits. A pointer is refused twice over even so, by the
 * range checks and by the unprivileged loads the copy helpers use, so this is
 * a backstop there. Only the dispatcher itself is reached here alone.
 */

#include <stddef.h>
#include <stdint.h>

#include "test.h"

#include "string.h"

#include "uapi/errno.h"
#include "uapi/fcntl.h"
#include "uapi/signals.h"
#include "uapi/syscalls.h"

#include "arch/exception.h"

#include "core/syscall.h"
#include "mm/addr.h"
#include "mm/mmu.h"
#include "mm/pmm.h"
#include "sched/process.h"

// Scratch user addresses, mapped into the borrowed pgd below.
#define USER_RW_VA 0x0000000050000000UL
#define USER_RO_VA 0x0000000050001000UL

#define USER_UNMAPPED_VA 0x0000000040000000UL

// Highest number the dispatcher has a slot for.
#define SYSCALL_MAX_NR SYS_SIGALTSTACK

// 288 bytes per frame, so the probes share one rather than nesting them.
static struct exception_trap_frame probe_tf;

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

// Lengths a caller may legitimately pass: a zero-length read returns 0, so
// the property is only that the call comes back.
static const uint64_t legal_lens[] = {0, 1, SYSCALL_MAX_RW_SIZE};

// Past the cap the dispatcher enforces, or wrapped: these must be refused.
static const uint64_t illegal_lens[] = {
    SYSCALL_MAX_RW_SIZE + 1,
    (uint64_t)-1,
    0x8000000000000000ULL,
};

static int64_t call_syscall(uint64_t nr, const uint64_t *args)
{
    memset(&probe_tf, 0, sizeof(probe_tf));
    probe_tf.x[8] = nr;
    for (int i = 0; i < 6; i++) {
        probe_tf.x[i] = args[i];
    }
    syscall_handle(&probe_tf);
    return (int64_t)probe_tf.x[0];
}

static int is_skipped(uint64_t nr)
{
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++) {
        if (skipped[i].nr == nr) {
            return 1;
        }
    }
    return 0;
}

/*
 * The caller owns the returned pgd and must pass it to release_user_pgd.
 * TTBR0 is left alone: the validation helpers walk the tables by pointer, and
 * a syscall that gets past them faults on the copy and takes the fixup path,
 * which is an error return either way.
 */
static unsigned long *borrow_user_pgd(void)
{
    unsigned long *pgd = mmu_create_user_pgd();
    if (!pgd) {
        return NULL;
    }

    void *rw = pmm_alloc_page();
    void *ro = pmm_alloc_page();
    if (!rw || !ro) {
        mmu_destroy_user_pgd(pgd);
        return NULL;
    }

    mmu_user_map_page(pgd, USER_RW_VA, V2P(rw), MMU_PAGE_USER_DATA);
    mmu_user_map_page(pgd, USER_RO_VA, V2P(ro), MMU_PAGE_USER_RODATA);

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    process_table[0]->user_pgd = pgd;
    spin_unlock_irqrestore(&process_table_lock, flags);

    return pgd;
}

static void release_user_pgd(unsigned long *pgd)
{
    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    process_table[0]->user_pgd = NULL;
    spin_unlock_irqrestore(&process_table_lock, flags);

    // Frees the mapped pages along with the tables.
    mmu_destroy_user_pgd(pgd);
}

void test_syscall_bounds(void)
{
    TEST_SUITE_BEGIN("Syscall Bounds");

    // a number with no handler must be reported, never dispatched
    {
        uint64_t args[6] = {0};

        TEST_ASSERT_EQ("syscall 0 refused", call_syscall(0, args), -ENOSYS);
        TEST_ASSERT_EQ("gap in the table refused", call_syscall(5, args), -ENOSYS);
        TEST_ASSERT_EQ("one past the table refused", call_syscall(SYSCALL_MAX_NR + 1, args),
                       -ENOSYS);
        TEST_ASSERT_EQ("far out of range refused", call_syscall(4096, args), -ENOSYS);
        TEST_ASSERT_EQ("a number that indexes nothing refused", call_syscall((uint64_t)-1, args),
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

    unsigned long *pgd = borrow_user_pgd();
    TEST_ASSERT("scratch address space borrowed", pgd != NULL);

    if (!pgd) {
        TEST_SUITE_END("Syscall Bounds");
        return;
    }

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

                if (call_syscall(probe->nr, args) >= 0) {
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

            if (call_syscall(probe->nr, args) >= 0) {
                pr_err("test: %s wrote to a read-only mapping [FAILED]\n", probe->name);
                rodata_ok = 0;
            }
        }

        TEST_ASSERT("a read-only mapping is refused for output", rodata_ok);
    }

    // a length at either extreme is refused rather than trusted
    {
        static const struct ptr_probe len_probes[] = {
            {"read", SYS_READ, 2, 0, {0, USER_RW_VA, 0, 0, 0, 0}},
            {"write", SYS_WRITE, 2, 0, {0, USER_RW_VA, 0, 0, 0, 0}},
            {"getdents", SYS_GETDENTS, 2, 0, {0, USER_RW_VA, 0, 0, 0, 0}},
            {"pread", SYS_PREAD, 2, 0, {0, USER_RW_VA, 0, 0, 0, 0}},
            {"pwrite", SYS_PWRITE, 2, 0, {0, USER_RW_VA, 0, 0, 0, 0}},
            {"getcwd", SYS_GETCWD, 1, 0, {USER_RW_VA, 0, 0, 0, 0, 0}},
        };
        int len_ok = 1;

        for (size_t p = 0; p < sizeof(len_probes) / sizeof(len_probes[0]); p++) {
            const struct ptr_probe *probe = &len_probes[p];

            for (size_t l = 0; l < sizeof(illegal_lens) / sizeof(illegal_lens[0]); l++) {
                uint64_t args[6];
                memcpy(args, probe->args, sizeof(args));
                args[probe->ptr_arg] = illegal_lens[l];

                int64_t ret = call_syscall(probe->nr, args);
                if (ret >= 0) {
                    pr_err("test: %s accepted length %lu, returned %ld [FAILED]\n", probe->name,
                           illegal_lens[l], (long)ret);
                    len_ok = 0;
                }
            }

            // Reaching the next probe is the result: a legal length may return
            // anything, but it may not fault.
            for (size_t l = 0; l < sizeof(legal_lens) / sizeof(legal_lens[0]); l++) {
                uint64_t args[6];
                memcpy(args, probe->args, sizeof(args));
                args[probe->ptr_arg] = legal_lens[l];
                call_syscall(probe->nr, args);
            }
        }

        TEST_ASSERT("no length past the cap is accepted", len_ok);
    }

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

            if (call_syscall(SYS_READ, read_args) >= 0 || call_syscall(SYS_CLOSE, close_args) >= 0
                || call_syscall(SYS_LSEEK, lseek_args) >= 0
                || call_syscall(SYS_FSTAT, fstat_args) >= 0
                || call_syscall(SYS_DUP2, dup2_args) >= 0
                || call_syscall(SYS_FTRUNCATE, ftruncate_args) >= 0) {
                fd_ok = 0;
            }
        }

        TEST_ASSERT("no out-of-table fd is accepted", fd_ok);
    }

    // a kernel address is never a user buffer, whichever argument carries it
    {
        uint64_t addr = (uint64_t)(uintptr_t)kernel_target;
        int kaddr_ok = 1;

        memset(kernel_target, 0xA5, sizeof(kernel_target));

        for (size_t p = 0; p < sizeof(ptr_probes) / sizeof(ptr_probes[0]); p++) {
            uint64_t args[6];
            memcpy(args, ptr_probes[p].args, sizeof(args));
            args[ptr_probes[p].ptr_arg] = addr;

            if (call_syscall(ptr_probes[p].nr, args) >= 0) {
                kaddr_ok = 0;
            }
        }

        for (size_t i = 0; i < sizeof(kernel_target); i++) {
            if ((unsigned char)kernel_target[i] != 0xA5) {
                kaddr_ok = 0;
                break;
            }
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
            call_syscall(nr, args);
            swept++;
        }

        TEST_ASSERT("every unskipped syscall survives zeroed arguments", swept > 0);
    }

    release_user_pgd(pgd);

    TEST_SUITE_END("Syscall Bounds");
}
