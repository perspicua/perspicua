"""
gdb_helpers.py - Custom GDB commands for perspicua kernel debugging.

Load in GDB with:  source scripts/gdb_helpers.py
Or automatically via .gdbinit.
"""

import gdb

# --- Constants matching kernel headers ---

SCHED_NUM_CORES = 4
PROCESS_TABLE_SIZE = 1024

TASK_STATE_NAMES = {
    0: "RUNNING",
    1: "READY",
    2: "BLOCKED",
    3: "DEAD",
}

PROCESS_STATE_NAMES = {
    0: "EMPTY",
    1: "RUNNING",
    2: "ZOMBIE",
    3: "DEAD",
}

# MMU PTE bits
PTE_VALID    = 1 << 0
PTE_TABLE    = 1 << 1
PTE_PAGE     = 1 << 1
PTE_AF       = 1 << 10
AP_USER      = 1 << 6
AP_RO        = 1 << 7
PTE_PXN      = 1 << 53
PTE_UXN      = 1 << 54
PTE_COW      = 1 << 55
PTE_ADDR_MASK = 0x0000FFFFFFFFF000


def read_ulong(addr):
    """Read an unsigned long from a virtual address."""
    return int(gdb.parse_and_eval(f"*(unsigned long *){addr:#x}"))


