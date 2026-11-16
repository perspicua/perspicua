#!/usr/bin/env bash
#
# host_libc_test.sh - Builds libc/src for the host with every public name prefixed p_, and
# compares it with the host's libc over generated inputs (host_libc_test.c).
#

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="${CC:-cc}"
WORK="$(mktemp -d -t perspicua-host-libc.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

SOURCES=(printf string stdlib ctype)
CFLAGS=(-std=gnu11 -O2 -g -Wall -ffreestanding -fno-builtin -fno-stack-protector -Wno-ignored-attributes -Wno-format
        -I "$ROOT/libc/include" -I "$ROOT")

# The names the library defines, which the host would otherwise resolve to its own.
for name in "${SOURCES[@]}"; do
    "$CC" "${CFLAGS[@]}" -c "$ROOT/libc/src/$name.c" -o "$WORK/$name.plain.o"
done
strip_underscore=cat
case "$("$CC" -dumpmachine)" in
    *darwin*) strip_underscore="sed s/^_//" ;;
esac
defined="$(nm -g -P "$WORK"/*.plain.o | awk 'NF >= 3 && $2 != "U" && $2 != "u" {print $1}' \
    | $strip_underscore | sort -u)"

renames=(-Derrno=p_errno -D__libc_write=p___libc_write)
for sym in $defined; do
    renames+=("-D$sym=p_$sym")
done

objects=()
for name in "${SOURCES[@]}"; do
    "$CC" "${CFLAGS[@]}" "${renames[@]}" -c "$ROOT/libc/src/$name.c" -o "$WORK/$name.o"
    objects+=("$WORK/$name.o")
done

"$CC" -std=gnu11 -O2 -Wall "$ROOT/scripts/host_libc_test.c" "${objects[@]}" -o "$WORK/host_libc_test"
"$WORK/host_libc_test"
