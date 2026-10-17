# perspicua justfile

set shell := ["bash", "-c"]

# Build directory and toolchain
build_dir := "build"
# Test builds use a separate tree so toggling CONFIG_TESTS never forces a
# full reconfigure and rebuild of the normal one.
test_build_dir := "build-test"
# The toolchain file takes the first of aarch64-none-elf-, aarch64-elf- and
# aarch64-linux-gnu- on PATH; name one with cmake_args="-DCROSS_COMPILE=aarch64-elf-"
# when a tree is first configured.
toolchain := "cmake/aarch64-none-elf.cmake"

# Seconds the headless test run may take before it counts as hung.
test_timeout := "180"

# Number of cores for parallel build
nproc := `nproc 2>/dev/null || sysctl -n hw.ncpu || echo 1`

# Default recipe
default: build

# Kernel configuration lives in CMakeLists.txt, which is the only list of it.
# Override from here by passing cmake flags, e.g.
#   just cmake_args="-DCONFIG_NR_CPUS=2 -DCONFIG_LOCKDEP=OFF" build
# An override lasts for that one command: the next one without it is back to the defaults.
cmake_args := ""

# Configure and build into a given tree with a given CONFIG_TESTS setting. The cached
# CONFIG_ options are dropped first, so the tree builds what this command asked for.
@_cmake dir type tests:
    cmake -B {{dir}} -S . \
        -U 'CONFIG_*' \
        -DCMAKE_TOOLCHAIN_FILE={{toolchain}} \
        -DCMAKE_BUILD_TYPE={{type}} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCONFIG_TESTS={{tests}} \
        {{cmake_args}}
    cmake --build {{dir}} -j {{nproc}}

# Build the project and publish the kernel to pi4-boot, the only place that copies it there
# Usage: just build [RelWithDebInfo|Debug|Release]  (default RelWithDebInfo: -O2 -g)
@build type="RelWithDebInfo": (_cmake build_dir type "OFF")
    cp {{build_dir}}/kernel/kernel8.img pi4-boot/kernel8.img
    ln -sf {{build_dir}}/compile_commands.json compile_commands.json
    echo "Build ({{type}}) complete: pi4-boot/kernel8.img"

# Build with the in-kernel test suites enabled
# Usage: just build-tests [RelWithDebInfo|Debug|Release]
@build-tests type="RelWithDebInfo": (_cmake test_build_dir type "ON")
    echo "Test build ({{type}}) complete: {{test_build_dir}}/kernel/kernel8.img"

# Build and run the in-kernel test suites headless; exits non-zero on failure
@test type="RelWithDebInfo": (build-tests type)
    cmake --build {{test_build_dir}} --target sdcard
    ./scripts/run_tests.sh \
        {{test_build_dir}}/kernel/kernel8.img \
        pi4-boot/bcm2711-rpi-4-b.dtb \
        {{test_build_dir}}/sdcard.img \
        {{test_timeout}}

# Build and run the test kernel interactively in QEMU (drops to the shell)
@test-shell type="RelWithDebInfo": (build-tests type)
    cmake --build {{test_build_dir}} --target run

# Show the configuration a tree was last built with
# Usage: just config [dir]  (default: the normal build; the test tree is build-test)
@config dir=build_dir:
    if [ -f {{dir}}/include/config.h ]; then \
        echo "Kernel configuration ({{dir}}):"; \
        sed -n -e 's|^/\* #undef \(CONFIG_[A-Z_]*\) \*/|  \1 = off|p' \
               -e 's|^#define \(CONFIG_[A-Z_]*\)  *\(.*[^ ]\) *$|  \1 = \2|p' \
               -e 's|^#define \(CONFIG_[A-Z_]*\) *$|  \1 = on|p' \
               {{dir}}/include/config.h; \
    else \
        echo "No build in {{dir}} yet."; \
    fi
    echo ""
    echo "Options are declared in CMakeLists.txt."
    echo 'Override with: just cmake_args="-DCONFIG_NR_CPUS=2" build'

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


