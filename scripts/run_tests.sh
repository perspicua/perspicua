#!/usr/bin/env bash
#
# run_tests.sh - Boots a CONFIG_TESTS kernel headless and reports pass/fail.
#
# Two phases. The in-kernel suites run at boot and report over the serial
# console. Then the kernel execs init, and the userspace suites are typed at
# the resulting shell prompt -- they have to run there because a signal
# handler only executes on the way back to EL0, which a boot-phase kernel test
# task never reaches.
#
# A run that never reaches a marker (hang, panic, or timeout) is a failure,
# not a pass.

set -uo pipefail

KERNEL="${1:?usage: run_tests.sh <kernel.img> <dtb> <sdcard.img> [timeout_s]}"
DTB="${2:?missing dtb}"
SDCARD="${3:?missing sdcard image}"
TIMEOUT="${4:-180}"

for f in "$KERNEL" "$DTB" "$SDCARD"; do
    if [ ! -f "$f" ]; then
        echo "run_tests: missing required file: $f" >&2
        exit 1
    fi
done

if ! command -v qemu-system-aarch64 >/dev/null; then
    echo "run_tests: qemu-system-aarch64 is not on PATH" >&2
    exit 1
fi

KERNEL_MARKERS=(
    "reached target: kernel self-test complete"
    "reached target: scheduler test complete"
    "reached target: post-init test complete"
)
SHELL_MARKER="Type help to see available commands"
PANIC_MARKER="KERNEL PANIC"

# A kernel that prints one of these has gone wrong without panicking.
FAIL_MARKERS=(
    "lockdep: attempting to release unheld"
    "lockdep: node table full"
    "user abort with no owning process"
)

# Kernel suites as "<name>:<fewest tests>". Each must print "test: <name>: N tests [OK]"
# with N at least that; raise a minimum when a suite grows, never lower one to pass.
KERNEL_SUITES=(
    "Types:24"
    "String:104"
    "Symbols:8"
    "Spinlock:233"
    "Physical Memory Manager:790"
    "Slab Allocator:1253"
    "Heap Allocator:1230"
    "Heap Properties:16"
    "Timer:21"
    "SD Driver:545"
    "MMU Dynamic Mapping:46"
    "MMU Per-Process Page Tables:86"
    "VFS:89"
    "FAT32:1524"
    "FAT32 Corrupt Media:15"
    "Pipe:45"
    "Procfs:25"
    "Mutex:27"
    "Semaphore:34"
    "Completion:39"
    "RWSEM:17"
    "WaitQueue:84"
    "User Access:27"
    "Process:36"
    "ELF Loader:46"
    "Syscall Bounds:20"
    "Fault Injection:58"
    "Scheduler:107"
    "WaitQueue-MultiTask:119"
    "Semaphore-MultiTask:60"
    "Completion-MultiTask:36"
    "RWSEM-MultiTask:48"
    "Spinlock-MultiTask:3"
    "TTY:16"
    "Signals:54"
)

# Built, or grown, only with CONFIG_LOCKDEP, which the kernel announces with LOCKDEP_MARKER.
LOCKDEP_SUITES=(
    "Lockdep-Sleeping:55"
    "Lockdep-Spinlock:5"
    "RWSEM:22"
)
LOCKDEP_MARKER="lockdep: initialized kernel lock dependency validator"

# Userspace suites typed at the prompt in order, as "<command line>:<fewest tests>". The
# command's first word names the suite: it must print "<name>: all N tests passed" and exit 0.
USER_SUITES=(
    "test_restart:20"
    "test_tools:109"
    "test_libc:426"
    "test_syscall:187"
    "test_fcntl:8"
    "test_stop_cont:39"
    "test_sh:37"
    "stress -s 1:1290"
)

# Lines typed at the shell WITHOUT waiting for each echo, so they overlap in
# the UART FIFO -- a drained FIFO is exactly when a late interrupt acknowledge
# cannot drop a byte, so waiting for each echo detects nothing at all.
#
# Against a kernel with that bug put back this caught it in 6 of 18 runs, and
# in 0 of 8 against the fixed one. A failure here is real; a pass is not proof.
# It depends on timing: QEMU 10 and later hand the guest up to a FIFO of stdin
# at once, where 9.x handed it one byte, so a slow guest can overflow the tty's
# input ring. That loss is counted in /proc/tty, which a failure prints.
CONSOLE_BURST_LINES=20
CONSOLE_BURST_GAP=0.05
CONSOLE_BURST_PAD=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789

WORK="$(mktemp -d -t perspicua-tests.XXXXXX)" || exit 1
LOG="$WORK/log"
CLEAN="$WORK/log.clean"
FIFO="$WORK/stdin"
if ! mkfifo "$FIFO"; then
    echo "run_tests: cannot create $FIFO" >&2
    rm -rf "$WORK"
    exit 1
fi

# Each run boots its own copy, so a build or another run writing the image cannot reach it.
CARD="$WORK/sdcard.img"

