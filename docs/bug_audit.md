# Perspicua bug audit

Audit of the kernel on `dev` at `fbb3296`, branch `test/bug_audit`, November 2026.

## Scope and method

- **Read in full:** `kernel/core` (locks, wait queues, sleeping locks, signals,
  syscalls, tty, ELF loader), `kernel/sched`, `kernel/mm`, `kernel/arch`,
  `kernel/fs` (VFS, FAT32, pipes, page cache, procfs), and the block, SD, UART,
  mailbox and framebuffer drivers.
- **Not read:** kdb, lockdep internals, the devicetree parser, the userspace
  programs and the user half of libc.
- **Nothing here was run.** This machine has no cross-compiler and no QEMU, so
  every finding comes from reading the code. Each one says how sure I am and
  how to trigger it, so it can be confirmed with a test before it is fixed.
- Items `docs/order.txt` already tracks are listed at the end and not repeated.

## Summary

| ID | Severity | Area | Finding |
|----|----------|------|---------|
| H1 [DONE] | High | VFS / FAT32 | `rename` into another filesystem writes a FAT32 directory entry into an unrelated cluster |
| H2 [DONE] | High | VFS / FAT32 | Two `open(O_CREAT)` calls racing on one new name create duplicate directory entries |
| H3 [DONE] | High | Block cache | A raw read of `/dev/sd0` can leave stale blocks in the cache the filesystem then uses |
| M1 [DONE] | Medium | procfs | `/proc/<pid>/maps` leaks 4 KB of kernel heap per read and can return uninitialised heap |
| M2 [DONE] | Medium | FAT32 | Writing past end of file exposes old disk contents instead of zeros |
| M3 | Medium | exec / caches | Instruction cache is flushed through the wrong address alias (real hardware only) |
| M4 | Medium | Mailbox | Firmware calls wait forever with interrupts off; reachable from `/proc/clock` |
| M5 | Medium | fork | `mmu_lock` is held with interrupts off for the whole address-space copy |
| L1 | Low | exec | `exec` fails with `ENFILE` when the caller's descriptor table is full |
| L2 | Low | VFS | Releasing a deep directory chain recurses once per level |
| L3 | Low | Scheduler / SD | `sched_sleep_ms` returns early on any signal; the SD driver counts those as elapsed time |
| L4 | Low | Syscalls | Side effects are kept when the copy back to user memory fails |
| L5 | Low | ELF | A segment may be mapped at address 0 |
| L6 | Low | Syscalls | Unknown syscall numbers print a kernel warning each time |
| L7 | Low | Allocators | The kernel heap never shrinks; slab pages are released and re-allocated on every use |
| L8 | Low | VFS | A file offset shared after `fork` is updated without a lock |

No lock-order inversion was found. The deadlock analysis and the hazards that
are not inversions are in their own section below.

---

## High

### H1. [DONE] `rename` across filesystems corrupts the FAT32 volume

**Where:** `vfs_rename` in `kernel/fs/vfs.c`; `fat32_rename` in `kernel/fs/fat32.c`.

`vfs_rename` resolves the two parent directories and calls
`old_parent->ops->rename(old_parent, old_name, new_parent, new_name)`. It never
checks that both parents belong to the same filesystem, and `fat32_rename` does
not check either. FAT32 reads `new_parent->internal_info` as a directory's
start cluster.

A `/proc/<pid>` directory vnode stores the pid in `internal_info`
(`procfs_root_lookup`). So for `rename("/file", "/proc/5/file")`:

1. FAT32 treats cluster 5, which belongs to whatever file owns it, as a directory.
2. `fat32_write_dir_entries(5, ...)` scans it for a 32-byte slot whose first
   byte is `0x00` or `0xE5` and writes a directory entry there, inside that
   file's data. If it finds none it extends that file's cluster chain.
3. The source's real directory entry is then deleted.

The source file disappears from the namespace and 32 bytes of another file may
be overwritten. With pid 2 the entry usually lands in the root directory,
because cluster 2 is normally the root.

**Trigger:** `mv somefile /proc/5` from the shell (`mv` appends the basename and
calls `rename`). Needs no privilege and no race.

**Confidence:** high; every step is visible in the code.

**Fix:** in `vfs_rename`, return an error (`EXDEV`, which `uapi/errno.h` would
need) when `old_parent_node->ops != new_parent_node->ops`. Add the same check
at the top of `fat32_rename` so the driver does not depend on its caller.

### H2. [DONE] Racing `open(O_CREAT)` calls create duplicate directory entries

**Where:** `vfs_open_pid` in `kernel/fs/vfs.c`; `fat32_create` in `kernel/fs/fat32.c`.

