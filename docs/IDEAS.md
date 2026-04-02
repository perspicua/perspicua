## Kernel Architecture & Performance

**1. Title: Demand Paging (On-Demand ELF Loading)**
* **Category:** 🧱 MM & Performance
* **Description:** Modify the ELF loader and page fault handler to support demand paging. Instead of loading the entire binary into RAM at `exec()`, map the segments as "not present" and load 4KB pages from the SD card only when the CPU triggers a translation fault.
* **Why It Matters:** Dramatically reduces process startup time and RAM usage. It allows running programs larger than physical memory and is a hallmark of a modern OS.
* **Difficulty:** High
* **Impact:** Very High

**2. Title: Fully Interrupt-Driven I/O (SD & UART)**
* **Category:** 🏎️ Drivers & Throughput
* **Description:** Transition the SD card (`sd.c`) and UART drivers away from polling loops and `sleep_ms()`. Use GIC interrupts to wake up blocked tasks only when the hardware is ready for the next data block.
* **Why It Matters:** Currently, the CPU wastes millions of cycles spinning while waiting for hardware. Interrupt-driven I/O unblocks the scheduler to run other tasks during I/O wait times, significantly improving SMP utilization.
* **Difficulty:** Medium
* **Impact:** High

**3. Title: Kernel Preemption & Priority Scheduling**
* **Category:** 🕒 Scheduling
* **Description:** Implement a `preempt_count` and allow the scheduler to switch tasks even while in EL1 (Kernel Mode), provided no spinlocks are held. Upgrade the Round-Robin scheduler to a Priority-based or MLFQ model.
* **Why It Matters:** Prevents long-running kernel tasks from lagging the entire system. Priority scheduling ensures the UI/Shell remains responsive even under heavy background load.
* **Difficulty:** High
* **Impact:** Medium

**4. Title: Shared Memory IPC (`shm`)**
* **Category:** 🛠️ IPC & MM
* **Description:** Add support for shared memory regions via `mmap(MAP_SHARED)`. Allow different processes to map the same physical page frames into their respective virtual address spaces.
* **Why It Matters:** Fastest form of IPC. Required for high-performance graphics servers, database engines, and complex multi-process applications.
* **Difficulty:** Medium
* **Impact:** High

---

## User-Space & libc Maturity

**1. Title: Buffered I/O (`FILE *`) & Stream Redirection**
* **Category:** 🛠️ libc & User-Space
* **Description:** Implement a standard `FILE` abstraction in `libc` with internal buffering (`fopen`, `fread`, `fwrite`, `fflush`).
* **Why It Matters:** Currently, every `printf` results in a direct UART write or raw `sys_write`. Buffering is critical for performance once file I/O moves to the SD card. It also simplifies porting software that expects `stdin`/`stdout`.
* **Difficulty:** Medium
* **Impact:** High

**2. Title: Standardized Error Handling (`errno.h`)**
* **Category:** 🛠️ libc & User-Space
* **Description:** Implement a global `errno` and ensure all system call wrappers in `libc` set it correctly based on kernel return codes.
* **Why It Matters:** This is the "missing link" for porting 3rd party C code. Almost all standard libraries and applications rely on `errno` to diagnose failures.
* **Difficulty:** Low
* **Impact:** High

**3. Title: Core POSIX Utilities (The "Base System")**
* **Category:** 🛠️ libc & User-Space
* **Description:** Implement missing core utilities: `mkdir`, `rm`, `cp`, `mv`, `free`, `ps`, `df`, `kill`, `grep`, and `wc`.
* **Why It Matters:** Transitioning from an "experiment" to a "system" requires basic file management and observation tools.
* **Difficulty:** Low to Medium
* **Impact:** High

