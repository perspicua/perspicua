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
        sleep 1
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

if wait_for "$SHELL_MARKER"; then
    for suite in "${USER_SUITES[@]}"; do
        printf '%s\n' "$suite" >&3
        # Matches a pass or a failure, so a failing suite reports at once
        # instead of waiting out the budget.
        wait_for "^$suite: (all [0-9]+ tests passed|[0-9]+ of [0-9]+ tests failed)" || break
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

if [ "$status" -eq 0 ]; then
    grep -E "all [0-9]+ tests passed" "$LOG" | sed 's/^/  /'
    echo "PASS: all kernel and userspace test suites passed"
fi

exit "$status"
