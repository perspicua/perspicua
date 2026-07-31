/*
 * process.h - Public API for process and address space management.
 *
 * This header defines the Process Control Block (PCB), virtual address
 * space tracking, and the lifecycle management interface.
 */

#ifndef PERSPICUA_SCHED_PROCESS_H
#define PERSPICUA_SCHED_PROCESS_H

#include "types.h"

#include "uapi/wait.h"

#include "arch/exception.h"

#include "core/signals.h"
#include "core/lock.h"
#include "fs/vfs.h"
#include "sched/sched.h"

/*
 * Process slots. PCBs are allocated on demand, so a slot costs one pointer
 * until it is used and this no longer multiplies the per-process limits below.
 *
 * This is *not* sized by memory or by the ASID pool, both of which would allow
 * far more. It is the backpressure that keeps fork() inside the range the
 * address-space copy is known to survive: user/apps/stress.c forks 900 children
 * and, once enough are live at once, children come back from fork() seeing a
 * corrupted stack -- they read a nonzero return, take the parent branch, and
 * fault writing near NULL. Measured over three runs of that test: 128 slots is
 * clean, 256 faults in one run of three, 65536 is badly unstable.
 *
 * Raising this is blocked on fixing that, not on the table.
 */
#ifdef CONFIG_MAX_PROCESSES
    #define PROCESS_TABLE_SIZE CONFIG_MAX_PROCESSES
#else
    #define PROCESS_TABLE_SIZE 128
#endif

/*
 * Range the per-process allocator hands out. It starts well above the address
 * user programs link at (0x100000), so ELF segments -- which elf_load maps
 * directly at their p_vaddr, without going through this allocator -- can never
 * collide with an allocated region.
 */
#define USER_VA_BASE        0x40000000ULL
#define USER_VA_LIMIT       0x8000000000ULL
#define USER_VA_MAX_REGIONS 64

/*
 * User stacks are populated eagerly -- every page is allocated and mapped at
 * process creation, touched or not -- so this is real memory per process, not
 * reserved address space. Keep it modest until stacks grow on demand.
 */
#define PROCESS_USER_STACK_PAGES 32

#define SPSR_EL0_USER      0x00000000ULL
#define SPSR_EL1_KERN      0x000003C5ULL
#define STACK_CANARY_VALUE 0xDEADC0DEDEADC0DEULL

/*
 * A free slot is a null pointer in process_table, not a state, so there is no
 * "empty" enumerator: nothing holds a PCB that describes no process. Numbering
 * starts at 1 so a zeroed allocation reads as an invalid state rather than a
 * running one.
 */
typedef enum {
    PROCESS_STATE_RUNNING = 1,
    PROCESS_STATE_ZOMBIE,
    PROCESS_STATE_DEAD
} process_state_t;

struct va_region {
    uintptr_t base;
    size_t pages;
};

/*
 * struct va_allocator - Allocated address ranges of one process.
 *
 * regions[] is kept sorted by base so a single pass finds the lowest gap that
 * fits a request.
 */
struct va_allocator {
    struct va_region regions[USER_VA_MAX_REGIONS];
    size_t count;
};

/*
 * struct process - The central Process Control Block (PCB).
 */
struct process {
    uint32_t pid;
    uint32_t pgid;
    uint32_t sid;
    int has_execed;
    int stop_reported;
    char name[64];
    process_state_t state;
    uint32_t parent_pid;
    struct task *main_task;
    int exit_status;

    uintptr_t paddr_code;
    uintptr_t paddr_user_stack;
    uintptr_t paddr_kernel_stack;

    uintptr_t vaddr_code;
    uintptr_t vaddr_user_stack;
    uintptr_t vaddr_kernel_stack;

    unsigned long *user_pgd;
    unsigned long asid;
    unsigned long asid_generation;
    unsigned long ttbr0;

    struct cpu_context context;
    struct va_allocator va;

    struct vfs_file *fd_table[VFS_MAX_FDS];
    int fd_flags[VFS_MAX_FDS];
    spinlock_t fd_lock;
    struct vfs_vnode *cwd;

    sigset_t pending_signals;
    sigset_t blocked_signals;
    struct sigaction signal_handlers[SIGNAL_COUNT];
    uintptr_t default_sigrestorer;
};

/*
 * Slots are pointers to PCBs allocated on demand. A null entry is a free slot.
 * Holding whole PCBs inline made every per-process limit cost
 * PROCESS_TABLE_SIZE times its size, whether or not the processes existed.
 *
 * Entries are published and cleared under process_table_lock.
 */
extern struct process *process_table[PROCESS_TABLE_SIZE];
extern spinlock_t process_table_lock;

/*
 * process_slot - The PCB for a pid, or NULL if the slot is free.
 *
 * Bounds-checks the pid, so it is safe on anything a caller might pass.
 */
static inline struct process *process_slot(uint32_t pid)
{
    return pid < PROCESS_TABLE_SIZE ? process_table[pid] : NULL;
}

/* Lifecycle and execution */
void process_init(void);
void process_create(void *code_ptr, size_t code_size, uint32_t pid);
int process_create_from_file(const char *path, uint32_t pid);
int process_exec(const char *path, char *const argv[], char *const envp[]);
int process_fork(struct exception_trap_frame *parent_tf);
void process_exit(uint32_t pid, int status);
int process_waitpid(int pid, int *status, int options);

/* Context and identity */
void process_drop_to_user(void *code_vaddr, void *stack_vaddr);
int process_find_current(void);

/*
 * process_current - PCB of the calling task's process, or NULL if it has none.
 *
 * Resolves the pid and the slot together: a caller cannot end up holding a pid
 * whose slot has already been freed.
 */
struct process *process_current(void);

unsigned long process_get_ttbr0(uint32_t pid);

#ifdef CONFIG_TESTS
/* Reserves a free PID slot, zeroed and RUNNING. Returns the pid or negative. */
int process_test_claim_slot(void);

/* Frees a slot claimed by the above. */
void process_test_release_slot(uint32_t pid);
#endif

/* Memory management */
uintptr_t process_va_alloc(struct va_allocator *va, size_t pages);
void process_va_free(struct va_allocator *va, uintptr_t base);
void process_flush_icache_range(void *start, size_t size);

#endif /* PERSPICUA_SCHED_PROCESS_H */
