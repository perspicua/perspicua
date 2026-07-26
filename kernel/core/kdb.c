/*
 * kdb.c - UART-based Kernel Debugger (KDB).
 *
 * Provides an interactive console to inspect kernel state after a panic
 * or when triggered explicitly. All I/O goes directly to the PL011 UART
 * so the debugger works even when the TTY subsystem is broken.
 *
 * Entry points:
 *   kdb_enter(reason)       -- from arbitrary kernel code
 *   kdb_enter_tf(reason,tf) -- from exception / panic handler
 *
 * Both disable interrupts and block until the user types 'continue'.
 */

#include "debug/kdb.h"

#include "stdio.h"
#include "string.h"

#include "arch/exception.h"
#include "core/timer.h"
#include "core/lock.h"
#include "driver/uart.h"
#include "mm/pmm.h"
#include "panic.h"
#include "sched/process.h"
#include "sched/sched.h"

#define KDB_LINE_MAX  128
#define KDB_MAX_ARGS  8
#define KDB_BT_FRAMES 24
#define KDB_RD_MAX    256

/* Trap frame saved on entry (NULL when entered without an exception frame). */
static struct exception_trap_frame *kdb_tf;

/* ------------------------------------------------------------------ */
/* Low-level I/O – bypass the TTY layer so KDB works in broken states  */
/* ------------------------------------------------------------------ */

static void kdb_putc(char c)
{
    if (c == '\n')
        uart_send('\r');
    uart_send(c);
}

static void kdb_puts(const char *s)
{
    while (*s)
        kdb_putc(*s++);
}

/*
 * kdb_readline - Read a line from UART with minimal line editing.
 *
 * Handles Enter (submit), Backspace / DEL (erase last character).
 * Returns the number of characters stored (excluding the NUL terminator).
 */
static int kdb_readline(char *buf, int maxlen)
{
    int pos = 0;

    for (;;) {
        char c = uart_getc();

        if (c == '\r' || c == '\n') {
            kdb_putc('\n');
            buf[pos] = '\0';
            return pos;
        }

        /* Backspace (^H) or DEL */
        if ((c == '\b' || c == 0x7f) && pos > 0) {
            pos--;
            kdb_puts("\b \b");
            continue;
        }

        /* Ignore control characters and overflow */
        if (c < 0x20 || pos >= maxlen - 1)
            continue;

        buf[pos++] = c;
        kdb_putc(c);
    }
}

/*
 * kdb_tokenize - Split a mutable string into argv-style tokens.
 * Returns the number of tokens found.
 */
static int kdb_tokenize(char *buf, char **argv, int maxargs)
{
    int argc = 0;
    char *p = buf;

    while (*p && argc < maxargs) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ')
            p++;
        if (*p)
            *p++ = '\0';
    }
    return argc;
}

/*
 * kdb_parse_hex - Parse a hex string (optional "0x" prefix).
 */