qemu_pid=""
cleanup()
{
    if [ -n "$qemu_pid" ]; then
        kill "$qemu_pid" 2>/dev/null
    fi
    exec 3>&-
    rm -rf "$WORK"
}
trap cleanup EXIT

if ! cp "$SDCARD" "$CARD"; then
    echo "run_tests: cannot copy $SDCARD" >&2
    exit 1
fi

timed_out=0
started=$(date +%s)

# Waits for a pattern to appear in the log. Fails on panic, on QEMU exiting,
# or when the overall budget runs out.
wait_for()
{
    local pattern="$1"
    # Poll interval. The default suits waits measured in seconds; the console
    # burst does dozens of round trips and would otherwise pay a second each.
    local interval="${2:-1}"
    while true; do
        if grep -qE "$pattern" "$LOG" 2>/dev/null; then
            return 0
        fi
        if grep -qF "$PANIC_MARKER" "$LOG" 2>/dev/null; then
            return 1
        fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then
            return 1
        fi
        if [ $(( $(date +%s) - started )) -ge "$TIMEOUT" ]; then
            timed_out=1
            return 1
        fi
        sleep "$interval"
    done
}

echo "run_tests: booting $(basename "$KERNEL") (timeout ${TIMEOUT}s)"
echo

# Read-write, not write-only: opening a FIFO for writing blocks until a reader
# appears, and QEMU is not started yet. Holding it open also keeps QEMU's stdin
# from seeing EOF between commands.
exec 3<>"$FIFO"

qemu-system-aarch64 \
    -M raspi4b -serial stdio -display none \
    -dtb "$DTB" -kernel "$KERNEL" \
    -drive file="$CARD",format=raw,if=sd <"$FIFO" >"$LOG" 2>&1 &
qemu_pid=$!

for marker in "${KERNEL_MARKERS[@]}"; do
    wait_for "$marker" || break
done

# Waits up to secs for a line matching pattern, without the run's overall budget.
wait_briefly()
{
    local deadline=$(( $(date +%s) + $2 ))
    until tr -d '\r' <"$LOG" | grep -qE "$1"; do
        [ "$(date +%s)" -ge "$deadline" ] && return 1
        sleep 0.2
    done
}

# Output lines only: the shell's echo of a typed line carries the prompt in front of it.
burst_lines_back()
{
    tr -d '\r' <"$LOG" | grep -cxE "B[0-9]+-$CONSOLE_BURST_PAD-END"
}

console_burst_ran=0
suites_finished=0

if wait_for "$SHELL_MARKER"; then
    suites_finished=1
    for entry in "${USER_SUITES[@]}"; do
        cmd="${entry%:*}"
        name="${cmd%% *}"
        # The status line comes back whether the suite passed, failed or crashed.
        printf '%s; echo %s-rc=$?\n' "$cmd" "$name" >&3
        if ! wait_for "^$name-rc=[0-9]+"; then
            suites_finished=0
            break
        fi
    done
fi

# Job control at the live prompt; each of these once left the console dead.
job_control_failures=()
if [ "$suites_finished" -eq 1 ]; then
    printf '\032' >&3
    printf 'echo JC-CTRLZ\n' >&3
    wait_briefly '^JC-CTRLZ$' 10 || job_control_failures+=("Ctrl-Z at the prompt stopped the shell")

    # A terminal's size report that lands at the prompt is read whole and dropped, not typed.
    printf '\033[24;80Recho LE-CPR\n' >&3
    wait_briefly '^LE-CPR$' 10 || job_control_failures+=("a size report at the prompt became typed text")
    # Delete's sequence leaves nothing behind, and a key right after a lone Esc is still typed.
    printf 'echo LE-DEL-a\033[3~b\n' >&3
    wait_briefly '^LE-DEL-ab$' 10 || job_control_failures+=("Delete at the prompt typed its sequence")
    printf '\033echo LE-ESC\n' >&3
    wait_briefly '^LE-ESC$' 10 || job_control_failures+=("the key after a lone Esc was lost")

    printf 'sh &\n' >&3
    printf 'echo JC-BGSH\n' >&3
    wait_briefly '^JC-BGSH$' 10 || job_control_failures+=("a background sh took the terminal")

    # Ctrl-Z reaches the job only once the shell has handed it the terminal, so it is repeated.
    printf 'sleep 2 | sleep 4\n' >&3
    stopped=0
    for _ in $(seq 1 12); do
        sleep 0.3
        printf '\032' >&3
        if wait_briefly '^\[stopped\]' 0; then
            stopped=1
            break
        fi
    done
    if [ "$stopped" -eq 1 ] || wait_briefly '^\[stopped\]' 2; then
        # grep finds no sleep, and says so with status 1, only once fg has waited for both.
        printf 'fg; ps | grep sleep; echo JC-FG-$?\n' >&3
        if ! wait_briefly '^JC-FG-1$' 15; then
            job_control_failures+=("fg returned before the whole pipeline finished")
        fi
    else
        job_control_failures+=("Ctrl-Z did not stop a foreground pipeline")
    fi
fi

