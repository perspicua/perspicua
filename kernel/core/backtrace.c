/*
 * backtrace.c - Bounded stack backtrace walker for panic and kdb.
 */

#include "debug/backtrace.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/exception.h"
#include "panic.h"
#include "sched/sched.h"

extern char _stack_bottom[];
extern char _stack_top[];

int backtrace_frame_ok(unsigned long fp)
{
    if ((fp & 0x7UL) != 0) {
        return 0;
    }

    // Usable kernel stack of the current task: [base + guard, top)
    struct task *t = sched_current_task();
    if (t && t->stack) {
        uintptr_t task_base = (uintptr_t)t->stack + SCHED_STACK_GUARD_PAGES * PAGE_SIZE;
        uintptr_t task_top = kstack_top(t->stack);
        if (fp >= task_base && fp + 16 <= task_top) {
            return 1;
        }
    }

    // Boot stacks from linker script: [_stack_bottom, _stack_top)
    uintptr_t boot_base = (uintptr_t)_stack_bottom;
    uintptr_t boot_top = (uintptr_t)_stack_top;
    if (fp >= boot_base && fp + 16 <= boot_top) {
        return 1;
    }

    // Per-core dedicated exception stacks
    for (int c = 0; c < CPU_MAX_CORES; c++) {
        uintptr_t exc_base = (uintptr_t)exception_stacks[c];
        uintptr_t exc_top = exc_base + EXCEPTION_STACK_SIZE;
        if (fp >= exc_base && fp + 16 <= exc_top) {
            return 1;
        }
    }

    return 0;
}

static void emit_frame_line(backtrace_printf_fn print_fn, int line_idx, unsigned long ret_addr,
                            int run_count)
{
    unsigned long offset = 0;
    const char *sym = panic_resolve_symbol(ret_addr, &offset);

    if (sym) {
        if (run_count > 1) {
            print_fn("  #%-2d  0x%016lx <%s+0x%lx>  x%d\n", line_idx, ret_addr, sym, offset,
                     run_count);
        } else {
            print_fn("  #%-2d  0x%016lx <%s+0x%lx>\n", line_idx, ret_addr, sym, offset);
        }
    } else {
        if (run_count > 1) {
            print_fn("  #%-2d  0x%016lx  x%d\n", line_idx, ret_addr, run_count);
        } else {
            print_fn("  #%-2d  0x%016lx\n", line_idx, ret_addr);
        }
    }
}

void backtrace_print(unsigned long fp, int max_lines, backtrace_printf_fn print_fn)
{
    if (!fp) {
        print_fn("  (frame pointer is NULL)\n");
        return;
    }

    if (!backtrace_frame_ok(fp)) {
        print_fn("  [frame pointer 0x%016lx outside known stack]\n", fp);
        return;
    }

    unsigned long cur_ret = 0;
    int run_count = 0;
    int line_idx = 0;

    while (backtrace_frame_ok(fp)) {
        unsigned long *frame = (unsigned long *)fp;
        unsigned long next_fp = frame[0];
        unsigned long ret_addr = frame[1];

        if (run_count == 0) {
            cur_ret = ret_addr;
            run_count = 1;
        } else if (ret_addr == cur_ret) {
            run_count++;
        } else {
            if (line_idx < max_lines) {
                emit_frame_line(print_fn, line_idx, cur_ret, run_count);
            }
            line_idx++;
            cur_ret = ret_addr;
            run_count = 1;
        }

        if (next_fp <= fp) {
            fp = next_fp;
            break;
        }
        fp = next_fp;
    }

    if (run_count > 0) {
        if (line_idx < max_lines) {
            emit_frame_line(print_fn, line_idx, cur_ret, run_count);
        }
        line_idx++;
    }

    if (fp != 0 && !backtrace_frame_ok(fp)) {
        print_fn("  [frame pointer 0x%016lx outside known stack]\n", fp);
    }
}