`vfs_open_pid` resolves the path, and if that fails with `ENOENT` and
`O_CREAT` is set, calls the parent's `create`. The lookup and the create each
take `fat32_lock` separately. `fat32_create` allocates a cluster and writes a
new entry without checking whether the name now exists (`fat32_mkdir` does
check, under the lock).

Two processes that open the same new name at the same time both see `ENOENT`
and both create it. The directory ends up with two entries of the same name,
each with its own cluster. Each process writes to a different file; later
lookups find only the first.

The window is wide, because the lookup and the create both do blocking SD I/O.

**Trigger:** `sh -c 'echo a > newfile & echo b > newfile'` in a loop, then list
the directory or run the host `fsck.vfat` that `just test` already runs.

**Confidence:** high for the mechanism; how often it fires needs a test.

**Fix:** have `fat32_create` look the name up under `fat32_lock` and return
`-EEXIST`. In `vfs_open_pid`, treat `-EEXIST` from `create` as "someone else
made it" and continue to the second resolve. There is no `O_EXCL` flag today;
this is where it would go.

### H3. [DONE] A raw device read can poison the block cache

**Where:** `cached_read_blocks`, `cache_fill` and `cached_write_blocks` in
`kernel/driver/block.c`.

A read that misses the cache does three things: it drops `cache_lock`, reads
the blocks from the device, then inserts them with `cache_fill`. A write goes
to the device and then updates a cache entry only if one already exists.

Interleaving, for one block B:

1. Reader R reads B from the device and gets the old contents.
2. Writer W writes new contents to the device. No cache entry exists, so
   nothing is cached.
3. R's `cache_fill` inserts the old contents.

The cache now disagrees with the disk until B is evicted. FAT32 itself is
safe from this, because every FAT32 operation holds `fat32_lock`. But
`/dev/sd0` is readable by any program through `block_device_vfs_read`, which
does not take that lock. If B is a FAT sector, the next `set_fat_entry` reads
the stale copy, modifies one entry and writes the whole stale sector back,
which undoes other allocations and cross-links files.

**Trigger:** read `/dev/sd0` in a loop (for example `cp /dev/sd0 /dev/null`
if the tools read in multiples of 512 bytes) while another process creates and
deletes files, then check the volume.

**Confidence:** high for the race; the misses of a multi-block read are filled
only after the whole run has been read, so the window grows with the read size.

**Fix:** make `cached_write_blocks` insert the block when no entry exists,
instead of only updating one. `cache_fill` already declines to overwrite an
existing entry, so the stale insert then cannot win in either order.

---

## Medium

### M1. [DONE] `/proc/<pid>/maps` leaks memory and can return uninitialised heap

**Where:** `procfs_pid_maps_read` in `kernel/fs/procfs.c`.

Two defects in one function:

- **Leak.** The 4 KB buffer from `heap_malloc(PROCFS_CONTENT_MAX)` is freed on
  the "no such process" path but not on the normal path, which returns
  `procfs_copy_out(...)` directly. Every `read` of the file leaks 4 KB plus a
  header. A loop of `cat /proc/self/maps` exhausts kernel memory.
- **Uninitialised read.** The length is taken with `strlen(buf)`, but the
  buffer is not initialised and nothing is written to it when the process has
  no regions. That is the case for pid 0 and for every zombie (`process_exit`
  sets `va.count = 0`). `strlen` then runs over old heap contents, possibly
  past the end of the buffer, and that many bytes are copied to the caller.

**Trigger:** `cat /proc/0/maps`; watch `/proc/meminfo` across repeated reads.

**Confidence:** high.

**Fix:** use `pos` as the length, and free the buffer after `procfs_copy_out`.

**Done:** the length is now `pos`. The leak was already gone at the time of the fix: the buffer is freed after `procfs_copy_out`.

### M2. [DONE] Writing past end of file exposes old disk contents

**Where:** `fat32_write_page` and `extend_cluster_chain` in `kernel/fs/fat32.c`.

`lseek` accepts any non-negative offset. A write beyond the current size walks
the cluster chain and extends it with `extend_cluster_chain`, which links new
clusters without zeroing them (only directory growth and `fat32_create` call
`zero_cluster`). The file size then covers the gap, and `fat32_read_page`
reads the gap from disk. The reader gets whatever deleted file last used those
clusters, where POSIX requires zeros.

The same happens after a shrinking `truncate`: bytes between the new size and
the end of the last kept cluster stay on disk and reappear if the file is
later extended past them.

**Trigger:** create a file, `lseek` 1 MB forward, write one byte, read the
first megabyte back.

**Confidence:** high.

**Fix:** zero each cluster that `fat32_write_page` adds while skipping to the
write position, and zero the tail of the old last cluster. Growing through
`truncate` is already refused with `ENOTSUP`, so it needs the same care when
it is implemented.