class TasksCommand(gdb.Command):
    """Dump all scheduler tasks across all CPU cores.

    Usage: tasks
    """

    def __init__(self):
        super().__init__("tasks", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        # Print per-core current task
        gdb.write("=== Per-Core Current Tasks ===\n")
        gdb.write(f"{'Core':<6} {'TaskID':<8} {'PID':<6} {'State':<10} {'SP':<18}\n")
        gdb.write("-" * 52 + "\n")

        for core in range(SCHED_NUM_CORES):
            try:
                pid = int(gdb.parse_and_eval(f"sched_core_pid[{core}]"))
                gdb.write(f"{core:<6} {'--':<8} {pid:<6}\n")
            except gdb.error:
                gdb.write(f"{core:<6} (unavailable)\n")

        # Print all tasks in ready queues
        gdb.write("\n=== Ready Queues ===\n")
        for core in range(SCHED_NUM_CORES):
            try:
                head = gdb.parse_and_eval(f"sched_rq_head[{core}]")
                if int(head) == 0:
                    continue
                gdb.write(f"  CPU{core}: ")
                t = head
                tasks = []
                while int(t) != 0:
                    tid = int(t["id"])
                    pid = int(t["pid"])
                    state = TASK_STATE_NAMES.get(int(t["state"]), "?")
                    tasks.append(f"T{tid}(pid={pid},{state})")
                    t = t["next"]
                gdb.write(" -> ".join(tasks) + "\n")
            except gdb.error:
                pass

        # Print sleep queue
        gdb.write("\n=== Sleep Queue ===\n")
        try:
            head = gdb.parse_and_eval("sched_sleep_head")
            if int(head) == 0:
                gdb.write("  (empty)\n")
            else:
                t = head
                while int(t) != 0:
                    tid = int(t["id"])
                    pid = int(t["pid"])
                    wake = int(t["wake_time"])
                    gdb.write(f"  T{tid}(pid={pid}) wake_at={wake}\n")
                    t = t["next"]
        except gdb.error:
            gdb.write("  (unavailable)\n")

        # Print scheduler stats
        gdb.write("\n=== Scheduler Stats ===\n")
        gdb.write(f"{'Core':<6} {'Switches':<12} {'Idle':<12}\n")
        gdb.write("-" * 30 + "\n")
        for core in range(SCHED_NUM_CORES):
            try:
                stats = gdb.parse_and_eval(f"core_sched_stats[{core}]")
                switches = int(stats["context_switches"])
                idle = int(stats["idle_count"])
                gdb.write(f"{core:<6} {switches:<12} {idle:<12}\n")
            except gdb.error:
                gdb.write(f"{core:<6} (unavailable)\n")


class ProcessesCommand(gdb.Command):
    """Dump the process table.

    Usage: ps [verbose]
    """

    def __init__(self):
        super().__init__("ps", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        verbose = "verbose" in arg

        gdb.write(f"{'PID':<6} {'Name':<20} {'State':<10} {'PPID':<6} {'ASID':<6}")
        if verbose:
            gdb.write(f" {'PGD':<18} {'Code VA':<18} {'Stack VA':<18}")
        gdb.write("\n")
        gdb.write("-" * (48 + (54 if verbose else 0)) + "\n")

        for i in range(PROCESS_TABLE_SIZE):
            try:
                p = gdb.parse_and_eval(f"process_table[{i}]")
                state = int(p["state"])
                if state == 0:  # EMPTY
                    continue

                pid = int(p["pid"])
                name = p["name"].string()
                state_str = PROCESS_STATE_NAMES.get(state, "?")
                ppid = int(p["parent_pid"])
                asid = int(p["asid"])

                gdb.write(f"{pid:<6} {name:<20} {state_str:<10} {ppid:<6} {asid:<6}")

                if verbose:
                    pgd = int(p["user_pgd"])
                    code_va = int(p["vaddr_code"])
                    stack_va = int(p["vaddr_user_stack"])
                    gdb.write(f" {pgd:#018x} {code_va:#018x} {stack_va:#018x}")

                gdb.write("\n")
            except gdb.error:
                continue


class PageTableCommand(gdb.Command):
    """Walk and dump a process's user page table.

    Usage: pagetable <pid>
    """

    def __init__(self):
        super().__init__("pagetable", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        if not arg.strip():
            gdb.write("Usage: pagetable <pid>\n")
            return

        pid = int(arg.strip())

        try:
            p = gdb.parse_and_eval(f"process_table[{pid}]")
            if int(p["state"]) == 0:
                gdb.write(f"PID {pid}: not active\n")
                return

            pgd_ptr = int(p["user_pgd"])
            if pgd_ptr == 0:
                gdb.write(f"PID {pid}: no page table\n")
                return
        except gdb.error as e:
            gdb.write(f"Error reading process: {e}\n")
            return

        gdb.write(f"Page table for PID {pid} (PGD @ {pgd_ptr:#x}):\n")
        gdb.write(f"{'Virtual Address':<20} {'Physical Address':<20} {'Flags'}\n")
        gdb.write("-" * 64 + "\n")

        count = 0
        try:
            # Walk L0 (512 entries)
            for l0i in range(512):
                l0e = read_ulong(pgd_ptr + l0i * 8)
                if not (l0e & PTE_VALID):
                    continue

                l1_base = l0e & PTE_ADDR_MASK
                # Walk L1
                for l1i in range(512):
                    l1e = read_ulong(l1_base + l1i * 8)
                    if not (l1e & PTE_VALID):
                        continue

                    l2_base = l1e & PTE_ADDR_MASK
                    # Walk L2
                    for l2i in range(512):
                        l2e = read_ulong(l2_base + l2i * 8)
                        if not (l2e & PTE_VALID):
                            continue

                        l3_base = l2e & PTE_ADDR_MASK
                        # Walk L3 (4KB pages)
                        for l3i in range(512):
                            l3e = read_ulong(l3_base + l3i * 8)
                            if not (l3e & PTE_VALID):
                                continue

                            va = (l0i << 39) | (l1i << 30) | (l2i << 21) | (l3i << 12)
                            pa = l3e & PTE_ADDR_MASK

                            flags = []
                            if l3e & AP_USER:
                                flags.append("USER")
                            if l3e & AP_RO:
                                flags.append("RO")
                            else:
                                flags.append("RW")
                            if not (l3e & PTE_UXN):
                                flags.append("X")
                            if l3e & PTE_COW:
                                flags.append("COW")

                            gdb.write(f"{va:#018x}   {pa:#018x}   {' '.join(flags)}\n")
                            count += 1
        except gdb.error as e:
            gdb.write(f"\n(walk aborted: {e})\n")

        gdb.write(f"\nTotal: {count} mapped pages ({count * 4} KB)\n")


class IRQStatsCommand(gdb.Command):
    """Dump per-core IRQ statistics.

    Usage: irqstats
    """

    def __init__(self):
        super().__init__("irqstats", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        gdb.write(f"{'Core':<6} {'Timer IRQs':<14} {'UART IRQs':<14}\n")
        gdb.write("-" * 34 + "\n")

        for core in range(SCHED_NUM_CORES):
            try:
                stats = gdb.parse_and_eval(f"core_irq_stats[{core}]")
                timer = int(stats["timer_count"])
                uart = int(stats["uart_count"])
                gdb.write(f"{core:<6} {timer:<14} {uart:<14}\n")
            except gdb.error:
                gdb.write(f"{core:<6} (unavailable)\n")


# Register all commands
TasksCommand()
ProcessesCommand()
PageTableCommand()
IRQStatsCommand()

gdb.write("perspicua GDB helpers loaded: tasks, ps, pagetable, irqstats\n")
