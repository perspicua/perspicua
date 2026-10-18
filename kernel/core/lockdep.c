/*
 * lockdep.c - Static-order lock validator.
 */

#include "core/lockdep.h"

#include <stdint.h>

#include "stdio.h"

#include "arch/irq.h"

#include "core/lock.h"
#include "panic.h"
#include "sched/sched.h"

// With CONFIG_LOCKDEP off, lockdep.h has inline stubs and this file compiles to nothing.
#ifdef CONFIG_LOCKDEP

    #define MAX_HELD_LOCKS 16
    // Distinct lock addresses: a full test run takes about 300, and the graph costs N^2/8 bytes.
    #define MAX_LOCK_NODES 1024

static spinlock_t lockdep_lock = SPINLOCK_INIT;
static int lockdep_disabled[CPU_MAX_CORES] = {0}; // Indexed by core ID

static uintptr_t held_locks[CPU_MAX_CORES][MAX_HELD_LOCKS];
static int held_count[CPU_MAX_CORES] = {0};

static uintptr_t lock_nodes[MAX_LOCK_NODES];
static int num_lock_nodes = 0;

// 1 once a lock found the table full, 2 once that has been reported.
static int lockdep_table_full;

// graph_edges[A][B] == 1 means lock A was acquired before lock B globally
static uint8_t graph_edges[MAX_LOCK_NODES][MAX_LOCK_NODES / 8];

    #ifdef CONFIG_TESTS
static int lockdep_quiet_mode = 0;
static int lockdep_violation_count = 0;

void lockdep_test_quiet(int on)
{
    lockdep_quiet_mode = on;
}

int lockdep_test_violations(void)
{
    return lockdep_violation_count;
}
    #endif

// Called with lockdep_lock dropped, since printing takes locks of its own.
static void lockdep_report_full(void)
{
    int expected = 1;
    if (__atomic_load_n(&lockdep_table_full, __ATOMIC_RELAXED) == 1
        && __atomic_compare_exchange_n(&lockdep_table_full, &expected, 2, 0, __ATOMIC_RELAXED,
                                       __ATOMIC_RELAXED)) {
        pr_warn("lockdep: node table full at %d locks; locks seen from now on go unchecked\n",
                MAX_LOCK_NODES);
    }
}

static void lockdep_violation(const char *msg)
{
    #ifdef CONFIG_TESTS
    if (lockdep_quiet_mode) {
        lockdep_violation_count++;
        pr_info("lockdep: violation: %s\n", msg);
        return;
    }
    #endif
    PANIC(msg);
}

static inline int get_edge(int a, int b)
{
    return (graph_edges[a][b / 8] & (1 << (b % 8))) != 0;
}

static inline void set_edge(int a, int b)
{
    graph_edges[a][b / 8] |= (1 << (b % 8));
}

static int get_or_create_node(uintptr_t lock)
{
    for (int i = 0; i < num_lock_nodes; i++) {
        if (lock_nodes[i] == lock) {
            return i;
        }
    }
    if (num_lock_nodes >= MAX_LOCK_NODES) {
        if (lockdep_table_full == 0) {
            lockdep_table_full = 1;
        }
        return -1;
    }
    int idx = num_lock_nodes++;
    lock_nodes[idx] = lock;
    return idx;
}

// Not spin_lock, which would recurse into lockdep.
static void raw_spin_lock(spinlock_t *lock)
{
    unsigned int tmp;
    unsigned int one = 1;
    asm volatile("   sevl\n"
                 "1: wfe\n"
                 "2: ldaxr   %w0, [%1]\n"
                 "   cbnz    %w0, 1b\n"
                 "   stxr    %w0, %w2, [%1]\n"
                 "   cbnz    %w0, 2b\n"
                 : "=&r"(tmp)
                 : "r"(&lock->locked), "r"(one)
                 : "memory");
}

static void raw_spin_unlock(spinlock_t *lock)
{
    asm volatile("   stlr    %w0, [%1]\n"
                 "   sev\n"
                 :
                 : "r"(0), "r"(&lock->locked)
                 : "memory");
}

static int lockdep_order(int held_node, int new_node)
{
    if (held_node == new_node) {
        return 1;
    }

    if (get_edge(new_node, held_node)) {
        return 1;
    }

    if (!get_edge(held_node, new_node)) {
        set_edge(held_node, new_node);

        // graph_edges stays transitively closed, so a cycle check is one lookup.
        for (int x = 0; x < num_lock_nodes; x++) {
            if (get_edge(x, held_node)) {
                set_edge(x, new_node);
                for (int y = 0; y < num_lock_nodes; y++) {
                    if (get_edge(new_node, y)) {
                        set_edge(x, y);
                    }
                }
            }
        }
        for (int y = 0; y < num_lock_nodes; y++) {
            if (get_edge(new_node, y)) {
                set_edge(held_node, y);
            }
        }
    }

    return 0;
}

void lockdep_init(void)
{
    pr_info("lockdep: initialized kernel lock dependency validator\n");
}

void lockdep_might_sleep(void)
{
    if (preempt_active()) {
        lockdep_violation("lockdep: sleeping lock taken while holding a spinlock");
    }
    if (irq_in_handler()) {
        lockdep_violation("lockdep: sleeping in an IRQ handler");
    }
}

