#!/usr/bin/env bash
#
# run_boot_test.sh - Boots a kernel headless in QEMU and waits for the shell banner.
#

set -uo pipefail

if [ "$#" -ne 5 ]; then
    echo "usage: $(basename "$0") <kernel.img> <dtb> <sdcard.img> <timeout_s> <banner>" >&2
    exit 1
fi

KERNEL="$1"
DTB="$2"
SDCARD="$3"
TIMEOUT="$4"
BANNER="$5"

for f in "$KERNEL" "$DTB" "$SDCARD"; do
    if [ ! -f "$f" ]; then
        echo "run_boot_test: missing required file: $f" >&2
        exit 1
    fi
done

if ! command -v qemu-system-aarch64 >/dev/null; then
    echo "run_boot_test: qemu-system-aarch64 is not on PATH" >&2
    exit 1
fi

WORK="$(mktemp -d -t perspicua-boot.XXXXXX)" || exit 1
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
    echo "run_boot_test: cannot copy $SDCARD" >&2
    exit 1
fi

qemu-system-aarch64 \
    -M raspi4b -serial stdio -display none \
    -dtb "$DTB" -kernel "$KERNEL" \
    -drive file="$CARD",format=raw,if=sd </dev/null >"$LOG" 2>&1 &
qemu_pid=$!

started=$(date +%s)
while true; do
    if grep -aqF "$BANNER" "$LOG" 2>/dev/null; then
        echo "PASS"
        exit 0
    fi
    if grep -aqF "KERNEL PANIC" "$LOG" 2>/dev/null; then
        echo "FAIL: the kernel panicked before the shell started"
        break
    fi
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        echo "FAIL: QEMU exited before the shell started"
        break
    fi
    if [ $(( $(date +%s) - started )) -ge "$TIMEOUT" ]; then
        echo "FAIL: no shell banner after ${TIMEOUT}s"
        break
    fi
    sleep 0.1
done

echo "Last 20 lines of log:"
tail -n 20 "$LOG"
exit 1