**Done:** a file's bytes past its size are now kept zero on disk. Every cluster
is zeroed as it joins a file (`extend_cluster_chain` and a file's first
cluster in `fat32_write_page`), and a shrinking `truncate` zeroes the rest of
the last cluster it keeps. Appending costs one extra write per new cluster.

### M3. The instruction cache is flushed through the kernel alias

**Where:** `elf_load` in `kernel/core/elf.c`; `process_flush_icache_range` in
`kernel/sched/process.c`.

After copying a code page, `elf_load` flushes it with `IC IVAU` on the page's
kernel linear-map address. The Cortex-A72's L1 instruction cache is 48 KB,
3-way, so one way is 16 KB: it is indexed by address bits above the 4 KB page
offset. On such a cache, invalidating by one virtual address is only
guaranteed to remove lines indexed by that address. The code will run at a
user address, which shares a cache index with the kernel address only one
time in four. Linux handles the same CPU by invalidating the whole
instruction cache in this situation.

The effect is that a physical page reused for code can still have lines from
its previous contents in the instruction cache, and the new program executes
them. It needs the old lines to still be resident, so it would show up as a
rare illegal-instruction or segmentation fault in a freshly started program.

**Trigger:** real hardware only; QEMU does not model the cache. Start many
different programs in quick succession on one core and look for sporadic
faults at start-up.

**Confidence:** medium. The cache geometry and the flush address are certain;
I have not seen the failure, and how often old lines survive is unknown.

**Fix:** issue `IC IALLUIS` once at the end of `elf_load`, or flush through
the user address with the new page table installed.

### M4. Mailbox calls can hang a core with interrupts off

**Where:** `mbox_exchange` in `kernel/driver/mailbox.c`.

`mbox_call` and `mbox_property` take `mbox_lock` with interrupts off and then
loop until the firmware answers on channel 8. There is no timeout. If the
firmware does not answer, that core never takes another interrupt, and every
other caller spins on `mbox_lock`.

The path is reachable by any program: reading `/proc/clock` makes several
mailbox calls. Even when the firmware is healthy, each call is an
interrupt-off stretch whose length the firmware decides.

**Confidence:** high that there is no timeout; whether the firmware ever
stalls is a hardware question.

**Fix:** bound both wait loops with the generic timer and return an error.

### M5. `fork` holds `mmu_lock` for the whole address-space copy

**Where:** `mmu_copy_user_pgd` in `kernel/mm/mmu.c`.

The function takes `mmu_lock` with interrupts off, then walks every mapped
page of the parent: it allocates the child's tables, takes `pmm_lock` once per
page for `pmm_hold_page`, and makes a second pass to mark the parent
copy-on-write. With a 256 MB mapping that is 65,536 pages under one lock hold.

During that time the forking core takes no interrupts, and every other core
that faults on a copy-on-write page or maps anything waits on `mmu_lock`. It is
a latency problem, not a deadlock.

**Confidence:** high.

**Fix:** drop and retake the lock between page tables. The parent is
single-threaded and is the caller, so its tables cannot change underneath the
copy; only the child's tables need the lock while they are built.

---

## Low

### L1. `exec` needs a free descriptor

`elf_load` opens the image with `vfs_open`, which takes a slot in the calling
process's descriptor table. A process with all `CONFIG_MAX_FDS` slots in use
cannot `exec`. Fix: resolve the path to a vnode and read through it without a
descriptor.

### L2. Releasing a directory chain recurses per level

`vfs_vnode_put` calls itself for the parent after freeing a node. Each `chdir`
into a subdirectory adds a level to the current directory's chain, with no
limit. At `-O2` the call is a tail call and compiles to a loop. In a Debug
build (`-O0`) it is real recursion, and a chain a little over a thousand
levels deep overflows the 60 KB kernel stack when the process exits. Fix:
write the release as a loop.

### L3. `sched_sleep_ms` can return early

`signal_send_target_locked` unblocks the target's task whenever it is blocked,
whatever it is blocked on. `sched_sleep_ms` does not re-sleep after such a
wake-up. Its only caller is `sd_wait_status`, which treats each return as one
millisecond elapsed, so a burst of signals to a process doing SD I/O shortens
the driver's timeouts. Fix: loop in `sched_sleep_ms` until the deadline.

### L4. Side effects survive a failed copy to user memory

Each of these validates the user buffer first, so it needs the mapping to
change in between; they matter once threads share an address space.

- `pipe_handler`: both descriptors stay open if writing them to the caller fails.
- `waitpid_handler`: the child is reaped and its status lost; the error
  returned is `ENOMEM` where it should be `EFAULT` (`stat_handler` has the same
  wrong code).
