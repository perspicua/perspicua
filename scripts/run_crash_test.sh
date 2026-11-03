#!/usr/bin/env bash
#
# run_crash_test.sh - Boots a crash-test kernel headless and asserts expected panic output.
#

set -uo pipefail

if [ "$#" -lt 5 ]; then
    echo "usage: $(basename "$0") <kernel.img> <dtb> <sdcard.img> <timeout_s> <pattern> [pattern...]" >&2
    exit 1
fi

KERNEL="$1"
DTB="$2"
SDCARD="$3"
TIMEOUT="$4"
shift 4
PATTERNS=("$@")

for f in "$KERNEL" "$DTB" "$SDCARD"; do
    if [ ! -f "$f" ]; then
        echo "run_crash_test: missing required file: $f" >&2
        exit 1
    fi
done

if ! command -v qemu-system-aarch64 >/dev/null; then
    echo "run_crash_test: qemu-system-aarch64 is not on PATH" >&2
    exit 1
fi

WORK="$(mktemp -d -t perspicua-crash.XXXXXX)" || exit 1
LOG="$WORK/log"
CARD="$WORK/sdcard.img"

qemu_pid=""
cleanup()
{
    if [ -n "$qemu_pid" ]; then
        kill "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

if ! cp "$SDCARD" "$CARD"; then
    echo "run_crash_test: cannot copy $SDCARD" >&2
    exit 1
fi

qemu-system-aarch64 \
    -M raspi4b -serial stdio -display none \
    -dtb "$DTB" -kernel "$KERNEL" \
    -drive file="$CARD",format=raw,if=sd </dev/null >"$LOG" 2>&1 &
qemu_pid=$!

PANIC_MARKER="KERNEL PANIC"
HALT_MARKER="All CPU cores halted"
started=$(date +%s)
timed_out=0
panicked=0
halted=0

while true; do
    if grep -qF "$PANIC_MARKER" "$LOG" 2>/dev/null; then
        panicked=1
        break
    fi
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        break
    fi
    if [ $(( $(date +%s) - started )) -ge "$TIMEOUT" ]; then
        timed_out=1
        break
    fi
    sleep 0.1
done

if [ "$panicked" -eq 1 ]; then
    while true; do
        if grep -qF "$HALT_MARKER" "$LOG" 2>/dev/null; then
            halted=1
            break
        fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then
            break
        fi
        if [ $(( $(date +%s) - started )) -ge "$TIMEOUT" ]; then
            timed_out=1
            break
        fi
        sleep 0.05
    done
fi

if [ -n "$qemu_pid" ]; then
    kill "$qemu_pid" 2>/dev/null || true
    wait "$qemu_pid" 2>/dev/null || true
    qemu_pid=""
fi

if [ "$timed_out" -eq 1 ] && [ "$panicked" -eq 0 ]; then
    echo "FAIL: timed out after ${TIMEOUT}s without a kernel panic"
    echo "Last 20 lines of log:"
    tail -n 20 "$LOG"
    exit 1
fi

if [ "$panicked" -eq 0 ]; then
    echo "FAIL: QEMU exited without a kernel panic"
    echo "Last 20 lines of log:"
    tail -n 20 "$LOG"
    exit 1
fi

if [ "$halted" -eq 0 ]; then
    echo "FAIL: panic report did not finish"
    echo "Last 20 lines of log:"
    tail -n 20 "$LOG"
    exit 1
fi

# An overflow must be caught before SP descends past the guard page.
overflow=$(grep -o 'SP=0x[0-9a-f]*, stack 0x[0-9a-f]*' "$LOG" | head -n 1)
if [ -n "$overflow" ]; then
    sp=${overflow#SP=}
    sp=${sp%%,*}
    base=${overflow##* }
    depth=$(( base - sp ))
    if [ "$depth" -gt 4096 ]; then
        echo "FAIL: overflow SP $sp is below the guard page under $base"
        exit 1
    fi
fi

missing=0
for pat in "${PATTERNS[@]}"; do
    if ! grep -qF "$pat" "$LOG"; then
        missing=1
        break
    fi
done

if [ "$missing" -eq 0 ]; then
    echo "PASS"
    exit 0
else
    echo "FAIL: panic output missing expected pattern(s)"
    echo "Expected patterns:"
    for pat in "${PATTERNS[@]}"; do
        if ! grep -qF "$pat" "$LOG"; then
            echo "  [MISSING] $pat"
        else
            echo "  [FOUND]   $pat"
        fi
    done
    echo
    echo "Panic output:"
    panic_block=$(sed -n '/crash-test:/,$p' "$LOG")
    if [ -n "$panic_block" ]; then
        echo "$panic_block"
    else
        cat "$LOG"
    fi
    exit 1
fi
