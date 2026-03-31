## High-Impact Improvements

These suggestions are prioritized to unblock the roadmap while mitigating architectural risks.

**1. Title: Unified Hardware Abstraction Layer (Device Model)**
* **Category:** 🧱 Architecture & Kernel Design
* **Description:** Replace the hardcoded initialization sequence in `main.c` with a dynamic device model. Implement a system where devices are probed dynamically from the Devicetree (FDT), and drivers register themselves to a central `bus`.
* **Why It Matters:** As you add PCIe, USB (xHCI), and Ethernet (Phase 2 & 4), hardcoding driver initialization will become unmaintainable. A unified model is required for dynamic driver loading, power management, and unified `sysfs` reporting.
* **Difficulty:** Medium
* **Impact:** High
* **Recommended Timing:** Soon (Prerequisite for Phase 2 PCIe/USB)

**2. Title: Kernel Lock Dependency Validator (Lockdep)**
* **Category:** 🐞 Debugging & Observability
* **Description:** Implement a debug-only tracking system for spinlocks that records the order in which locks are acquired by each CPU thread. If a thread attempts to acquire a lock out of the globally established order, panic and print the cycle.
* **Why It Matters:** You have a 4-core SMP system. As you move to interrupt-driven SD/UART and block caches, locking complexity will explode. SMP deadlocks are notoriously difficult to debug via GDB; a lockdep system catches them instantly at runtime.
* **Difficulty:** High
* **Impact:** High
* **Recommended Timing:** Now (Before adding more SMP complexity)

**3. Title: Symbolized Stack Traces on Panic / Abort** [done]
* **Category:** 🐞 Debugging & Observability
* **Description:** Embed the kernel symbol table (`.symtab` and `.strtab`) directly into the kernel image. Update `handle_abort` and `panic` to unwind the AArch64 frame pointers (`x29`) and print the actual function names instead of raw hex addresses.
* **Why It Matters:** Dramatically reduces the time needed to debug data aborts. You won't need to run `just disasm` or manually correlate addresses with `kernel8.elf` every time the system crashes.
* **Difficulty:** Medium
* **Impact:** High
* **Recommended Timing:** Now

**4. Title: Unified Page Cache Layer**
* **Category:** 🧱 Architecture & Kernel Design
* **Description:** Implement the Phase 1 roadmap goal by creating a generic page cache layer between the VFS and the block device. Read requests to `fat32` should query an in-memory radix tree of cached blocks. Write requests mark pages as "dirty" to be flushed asynchronously.
* **Why It Matters:** This is the absolute prerequisite for Demand Paging (Phase 3). Without it, every page fault will block on physical SD card I/O, freezing the task. It also prevents thrashing the SD card.
* **Difficulty:** High
* **Impact:** High
* **Recommended Timing:** Now (Completes Phase 1)

**5. Title: Kernel Address Sanitizer (KASAN) / Slab Redzones** [DONE]
* **Category:** 🐞 Debugging & Observability
* **Description:** Implement lightweight memory sanitization for the kernel heap/slab allocators. Add "redzones" (magic bytes like `0xDEADBEEF`) around allocated slabs. Check these zones on `kfree()` to detect buffer overflows or use-after-free bugs.
* **Why It Matters:** Memory corruption in the kernel often manifests millions of cycles after the actual bug occurred, making it impossible to trace. Redzones catch overflows at the exact moment the memory is freed.
* **Difficulty:** Medium
* **Impact:** High
* **Recommended Timing:** Soon

**6. Title: User-Space System Call Fuzzer** [DONE]
* **Category:** 🧪 Testing Infrastructure
* **Description:** Create a user-space application (`stress_syscall.c`) that rapidly calls all implemented syscalls with randomized, invalid, or boundary-case arguments (e.g., null pointers, unmapped addresses, negative file descriptors).
* **Why It Matters:** Ensures the kernel's `validate_user_buffer` and boundary checks are bulletproof. Fuzzing will uncover hidden validation bugs before they cause kernel panics during Phase 3 third-party validation (like porting DOOM).
* **Difficulty:** Low
* **Impact:** Medium
* **Recommended Timing:** Soon

---

## Tooling & Workflow Improvements

**Build & Dev Workflow**
1. **Automated CI with QEMU Expect Scripts:** Add a GitHub Action or GitLab CI pipeline that builds the kernel, boots it in QEMU headlessly, and uses `pexpect` to wait for the shell prompt, run `test`, and assert the output says "all tests passed". This prevents regressions.
2. **Reproducible Devcontainer:** Provide a `Dockerfile` or `.devcontainer.json` that locks in specific versions of the AArch64 toolchain, `mtools`, CMake, and `just`. "It works on my machine" is a massive bottleneck in OS dev.
3. **Incremental SD Card Updates:** Instead of recreating the entire 32MB SD card image every time a single user app changes, create a `just update-sd` target that uses `mcopy -o` to dynamically inject only the changed `.elf` files into the existing FAT32 image. This speeds up the dev loop significantly.

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
