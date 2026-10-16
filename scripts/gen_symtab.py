#!/usr/bin/env python3
"""
gen_symtab.py - The kernel's symbol table, and the check that it fits the final link.

  gen_symtab.py table <nm> <pass1.elf> <out.c>
      Writes the text symbols of the first link as kernel/include/debug/ksym.h's table.

  gen_symtab.py check <nm> <pass1.elf> <final.elf>
      Fails, and deletes final.elf so the next build relinks, unless both links
      put every text symbol at the same address.
"""

import os
import subprocess
import sys


def text_symbols(nm, elf):
    try:
        out = subprocess.run([nm, "-n", elf], check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit(f"gen_symtab: {nm} -n {elf} failed: {e}")

    symbols = []
    for line in out.splitlines():
        parts = line.split()
        # $x and $d are mapping symbols: they mark code and data inside a function.
        if len(parts) == 3 and parts[1] in ("t", "T", "W") and not parts[2].startswith("$"):
            symbols.append((int(parts[0], 16), parts[2]))
    return symbols


def write_table(nm, elf, out_c):
    symbols = text_symbols(nm, elf)
    if not symbols:
        sys.exit(f"gen_symtab: no text symbols in {elf}")

    with open(out_c, "w") as f:
        f.write('#include "debug/ksym.h"\n\n')
        f.write(f"const unsigned long ksym_count = {len(symbols)};\n\n")
        f.write("const struct ksym ksym_table[] = {\n")
        for addr, name in symbols:
            f.write(f'    {{0x{addr:016x}UL, "{name}"}},\n')
        f.write("};\n")


def check(nm, pass1, final):
    first = text_symbols(nm, pass1)
    shipped = text_symbols(nm, final)
    if first == shipped:
        return

    for (a_addr, a_name), (b_addr, b_name) in zip(first, shipped):
        if (a_addr, a_name) != (b_addr, b_name):
            where = f"{a_name} at 0x{a_addr:x} in the first link, {b_name} at 0x{b_addr:x} in the final"
            break
    else:
        where = f"{len(first)} text symbols in the first link, {len(shipped)} in the final"

    os.remove(final)
    sys.exit(f"gen_symtab: text moved between the link passes ({where});"
             " backtraces would name the wrong functions")


if __name__ == "__main__":
    if len(sys.argv) != 5 or sys.argv[1] not in ("table", "check"):
        sys.exit("usage: gen_symtab.py table <nm> <pass1.elf> <out.c>\n"
                 "       gen_symtab.py check <nm> <pass1.elf> <final.elf>")
    if sys.argv[1] == "table":
        write_table(*sys.argv[2:])
    else:
        check(*sys.argv[2:])