# Clean build artifacts, the published kernel included
@clean:
    rm -rf {{build_dir}} {{test_build_dir}} compile_commands.json pi4-boot/kernel8.img

# Show kernel binary size
@size: build
    cmake --build {{build_dir}} --target size

# List kernel symbols
@symbols: build
    cmake --build {{build_dir}} --target symbols

clang_format_version := "22.1.8"

# Re-format code
@format: _clang-format-version
    find kernel libc uapi user scripts \( -name "*.c" -o -name "*.h" \) -print0 | xargs -0 clang-format -i

# Check formatting
@check-format: _clang-format-version
    find kernel libc uapi user scripts \( -name "*.c" -o -name "*.h" \) -print0 | xargs -0 clang-format --dry-run --Werror

# Another clang-format version formats differently, so a pass or a diff from it means nothing.
@_clang-format-version:
    clang-format --version | grep -qF " {{clang_format_version}}" || { \
        echo "clang-format {{clang_format_version}} is needed (pipx install clang-format=={{clang_format_version}}), found: $(clang-format --version)"; \
        exit 1; }

# --- Network TAP Setup (for future networking support) ---

tap_iface := "tap0"
bridge_iface := "br0"
host_iface := "en0"

# Create a TAP interface for QEMU networking (macOS - requires tuntap kext)
[macos]
@net-setup:
    echo "Setting up TAP interface {{tap_iface}}..."
    sudo ifconfig {{tap_iface}} create || true
    sudo ifconfig {{bridge_iface}} create || true
    sudo ifconfig {{bridge_iface}} addm {{host_iface}} addm {{tap_iface}}
    sudo ifconfig {{bridge_iface}} up
    sudo ifconfig {{tap_iface}} up
    echo "TAP interface ready: {{tap_iface}} bridged via {{bridge_iface}}"

# Create a TAP interface for QEMU networking (Linux)
[linux]
@net-setup:
    echo "Setting up TAP interface {{tap_iface}}..."
    sudo ip tuntap add dev {{tap_iface}} mode tap user $(whoami)
    sudo ip link set {{tap_iface}} up
    sudo ip link add {{bridge_iface}} type bridge 2>/dev/null || true
    sudo ip link set {{tap_iface}} master {{bridge_iface}}
    sudo ip link set {{host_iface}} master {{bridge_iface}} 2>/dev/null || true
    sudo ip link set {{bridge_iface}} up
    sudo ip addr add 10.0.0.1/24 dev {{bridge_iface}} 2>/dev/null || true
    echo "TAP interface ready: {{tap_iface}} bridged via {{bridge_iface}} (10.0.0.1/24)"

# Tear down TAP/bridge networking
[macos]
@net-teardown:
    sudo ifconfig {{bridge_iface}} destroy 2>/dev/null || true
    sudo ifconfig {{tap_iface}} destroy 2>/dev/null || true
    echo "TAP interface torn down."

[linux]
@net-teardown:
    sudo ip link set {{bridge_iface}} down 2>/dev/null || true
    sudo ip link delete {{bridge_iface}} 2>/dev/null || true
    sudo ip link delete {{tap_iface}} 2>/dev/null || true
    echo "TAP interface torn down."

# Run QEMU with TAP networking enabled
@run-net: build net-setup
    cmake --build {{build_dir}} --target sdcard
    qemu-system-aarch64 \
        -M raspi4b -serial stdio -display none \
        -dtb pi4-boot/bcm2711-rpi-4-b.dtb \
        -kernel {{build_dir}}/kernel/kernel8.img \
        -drive file={{build_dir}}/sdcard.img,format=raw,if=sd \
        -netdev tap,id=net0,ifname={{tap_iface}},script=no,downscript=no \
        -device usb-net,netdev=net0
