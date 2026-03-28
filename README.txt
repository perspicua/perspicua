Perspicua Kernel
================

Perspicua is a 64-bit multi-core operating system kernel for the 
Raspberry Pi 4B (BCM2711), designed for AArch64.

HARDWARE SUPPORT:
  - Architecture: AArch64 (Cortex-A72)
  - Memory: 39-bit VA, 4KB pages, PMM/MMU
  - Multi-core: SMP (4 cores)
  - Graphics: Framebuffer console

BUILDING THE KERNEL:
  - Requires: AArch64 cross-compiler, CMake, Just, mtools, and cpio.
  - Commands:
      just build [type] # Build the project (default: debug)
      just run          # Compile and run in QEMU
      just run-gui      # Run in QEMU with GUI (serial vc)
      just debug        # Compile and start in debug mode (waits for GDB)
      just gdb          # Connect GDB to the QEMU debug instance
      just disasm       # Show disassembled kernel
      just clean        # Remove build artifacts

      Examples:
        just build            # Builds Debug
        just build release    # Builds Release (Optimized)

The source code is organized as follows:
  - kernel/     Core kernel subsystems and drivers
  - libc/       Standard C library
  - user/       User-space applications
  - uapi/       Kernel-user interface headers
  - pi4-boot/   Firmware and boot configuration

For licensing information, see the LICENSE file.
