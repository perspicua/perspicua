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

KERNEL_MARKERS=(
    "reached target: kernel self-test complete"
    "reached target: scheduler test complete"
    "reached target: post-init test complete"
)
SHELL_MARKER="Type help to see available commands"
PANIC_MARKER="KERNEL PANIC"

# Userspace suites, run in order at the shell prompt. Each must print
# "<name>: all N tests passed"; add a program here to have it gated.
USER_SUITES=(test_restart)

# Lines typed at the shell WITHOUT waiting for each echo, so they overlap in
# the UART FIFO -- a drained FIFO is exactly when a late interrupt acknowledge
# cannot drop a byte, so waiting for each echo detects nothing at all.
#
# Against a kernel with that bug put back this caught it in 6 of 18 runs, and
# in 0 of 8 against the fixed one. A failure here is real; a pass is not proof.
CONSOLE_BURST_LINES=20
CONSOLE_BURST_GAP=0.05
CONSOLE_BURST_PAD=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789

LOG="$(mktemp -t perspicua-tests.XXXXXX)"
FIFO="$(mktemp -u -t perspicua-stdin.XXXXXX)"
mkfifo "$FIFO"

qemu_pid=""
cleanup()
{
    if [ -n "$qemu_pid" ]; then
        kill "$qemu_pid" 2>/dev/null
    fi
    exec 3>&-
    rm -f "$LOG" "$FIFO"
}
trap cleanup EXIT

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
    -drive file="$SDCARD",format=raw,if=sd <"$FIFO" >"$LOG" 2>&1 &
qemu_pid=$!

for marker in "${KERNEL_MARKERS[@]}"; do
    wait_for "$marker" || break
done

console_burst_ran=0

if wait_for "$SHELL_MARKER"; then
    for suite in "${USER_SUITES[@]}"; do
        printf '%s\n' "$suite" >&3
        # Matches a pass or a failure, so a failing suite reports at once
        # instead of waiting out the budget.
        wait_for "^$suite: (all [0-9]+ tests passed|[0-9]+ of [0-9]+ tests failed)" || break
    done

    # Each line goes out as a single write and must come back before the next
    # one is sent: a dropped byte shows up as the echo that never arrives.
    console_burst_ran=1
    for i in $(seq 1 "$CONSOLE_BURST_LINES"); do
        printf 'echo B%d-%s-END\n' "$i" "$CONSOLE_BURST_PAD" >&3
        sleep "$CONSOLE_BURST_GAP"
    done

    # Bounded on its own rather than through wait_for, so a wedged console
    # fails in seconds instead of eating the whole run's budget.
    burst_deadline=$(( $(date +%s) + 20 ))
    until grep -qF "B$CONSOLE_BURST_LINES-$CONSOLE_BURST_PAD-END" "$LOG"; do
        [ "$(date +%s)" -ge "$burst_deadline" ] && break
        sleep 0.2
    done
fi

kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
qemu_pid=""

cat "$LOG"

echo
echo "--------------------------------------------------------------------------"

status=0

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

for suite in "${USER_SUITES[@]}"; do
    if ! grep -qE "^$suite: all [0-9]+ tests passed" "$LOG"; then
        echo "FAIL: userspace suite '$suite' did not pass"
        status=1
    fi
done

if [ "$console_burst_ran" -eq 1 ]; then
    burst_missing=0
    for i in $(seq 1 "$CONSOLE_BURST_LINES"); do
        grep -qF "B$i-$CONSOLE_BURST_PAD-END" "$LOG" || burst_missing=$((burst_missing + 1))
    done
    if [ "$burst_missing" -gt 0 ]; then
        echo "FAIL: $burst_missing of $CONSOLE_BURST_LINES console lines came back garbled or not at all"
        status=1
    fi
fi

if [ "$status" -eq 0 ]; then
    grep -E "all [0-9]+ tests passed" "$LOG" | sed 's/^/  /'
    echo "PASS: all kernel and userspace test suites passed"
fi

exit "$status"