- `read_handler`: bytes already taken from a pipe or the tty are dropped.

### L5. An ELF segment may map address 0

`elf_load` rejects an entry point of 0 but accepts a loadable segment whose
address is 0. A program built that way has a mapped null page, so its null
dereferences do not fault.

### L6. Unknown syscalls log every time

`syscall_handle` calls `pr_warn` for each unknown number. A program can fill
the console, and each message is sent at 115200 baud. Fix: rate-limit or drop
the message.

### L7. Allocator behaviour under churn

- The first-fit heap never returns memory to the page allocator. A single
  4 MB `read` makes the heap 4 MB larger for good.
- A slab page is released as soon as its last object is freed. A size class
  with one object that is allocated and freed in a loop allocates and zeroes a
  page each time. Keeping one empty page per class would avoid it.

### L8. Shared file offsets are unsynchronised

After `fork`, parent and child share one `struct vfs_file`. `vfs_read` and
`vfs_write` pass `&f->offset` to the filesystem with no lock on the file, so
two processes reading through the shared descriptor can read the same bytes
twice.

---

## Deadlock analysis

### Lock order

I traced every nested acquisition. The order below holds everywhere; a lock
is only ever taken while holding locks above it in its own chain.

| Outer lock | Taken inside it |
|------------|-----------------|
| `fat32_lock` (sleeping, recursive) | `sd_op_mutex`, `pagecache_lock`, block `cache_lock`, slab and page locks |
| `process_table_lock` | a process's `fd_lock`, `asid_lock`, `sched_sleep_lock`, run-queue locks |
| `pipe->lock`, `tty->lock`, kmutex and rwsem guards | the wait queue's lock |
| `tty->lock` | `uart_tx_lock` |
| a wait queue's lock | `sched_sleep_lock`, run-queue locks |
| `mmu_lock`, slab class locks, `pagecache_lock`, block `cache_lock` | `pmm_lock` |

`pmm_lock`, `uart_tx_lock`, `heap_lock`, `sd_irq_lock`, `mbox_lock` and the
run-queue locks are leaves: nothing else is taken inside them.

Specific things I checked and found sound:

- **Lost wake-ups.** `wq_prepare` queues the entry and marks the task blocked
  before the condition is tested, and wakers change state with a
  compare-and-swap, so a wake that lands before the switch is kept. The same
  holds for `waitpid`, `sigsuspend` and the stop path, which publish the
  blocked state under `process_table_lock` or behind a fence.
- **Interrupt handlers.** `process_table_lock`, `tty->lock` and every lock an
  interrupt handler reaches are always taken with interrupts off, so a handler
  cannot interrupt its own lock's holder.
- **Sleeping under a spinlock.** The page cache drops its lock before calling
  `write_page`, and every `vfs_vnode_put` that may sleep runs after the
  spinlock is released.
- **Console.** `printk` takes only `uart_tx_lock` and pulls the tty ring
  without `tty->lock`, so it does not invert against `tty_write`.

### Hazards that are not inversions

- **M4** (mailbox) and **M5** (`fork`) above are the two unbounded or very
  long interrupt-off sections.
- **`printk` under a spinlock.** `uart_write_locked` busy-waits for the UART
  FIFO. When the caller already has interrupts off, a 100-character message
  holds them off for about 9 ms at 115200 baud.
- **Pipe copies.** `pipe_read` moves up to 4096 bytes one at a time under
  `pipe->lock` with interrupts off.
- **Wake-up onto the waker's own core.** A task woken while it is still being
  switched out is queued behind an `on_core` guard, and its core can go idle
  until the next tick with that task runnable. This costs up to 10 ms of
  latency, not correctness, and goes away with the reschedule IPI that
  `order.txt` plans.

---

## Already tracked in `docs/order.txt`

Seen again during this audit and not re-reported as new:

- `syscall_validate_user_buffer` checks, then drops the lock before the copy
  (Phase 2 item 7).
- Byte-at-a-time user copies and the per-syscall bounce buffer (Phase 2 item 7).
- A pid is its table index and can be reused immediately (Phase 2 item 11).
- No bounds check on block numbers at the block layer (Phase 2 item 5).
- `O_APPEND` reads the size and writes in two separate lock holds (Phase 4 item 8).
- The panic IPI cannot stop a core that has interrupts masked (Phase 2 item 14).

## Suggested order of fixes

1. H1, M1: one-line checks, no design questions.
2. H2, H3: small, each wants a regression test that races two processes.
3. M2, M4: contained changes in one file each.
4. M3: confirm on hardware first, then apply the one-instruction fix.
5. M5 and the low items as time allows.