if [ "$suites_finished" -eq 1 ]; then
    # Sent at a fixed gap without waiting, so the lines pile up in the UART FIFO.
    console_burst_ran=1
    for i in $(seq 1 "$CONSOLE_BURST_LINES"); do
        printf 'echo B%d-%s-END\n' "$i" "$CONSOLE_BURST_PAD" >&3
        sleep "$CONSOLE_BURST_GAP"
    done

    # Bounded on its own rather than through wait_for, so a wedged console
    # fails in seconds instead of eating the whole run's budget.
    burst_deadline=$(( $(date +%s) + 20 ))
    until [ "$(burst_lines_back)" -ge "$CONSOLE_BURST_LINES" ]; do
        [ "$(date +%s)" -ge "$burst_deadline" ] && break
        sleep 0.2
    done
    # Where the bytes went: dropped from a full input ring, or lost at the UART.
    if [ "$(burst_lines_back)" -lt "$CONSOLE_BURST_LINES" ]; then
        printf 'cat /proc/tty\n' >&3
        wait_briefly '^break [0-9]+$' 10
    fi
fi

kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
qemu_pid=""

cat "$LOG"
tr -d '\r' <"$LOG" >"$CLEAN"

echo
echo "--------------------------------------------------------------------------"

status=0

# Prints the N of the first line containing prefix and ending in suffix, or nothing.
count_after()
{
    grep -F -- "$1" "$CLEAN" | sed -nE "s/.*$2/\1/p" | head -n 1
}

# Fails the run unless suite reported at least min tests.
check_count()
{
    local what="$1" n="$2" min="$3"
    if [ -z "$n" ]; then
        echo "FAIL: $what did not pass"
        status=1
    elif [ "$n" -lt "$min" ]; then
        echo "FAIL: $what ran $n tests, fewer than the $min expected"
        status=1
    fi
}

# A failing suite prints [FAILED]; a suite that never ran prints nothing at
# all, so completion is checked separately from failure.
if grep -q "\[FAILED\]" "$LOG"; then
    echo "FAIL: one or more test assertions failed"
    grep "\[FAILED\]" "$LOG" | sed 's/^/  /'
    status=1
fi

if grep -q "$PANIC_MARKER" "$LOG"; then
    echo "FAIL: kernel panicked during the run"
    status=1
fi

if [ "$timed_out" -eq 1 ]; then
    echo "FAIL: timed out after ${TIMEOUT}s without completing the test suites"
    status=1
fi

for marker in "${KERNEL_MARKERS[@]}"; do
    if ! grep -qF "$marker" "$LOG"; then
        echo "FAIL: never reached '$marker'"
        status=1
    fi
done

for marker in "${FAIL_MARKERS[@]}"; do
    if grep -qF "$marker" "$CLEAN"; then
        echo "FAIL: the kernel reported '$marker'"
        status=1
    fi
done

expected_kernel=("${KERNEL_SUITES[@]}")
if grep -qF "$LOCKDEP_MARKER" "$CLEAN"; then
    expected_kernel+=("${LOCKDEP_SUITES[@]}")
fi
for entry in "${expected_kernel[@]}"; do
    name="${entry%:*}"
    n="$(count_after "] test: $name: " ': ([0-9]+) tests \[OK\]$')"
    check_count "kernel suite '$name'" "$n" "${entry##*:}"
done

for entry in "${USER_SUITES[@]}"; do
    cmd="${entry%:*}"
    name="${cmd%% *}"
    rc="$(grep -E "^$name-rc=[0-9]+$" "$CLEAN" | head -n 1)"
    if [ -z "$rc" ]; then
        echo "FAIL: userspace suite '$name' never finished"
        status=1
    elif [ "$rc" != "$name-rc=0" ]; then
        echo "FAIL: userspace suite '$name' exited with status ${rc#*=}"
        status=1
    fi
    n="$(grep -E "^$name: all [0-9]+ tests passed$" "$CLEAN" | sed -E 's/.* all ([0-9]+) tests.*/\1/')"
    check_count "userspace suite '$name'" "$n" "${entry##*:}"
done

for failure in "${job_control_failures[@]}"; do
    echo "FAIL: job control: $failure"
    status=1
done

if [ "$console_burst_ran" -eq 1 ]; then
    burst_missing=0
    for i in $(seq 1 "$CONSOLE_BURST_LINES"); do
        grep -qxF "B$i-$CONSOLE_BURST_PAD-END" "$CLEAN" || burst_missing=$((burst_missing + 1))
    done
    if [ "$burst_missing" -gt 0 ]; then
        echo "FAIL: console burst: $burst_missing of $CONSOLE_BURST_LINES lines did not come back" \
            "($(qemu-system-aarch64 --version | head -n 1))"
        grep -E '^(rx_dropped|overrun|framing|parity|break) [0-9]+$' "$CLEAN" | sed 's/^/  \/proc\/tty: /'
        status=1
    fi
fi

if [ "$status" -eq 0 ]; then
    grep -E "all [0-9]+ tests passed" "$CLEAN" | sed 's/^/  /'
    echo "PASS: all kernel and userspace test suites passed"
fi

exit "$status"
