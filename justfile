# perspicua justfile

set shell := ["bash", "-c"]

# Build directory and toolchain
build_dir := "build"
toolchain := "cmake/aarch64-none-elf.cmake"

# Number of cores for parallel build
nproc := `nproc 2>/dev/null || sysctl -n hw.ncpu || echo 1`

# Default recipe
default: build

# Setup and build the project (defaults to Debug)
# Usage: just build [debug|release]
@build type="Debug":
    cmake -B {{build_dir}} -S . -DCMAKE_TOOLCHAIN_FILE={{toolchain}} -DCMAKE_BUILD_TYPE={{type}} -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cmake --build {{build_dir}} -j {{nproc}}
    ln -sf {{build_dir}}/compile_commands.json compile_commands.json
    echo "Build ({{type}}) complete: pi4-boot/kernel8.img"

# Run the kernel in QEMU
@run: build
    cmake --build {{build_dir}} --target run

# Run the kernel in QEMU with GUI (serial vc)
@run-gui: build
    cmake --build {{build_dir}} --target run-gui

# Run the kernel in QEMU with debug flags (waits for GDB)
@debug: build
    cmake --build {{build_dir}} --target debug

# Connect to QEMU via GDB
@gdb: build
    cmake --build {{build_dir}} --target gdb

# Show disassembled kernel
@disasm: build
    cmake --build {{build_dir}} --target disasm


# Clean build artifacts
@clean:
    rm -rf {{build_dir}} compile_commands.json sdcard.img

# Show kernel binary size
@size: build
    cmake --build {{build_dir}} --target size

# List kernel symbols
@symbols: build
    cmake --build {{build_dir}} --target symbols

# Re-format code
@format:
    find kernel libc uapi user -name "*.c" -o -name "*.h" | xargs clang-format -i

# Check formatting
@check-format:
    find kernel libc uapi user -name "*.c" -o -name "*.h" | xargs clang-format --dry-run --Werror
