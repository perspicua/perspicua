"""
gdb_helpers.py - Custom GDB commands for perspicua kernel debugging.

Load in GDB with:  source scripts/gdb_helpers.py
Or automatically via .gdbinit.
"""

import gdb

# --- Constants matching kernel headers ---

TASK_STATE_NAMES = {
    0: "RUNNING",
    1: "READY",
    2: "BLOCKED",
    3: "STOPPED",
    4: "DEAD",
}

# A free process slot is a NULL entry, not a state.
PROCESS_STATE_NAMES = {
    1: "RUNNING",
    2: "ZOMBIE",
    3: "DEAD",
}

# Physical to kernel virtual: P2V in kernel/include/mm/addr.h.
KERNEL_VMA = 0xFFFFFF8000000000

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


def array_len(name):
    """Element count of a kernel array, from its symbol."""
    return int(gdb.parse_and_eval(f"sizeof({name}) / sizeof({name}[0])"))


def pte_flags(entry):
    """Permission flags of a leaf entry, as one string."""
    flags = []
    if entry & AP_USER:
        flags.append("USER")
    flags.append("RO" if entry & AP_RO else "RW")
    if not (entry & PTE_UXN):
        flags.append("X")
    if entry & PTE_COW:
        flags.append("COW")
    return " ".join(flags)


def process_at(index):
    """The struct process in a table slot, or None for a free slot."""
    p = gdb.parse_and_eval(f"process_table[{index}]")
    return p.dereference() if int(p) != 0 else None


class TasksCommand(gdb.Command):
    """Dump all scheduler tasks across all CPU cores.

    Usage: tasks
    """

    def __init__(self):
        super().__init__("tasks", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        cores = array_len("core_sched_stats")

        # Each core is a gdb thread; TPIDR_EL1 holds its current task.
        gdb.write("=== Per-Core Current Tasks ===\n")
        gdb.write(f"{'Core':<6} {'TaskID':<8} {'PID':<6} {'State':<10} {'SP':<18}\n")
        gdb.write("-" * 52 + "\n")

        saved = gdb.selected_thread()
        for thread in sorted(gdb.selected_inferior().threads(), key=lambda t: t.num):
            core = thread.num - 1
            try:
                thread.switch()
                t = gdb.parse_and_eval("(struct task *)$TPIDR_EL1")
                if int(t) == 0:
                    gdb.write(f"{core:<6} (no task yet)\n")
                    continue
                tid = int(t["id"])
                pid = int(t["pid"])
                state = TASK_STATE_NAMES.get(int(t["state"]), "?")
                sp = int(t["context"]["sp"])
                gdb.write(f"{core:<6} {tid:<8} {pid:<6} {state:<10} {sp:#018x}\n")
            except gdb.error:
                gdb.write(f"{core:<6} (unavailable)\n")
        if saved is not None and saved.is_valid():
            saved.switch()

        gdb.write("\n=== Ready Queues ===\n")
        for core in range(cores):
            try:
                t = gdb.parse_and_eval(f"sched_rq_head[{core}]")
                if int(t) == 0:
                    continue
                gdb.write(f"  CPU{core}: ")
                tasks = []
                while int(t) != 0:
                    tid = int(t["id"])
                    pid = int(t["pid"])
                    state = TASK_STATE_NAMES.get(int(t["state"]), "?")
                    tasks.append(f"T{tid}(pid={pid},{state})")
                    t = t["rq_next"]
                gdb.write(" -> ".join(tasks) + "\n")
            except gdb.error:
                pass

        gdb.write("\n=== Sleep Queue ===\n")
        try:
            t = gdb.parse_and_eval("sched_sleep_head")
            if int(t) == 0:
                gdb.write("  (empty)\n")
            while int(t) != 0:
                tid = int(t["id"])
                pid = int(t["pid"])
                wake = int(t["wake_time"])
                gdb.write(f"  T{tid}(pid={pid}) wake_at={wake}\n")
                t = t["sleep_next"]
        except gdb.error:
            gdb.write("  (unavailable)\n")

        gdb.write("\n=== Scheduler Stats ===\n")
        gdb.write(f"{'Core':<6} {'Switches':<12} {'Idle':<12}\n")
        gdb.write("-" * 30 + "\n")
        for core in range(cores):
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

        for i in range(array_len("process_table")):
            try:
                p = process_at(i)
                if p is None:
                    continue

                pid = int(p["pid"])
                name = p["name"].string()
                state_str = PROCESS_STATE_NAMES.get(int(p["state"]), "?")
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
            if not 0 <= pid < array_len("process_table"):
                gdb.write(f"PID {pid}: out of range\n")
                return
            p = process_at(pid)
            if p is None:
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
            # 39-bit VA: user_pgd is the L1 table, an L2 entry may be a 2 MB block.
            for l1i in range(512):
                l1e = read_ulong(pgd_ptr + l1i * 8)
                if not (l1e & PTE_VALID):
                    continue

                l2_base = (l1e & PTE_ADDR_MASK) + KERNEL_VMA
                for l2i in range(512):
                    l2e = read_ulong(l2_base + l2i * 8)
                    if not (l2e & PTE_VALID):
                        continue

                    va = (l1i << 30) | (l2i << 21)
                    if not (l2e & PTE_TABLE):
                        pa = l2e & PTE_ADDR_MASK
                        gdb.write(f"{va:#018x}   {pa:#018x}   {pte_flags(l2e)} (2MB)\n")
                        count += 512
                        continue

                    l3_base = (l2e & PTE_ADDR_MASK) + KERNEL_VMA
                    for l3i in range(512):
                        l3e = read_ulong(l3_base + l3i * 8)
                        if not (l3e & PTE_VALID):
                            continue

                        pa = l3e & PTE_ADDR_MASK
                        gdb.write(f"{va | (l3i << 12):#018x}   {pa:#018x}   {pte_flags(l3e)}\n")
                        count += 1
        except gdb.error as e:
            gdb.write(f"\n(walk aborted: {e})\n")

        gdb.write(f"\nTotal: {count} mapped pages ({count * 4} KB)\n")


# Register all commands
TasksCommand()
ProcessesCommand()
PageTableCommand()

gdb.write("perspicua GDB helpers loaded: tasks, ps, pagetable\n")
