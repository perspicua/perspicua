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
  - Requires: AArch64 cross-compiler (aarch64-none-elf-)
  - Command:  make

RUNNING IN QEMU:
  - Terminal: make run
  - GUI:      make run-gui

The source code is organized as follows:
  - kernel/     Core kernel subsystems and drivers
  - libc/       Standard C library
  - user/       User-space applications
  - uapi/       Kernel-user interface headers
  - pi4-boot/   Firmware and boot configuration

For licensing information, see the LICENSE file.
