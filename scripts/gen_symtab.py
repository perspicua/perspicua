import subprocess
import sys
import os

def create_symbol_table(elf_file, output_c):
    try:
        output = subprocess.check_output(["nm", "-n", elf_file]).decode("utf-8")
    except Exception as e:
        print(f"Error running nm: {e}")
        with open(output_c, "w") as f:
            f.write("const int kernel_symbol_count = 0;\n")
            f.write("const void* kernel_symbols = 0;\n")
            f.write("const char* panic_resolve_symbol(unsigned long addr, unsigned long *offset) { return 0; }\n")
        return

    symbols = []
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            addr_str = parts[0]
            type_char = parts[1]
            name = parts[2]
            if type_char.lower() in ("t", "T"):
                addr = int(addr_str, 16)
                symbols.append((addr, name))
                
    with open(output_c, "w") as f:
        f.write("#include \"panic.h\"\n\n")
        f.write("struct kernel_symbol {\n")
        f.write("    unsigned long addr;\n")
        f.write("    const char *name;\n")
        f.write("};\n\n")
        f.write(f"const int kernel_symbol_count = {len(symbols)};\n\n")
        f.write("const struct kernel_symbol kernel_symbols[] = {\n")
        for addr, name in symbols:
            f.write(f"    {{ 0x{addr:016x}, \"{name}\" }},\n")
        f.write("};\n\n")
        f.write("""
const char* panic_resolve_symbol(unsigned long addr, unsigned long *offset) {
    if (kernel_symbol_count == 0) return 0;
    
    int low = 0, high = kernel_symbol_count - 1;
    int best = -1;
    
    while (low <= high) {
        int mid = (low + high) / 2;
        if (kernel_symbols[mid].addr <= addr) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    
    if (best >= 0) {
        *offset = addr - kernel_symbols[best].addr;
        return kernel_symbols[best].name;
    }
    return 0;
}
""")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: gen_symtab.py <elf_file> <output_c>")
        sys.exit(1)
    create_symbol_table(sys.argv[1], sys.argv[2])