void lockdep_acquire(spinlock_t *lock)
{
    int core = cpu_id();

    // After a panic a halted core may hold lockdep_lock, and nothing is left to validate.
    if (lockdep_disabled[core] || kernel_panicked) {
        return;
    }

    lockdep_disabled[core] = 1;
    raw_spin_lock(&lockdep_lock);

    int count = held_count[core];
    if (count >= MAX_HELD_LOCKS) {
        raw_spin_unlock(&lockdep_lock);
        lockdep_disabled[core] = 0;
        PANIC("lockdep: max held locks exceeded");
    }

    uintptr_t new_addr = (uintptr_t)lock;
    char msg[96];
    const char *violation = NULL;

    for (int i = 0; i < count; i++) {
        if (held_locks[core][i] == new_addr) {
            snprintf(msg, sizeof(msg), "lockdep: recursive acquisition of spinlock %p on core %d",
                     (void *)new_addr, core);
            violation = msg;
            break;
        }
    }

    int new_node = violation ? -1 : get_or_create_node(new_addr);
    for (int i = 0; new_node >= 0 && i < count && !violation; i++) {
        int held_node = get_or_create_node(held_locks[core][i]);
        if (held_node >= 0 && lockdep_order(held_node, new_node)) {
            snprintf(msg, sizeof(msg),
                     "lockdep: spinlock order cycle on core %d: %p held, acquiring %p", core,
                     (void *)held_locks[core][i], (void *)new_addr);
            violation = msg;
        }
    }

    // Recorded even on a violation, so a quiet-mode test's release still balances.
    held_locks[core][count] = new_addr;
    held_count[core] = count + 1;

    raw_spin_unlock(&lockdep_lock);
    lockdep_disabled[core] = 0;

    lockdep_report_full();
    if (violation) {
        lockdep_violation(violation);
    }
}

void lockdep_release(spinlock_t *lock)
{
    int core = cpu_id();

    // After a panic a halted core may hold lockdep_lock, and nothing is left to validate.
    if (lockdep_disabled[core] || kernel_panicked) {
        return;
    }

    lockdep_disabled[core] = 1;
    raw_spin_lock(&lockdep_lock);

    int count = held_count[core];
    uintptr_t addr = (uintptr_t)lock;

    int found = -1;
    for (int i = count - 1; i >= 0; i--) {
        if (held_locks[core][i] == addr) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        for (int i = found; i < count - 1; i++) {
            held_locks[core][i] = held_locks[core][i + 1];
        }
        held_count[core] = count - 1;
    } else {
        // Logged, not fatal: an unbalanced release is a bug, not a reason to halt.
        raw_spin_unlock(&lockdep_lock);
        lockdep_disabled[core] = 0;
        pr_err("lockdep: attempting to release unheld lock at %p\n", (void *)addr);
        return;
    }

    raw_spin_unlock(&lockdep_lock);
    lockdep_disabled[core] = 0;
}

void lockdep_acquire_sleep(const void *lock)
{
    lockdep_might_sleep();

    struct task *t = sched_current_task();
    if (!t) {
        return;
    }

    unsigned long flags = irq_save();
    int core = cpu_id();

    lockdep_disabled[core] = 1;
    raw_spin_lock(&lockdep_lock);

    char cycle_msg[80];
    const char *violation = NULL;

    for (int i = 0; i < t->lockdep_depth; i++) {
        if (t->lockdep_held[i] == lock) {
            violation = "recursive sleeping lock";
            break;
        }
    }

    if (t->lockdep_depth == LOCKDEP_TASK_HELD) {
        raw_spin_unlock(&lockdep_lock);
        lockdep_disabled[core] = 0;
        irq_restore(flags);
        PANIC("lockdep: max held sleeping locks exceeded");
    }

    if (!violation) {
        int new_node = get_or_create_node((uintptr_t)lock | 1);
        if (new_node >= 0) {
            for (int i = 0; i < t->lockdep_depth; i++) {
                int held_node = get_or_create_node((uintptr_t)t->lockdep_held[i] | 1);
                if (held_node < 0) {
                    continue;
                }
                if (lockdep_order(held_node, new_node)) {
                    if (!violation) {
                        snprintf(cycle_msg, sizeof(cycle_msg),
                                 "sleeping lock order cycle: %p held, acquiring %p",
                                 t->lockdep_held[i], lock);
                        violation = cycle_msg;
                    }
                }
            }
        }
    }

    t->lockdep_held[t->lockdep_depth++] = lock;

    raw_spin_unlock(&lockdep_lock);
    lockdep_disabled[core] = 0;
    irq_restore(flags);

    lockdep_report_full();
    if (violation) {
        lockdep_violation(violation);
    }
}

void lockdep_release_sleep(const void *lock)
{
    struct task *t = sched_current_task();
    if (!t) {
        return;
    }

    int found = -1;
    for (int i = t->lockdep_depth - 1; i >= 0; i--) {
        if (t->lockdep_held[i] == lock) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        for (int i = found; i < t->lockdep_depth - 1; i++) {
            t->lockdep_held[i] = t->lockdep_held[i + 1];
        }
        t->lockdep_depth--;
    } else {
        pr_err("lockdep: attempting to release unheld sleeping lock at %p\n", lock);
    }
}

void lockdep_assert_no_sleep_locks(const char *where)
{
    struct task *t = sched_current_task();
    if (!t) {
        return;
    }

    if (t->lockdep_depth != 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "sleeping lock %p held at %s",
                 t->lockdep_held[t->lockdep_depth - 1], where ? where : "exit");
        lockdep_violation(msg);
    }
}

void lockdep_assert_preemptible(void)
{
    if (preempt_active()) {
        struct task *t = sched_current_task();
        char msg[80];
        snprintf(msg, sizeof(msg), "sched_schedule with a spinlock held (preempt_count %d)",
                 t ? t->preempt_count : 0);
        lockdep_violation(msg);
    }
}

#endif // CONFIG_LOCKDEP
