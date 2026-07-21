#!/usr/bin/env bash
#
# run_tests.sh - Boots a CONFIG_TESTS kernel headless and reports pass/fail.
#
# The in-kernel suites report their results over the serial console and the
# kernel then carries on into userspace, so QEMU is stopped as soon as the
# final completion marker appears rather than being left to time out. A run
# that never reaches those markers (hang, panic, or timeout) is a failure,
# not a pass.

set -uo pipefail

KERNEL="${1:?usage: run_tests.sh <kernel.img> <dtb> <sdcard.img> [timeout_s]}"
DTB="${2:?missing dtb}"
SDCARD="${3:?missing sdcard image}"
TIMEOUT="${4:-120}"

for f in "$KERNEL" "$DTB" "$SDCARD"; do
    if [ ! -f "$f" ]; then
        echo "run_tests: missing required file: $f" >&2
        exit 1
    fi
done

DONE_MARKER="reached target: post-init test complete"
PANIC_MARKER="KERNEL PANIC"

LOG="$(mktemp -t perspicua-tests.XXXXXX)"

qemu_pid=""
cleanup()
{
    if [ -n "$qemu_pid" ]; then
        kill "$qemu_pid" 2>/dev/null
    fi
    rm -f "$LOG"
}
trap cleanup EXIT

echo "run_tests: booting $(basename "$KERNEL") (timeout ${TIMEOUT}s)"
echo

qemu-system-aarch64 \
    -M raspi4b -serial stdio -display none \
    -dtb "$DTB" -kernel "$KERNEL" \
    -drive file="$SDCARD",format=raw,if=sd >"$LOG" 2>&1 &
qemu_pid=$!

timed_out=0
elapsed=0
while kill -0 "$qemu_pid" 2>/dev/null; do
    if grep -q "$DONE_MARKER" "$LOG" 2>/dev/null; then
        break
    fi
    if grep -q "$PANIC_MARKER" "$LOG" 2>/dev/null; then
        break
    fi
    if [ "$elapsed" -ge "$TIMEOUT" ]; then
        timed_out=1
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

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

for marker in "reached target: kernel self-test complete" \
              "reached target: scheduler test complete" "$DONE_MARKER"; do
    if ! grep -q "$marker" "$LOG"; then
        echo "FAIL: never reached '$marker'"
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    grep -E "all [0-9]+ tests passed" "$LOG" | sed 's/^/  /'
    echo "PASS: all in-kernel test suites passed"
fi

exit "$status"