**4. Title: Shell (`sh`) Quality of Life: Tab Completion & Globbing**
* **Category:** 🛠️ libc & User-Space
* **Description:** Add filename tab completion (reading the CWD), globbing (`*`), and basic job control (`jobs`, `fg`, `bg`) to the shell.
* **Why It Matters:** Significantly improves the interactive developer experience. Navigating the VFS becomes exponentially faster with completion.
* **Difficulty:** Medium
* **Impact:** Medium

**5. Title: User-Space System Integrity (assert.h & libc tests)**
* **Category:** 🛠️ libc & User-Space
* **Description:** Implement `assert.h` (using `abort()`) and a standalone `libc` test suite (running in user-space). Refactor headers (moving `VFS_O_RDONLY` to `fcntl.h`, etc.) to align with POSIX expectations.
* **Why It Matters:** As more code moves to user-space, the kernel's `ASSERT` becomes inappropriate. Standardized testing for `libc` ensures core functions like `malloc` and `string.c` don't regress.
* **Difficulty:** Medium
* **Impact:** High

---

## Tooling & Workflow Improvements

**Build & Dev Workflow**
1. **Incremental SD Card Updates:** Instead of recreating the entire 32MB SD card image every time a single user app changes, create a `just update-sd` target that uses `mcopy -o` to dynamically inject only the changed `.elf` files into the existing FAT32 image. This speeds up the dev loop significantly.

**Debugging & Testing**
4. **GDB Python Helper Scripts:** Write a `.gdbinit` or Python GDB extension specifically for Perspicua. Add custom commands like `pers_dump_tasks` (to walk the process list) or `pers_dump_pt <pid>` (to translate virtual addresses). Inspecting nested kernel structs manually in GDB is tedious.
5. **Configurable Kernel Features (`Kconfig` lite):** Introduce a configuration system (via CMake variables) to toggle features like `CONFIG_SMP` or `CONFIG_DEBUG_MEMORY`. Being able to compile a stripped-down uniprocessor version for debugging specific subsystems is invaluable.
6. **Network TAP Setup in Justfile:** Update the `run` targets to automatically configure a TAP/TUN network interface bridging QEMU and the host. You cannot develop or test the TCP/IP stack (Phase 4) without this.

---

## Long-Term Strategic Ideas

1. **POSIX ABI Compatibility:** Instead of inventing custom syscall numbers, align your `uapi/syscalls.h` exactly with the standard Linux AArch64 syscall ABI. This allows you to port standard libraries (like `musl libc`) without massive modifications, instantly unlocking thousands of third-party programs.
2. **User-Space Graphics Server (Wayland-lite):** Once Phase 3 (Shared Memory) is complete, design a compositing window manager entirely in user-space. The kernel should only handle the dumb framebuffer mapping and input events (`evdev`), keeping complex graphics drivers out of kernel space.
3. **Hardware-Assisted Virtualization (KVM lite):** AArch64 has excellent virtualization extensions (EL2). A long-term stretch goal could be utilizing EL2 to run a secondary guest OS alongside Perspicua to prove the absolute robustness of your scheduler and memory manager.

---

## Hidden Risks & Warnings

1. **The Polled I/O Deadlock Trap:** Moving from polled SD/UART to interrupt-driven logic in an SMP environment is arguably the most dangerous transition for an OS. If Core 0 handles a block read interrupt while Core 1 tries to write, and the VFS locks aren't perfectly granular, the system will instantly freeze. 
2. **Exception Level Privilege Escalation:** Ensure that the `uaccess.S` routines strictly use unprivileged load/store instructions (`ldtr`/`sttr` in AArch64) so the CPU hardware enforces permissions. If the kernel uses standard `ldr` for user pointers, a compromised user process can trick the kernel into overwriting its own memory.
3. **FAT32 Patent/Complexity Quirks:** Implementing FAT32 write support (Phase 2) is notoriously prone to corrupting the filesystem if power is lost during a cluster chain update. Ensure you have a robust sync/flush mechanism, or strongly consider implementing a simpler journaled filesystem (like ext2) for writes instead.