static unsigned long kdb_parse_hex(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;

    unsigned long val = 0;
    while (*s) {
        char c = *s++;
        unsigned long nibble;
        if (c >= '0' && c <= '9')
            nibble = (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f')
            nibble = (unsigned long)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F')
            nibble = (unsigned long)(c - 'A') + 10;
        else
            break;
        val = (val << 4) | nibble;
    }
    return val;
}

/* ------------------------------------------------------------------ */
/* Command implementations                                              */
/* ------------------------------------------------------------------ */

static void cmd_help(void)
{
    kdb_puts("\n"
             "  KDB Commands:\n"
             "  -------------\n"
             "  help              Show this message\n"
             "  regs              Dump CPU registers\n"
             "  sysregs           Dump key AArch64 system registers\n"
             "  tasks             List processes and their tasks\n"
             "  mem               Show physical memory statistics\n"
             "  bt                Print a stack backtrace\n"
             "  rd <addr> [n]     Hex-dump n 64-bit words at addr (default 8)\n"
             "  wr <addr> <val>   Write a 64-bit value to addr\n"
             "  continue          Exit KDB and resume (or halt if called from panic)\n"
             "\n");
}

static void cmd_regs(void)
{
    if (kdb_tf) {
        struct exception_trap_frame *tf = kdb_tf;

        kdb_puts("\n  --- General Registers (from exception frame) ---\n");
        for (int i = 0; i < 28; i += 2) {
            printf("  x%-2d: 0x%016lx   x%-2d: 0x%016lx\n", i, tf->x[i], i + 1, tf->x[i + 1]);
        }
        printf("  x29: 0x%016lx   x30: 0x%016lx\n", tf->x[29], tf->x30);

        kdb_puts("\n  --- Exception Frame Registers ---\n");
        printf("  ELR_EL1  : 0x%016lx  (PC at exception)\n", tf->elr_el1);
        printf("  SP_EL0   : 0x%016lx\n", tf->sp_el0);
        printf("  SPSR_EL1 : 0x%016lx\n", tf->spsr_el1);
    } else {
        unsigned long sp, lr, fp;
        asm volatile("mov %0, sp" : "=r"(sp));
        asm volatile("mov %0, x30" : "=r"(lr));
        asm volatile("mov %0, x29" : "=r"(fp));

        kdb_puts("\n  --- Registers (live EL1 snapshot) ---\n");
        printf("  SP  (x31): 0x%016lx\n", sp);
        printf("  LR  (x30): 0x%016lx\n", lr);
        printf("  FP  (x29): 0x%016lx\n", fp);
        kdb_puts("  (x0-x28 not preserved in non-exception KDB entry)\n");
    }
}

static void cmd_sysregs(void)
{
    unsigned long esr, far_reg, spsr, ttbr0, ttbr1, sctlr, mair, tcr, mpidr, midr, tpidr;

    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, far_el1" : "=r"(far_reg));
    asm volatile("mrs %0, spsr_el1" : "=r"(spsr));
    asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    asm volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    asm volatile("mrs %0, mair_el1" : "=r"(mair));
    asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    asm volatile("mrs %0, midr_el1" : "=r"(midr));
    asm volatile("mrs %0, tpidr_el1" : "=r"(tpidr));

    kdb_puts("\n  --- AArch64 System Registers ---\n");
    printf("  ESR_EL1   : 0x%016lx  EC=0x%02lx ISS=0x%06lx\n", esr, (esr >> 26) & 0x3fUL,
           esr & 0x1ffffffUL);
    printf("  FAR_EL1   : 0x%016lx\n", far_reg);
    printf("  SPSR_EL1  : 0x%016lx\n", spsr);
    printf("  TTBR0_EL1 : 0x%016lx\n", ttbr0);
    printf("  TTBR1_EL1 : 0x%016lx\n", ttbr1);
    printf("  SCTLR_EL1 : 0x%016lx\n", sctlr);
    printf("  MAIR_EL1  : 0x%016lx\n", mair);
    printf("  TCR_EL1   : 0x%016lx\n", tcr);
    printf("  MPIDR_EL1 : 0x%016lx  (core %lu)\n", mpidr, mpidr & 3UL);
    printf("  MIDR_EL1  : 0x%016lx\n", midr);
    printf("  TPIDR_EL1 : 0x%016lx  (current task)\n", tpidr);
}

/* Stringify task execution state. */
static const char *task_state_str(enum sched_task_state s)
{
    switch (s) {
        case SCHED_TASK_RUNNING:
            return "RUNNING";
        case SCHED_TASK_READY:
            return "READY  ";
        case SCHED_TASK_BLOCKED:
            return "BLOCKED";
        case SCHED_TASK_STOPPED:
            return "STOPPED";
        case SCHED_TASK_DEAD:
            return "DEAD   ";
        default:
            return "???????";
    }
}

/* Stringify process lifecycle state. */
static const char *proc_state_str(process_state_t s)
{
    switch (s) {
        case PROCESS_STATE_EMPTY:
            return "EMPTY  ";
        case PROCESS_STATE_RUNNING:
            return "RUNNING";
        case PROCESS_STATE_ZOMBIE:
            return "ZOMBIE ";
        case PROCESS_STATE_DEAD:
            return "DEAD   ";
        default:
            return "???????";
    }
}

static void cmd_tasks(void)
{
    kdb_puts("\n  --- Process Table ---\n");
    printf("  %-5s %-8s %-5s %-18s %-18s  %s\n", "PID", "STATE", "PPID", "CODE_VA", "KSTACK_VA",
           "NAME");

    unsigned long flags = spin_lock_irqsave(&process_table_lock);

    int shown = 0;
    for (int i = 0; i < PROCESS_TABLE_SIZE; i++) {
        struct process *p = &process_table[i];
        if (p->state == PROCESS_STATE_EMPTY)
            continue;

        shown++;
        printf("  %-5u %-8s %-5u 0x%016lx 0x%016lx  %s\n", p->pid, proc_state_str(p->state),
               p->parent_pid, (unsigned long)p->vaddr_code, (unsigned long)p->vaddr_kernel_stack,
               p->name[0] ? p->name : "(unnamed)");

        if (p->main_task) {
            struct task *t = p->main_task;
            printf("    -> task id=%-4lu state=%-7s on_core=%-2d stack=0x%016lx\n", t->id,
                   task_state_str(t->state), t->on_core, (unsigned long)t->stack);
        }
    }

    spin_unlock_irqrestore(&process_table_lock, flags);

    if (!shown)
        kdb_puts("  (no user processes)\n");

    /* Current kernel task */
    struct task *cur = sched_get_current();
    kdb_puts("\n  --- Current Kernel Task ---\n");
    if (cur) {
        printf("  id=%-4lu pid=%-4u state=%-7s on_core=%-2d stack=0x%016lx\n", cur->id, cur->pid,
               task_state_str(cur->state), cur->on_core, (unsigned long)cur->stack);
    } else {
        kdb_puts("  (none)\n");
    }

    /* Per-core scheduler statistics */
    kdb_puts("\n  --- Scheduler Statistics ---\n");
    for (int c = 0; c < SCHED_NUM_CORES; c++) {
        printf("  core%d: ctx_switches=%llu  idle_ticks=%llu\n", c,
               (unsigned long long)core_sched_stats[c].context_switches,
               (unsigned long long)core_sched_stats[c].idle_count);
    }
}

static void cmd_mem(void)
{
    unsigned long free_pages = pmm_get_free_pages();
    unsigned long total_pages = pmm_get_total_pages();
    unsigned long used_pages = total_pages - free_pages;

    kdb_puts("\n  --- Physical Memory ---\n");
    printf("  Total  : %6lu pages  (%4lu MiB)\n", total_pages, (total_pages * 4UL) >> 10);
    printf("  In use : %6lu pages  (%4lu MiB)\n", used_pages, (used_pages * 4UL) >> 10);
    printf("  Free   : %6lu pages  (%4lu MiB)\n", free_pages, (free_pages * 4UL) >> 10);
}

/*
 * cmd_bt - Walk the AArch64 frame pointer chain.
 *
 * If entered from an exception frame, starts at x29 in the frame.
 * Otherwise uses the live x29 (which points into KDB's own call chain,
 * letting you see what called kdb_enter).
 */
static void cmd_bt(void)
{
    unsigned long fp;

    if (kdb_tf) {
        fp = kdb_tf->x[29];
    } else {
        asm volatile("mov %0, x29" : "=r"(fp));
    }

    kdb_puts("\n  --- Stack Backtrace ---\n");

    if (!fp) {
        kdb_puts("  (frame pointer is NULL)\n");
        return;
    }

    for (int i = 0; i < KDB_BT_FRAMES; i++) {
        if (fp & 0x7UL) {
            printf("  #%-2d [unaligned fp 0x%016lx]\n", i, fp);
            break;
        }

        unsigned long *frame = (unsigned long *)fp;
        unsigned long prev_fp = frame[0];
        unsigned long ret_addr = frame[1];

        unsigned long offset = 0;
        const char *sym = panic_resolve_symbol(ret_addr, &offset);

        if (sym) {
            printf("  #%-2d 0x%016lx  <%s+0x%lx>\n", i, ret_addr, sym, offset);
        } else {
            printf("  #%-2d 0x%016lx\n", i, ret_addr);
        }

        if (!prev_fp || prev_fp <= fp)
            break;
        fp = prev_fp;
    }
}

static void cmd_rd(int argc, char **argv)
{
    if (argc < 2) {
        kdb_puts("  usage: rd <addr> [count]\n");
        return;
    }

    unsigned long addr = kdb_parse_hex(argv[1]);
    unsigned long count = (argc >= 3) ? kdb_parse_hex(argv[2]) : 8;

    if (count > KDB_RD_MAX)
        count = KDB_RD_MAX;

    kdb_puts("\n");
    for (unsigned long i = 0; i < count; i++) {
        volatile unsigned long *p = (volatile unsigned long *)(addr + i * 8);
        printf("  0x%016lx :  0x%016lx\n", (unsigned long)p, *p);
    }
}

static void cmd_wr(int argc, char **argv)
{
    if (argc < 3) {
        kdb_puts("  usage: wr <addr> <val>\n");
        return;
    }

    unsigned long addr = kdb_parse_hex(argv[1]);
    unsigned long val = kdb_parse_hex(argv[2]);
    unsigned long *p = (unsigned long *)addr;

    *p = val;
    asm volatile("dsb sy" ::: "memory");
    printf("  [0x%016lx] <- 0x%016lx\n", addr, val);
}

/* ------------------------------------------------------------------ */
/* Main REPL                                                            */
/* ------------------------------------------------------------------ */

static void kdb_repl(void)
{
    char line[KDB_LINE_MAX];
    char *argv[KDB_MAX_ARGS];

    for (;;) {
        kdb_puts("kdb> ");
        kdb_readline(line, sizeof(line));

        if (!line[0])
            continue;

        int argc = kdb_tokenize(line, argv, KDB_MAX_ARGS);
        if (!argc)
            continue;

        if (!strcmp(argv[0], "help")) {
            cmd_help();
        } else if (!strcmp(argv[0], "regs")) {
            cmd_regs();
        } else if (!strcmp(argv[0], "sysregs")) {
            cmd_sysregs();
        } else if (!strcmp(argv[0], "tasks") || !strcmp(argv[0], "ps")) {
            cmd_tasks();
        } else if (!strcmp(argv[0], "mem")) {
            cmd_mem();
        } else if (!strcmp(argv[0], "bt")) {
            cmd_bt();
        } else if (!strcmp(argv[0], "rd")) {
            cmd_rd(argc, argv);
        } else if (!strcmp(argv[0], "wr")) {
            cmd_wr(argc, argv);
        } else if (!strcmp(argv[0], "continue") || !strcmp(argv[0], "c")) {
            kdb_puts("  Resuming...\n\n");
            return;
        } else {
            printf("  Unknown command '%s' — type 'help'\n", argv[0]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                  */
/* ------------------------------------------------------------------ */

void kdb_enter(const char *reason)
{
    disable_interrupts();
    kdb_tf = NULL;

    printf("\n======== KDB: %s  [uptime %lu ms, core %d] ========\n", reason, get_system_time(),
           get_core_id());
    cmd_help();
    kdb_repl();
}

void kdb_enter_tf(const char *reason, struct exception_trap_frame *tf)
{
    disable_interrupts();
    kdb_tf = tf;

    printf("\n======== KDB: %s  [uptime %lu ms, core %d] ========\n", reason, get_system_time(),
           get_core_id());
    cmd_help();
    kdb_repl();
}
