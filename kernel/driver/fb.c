/*
 * fb.c - Driver for the Raspberry Pi VideoCore framebuffer.
 */

#include "driver/fb.h"

#include "stdio.h"
#include <stddef.h>
#include <stdint.h>

#include "uapi/errno.h"
#include "uapi/mman.h"

#include "core/lock.h"
#include "driver/fb_console.h"
#include "driver/mailbox.h"
#include "fs/devfs.h"
#include "mm/addr.h"
#include "mm/pmm.h"
#include "mm/mmu.h"
#include "sched/process.h"

// Mailbox message buffer for GPU requests (must be 16-byte aligned)
static __attribute__((aligned(16))) unsigned int mbox[36];

// VFS operations for the /dev/fb0 device
static struct vfs_vnode_ops fb_vfs_ops;

struct fb_info_struct fb_info;

// Open files that have mapped the screen; while any stays open the console keeps off it.
#define FB_MAX_OWNERS 8
static struct vfs_file *fb_owners[FB_MAX_OWNERS];
static spinlock_t fb_owner_lock = SPINLOCK_INIT;

static void fb_track_owner(struct vfs_file *file, int owns)
{
    unsigned long flags = spin_lock_irqsave(&fb_owner_lock);
    int count = 0;
    struct vfs_file **slot = NULL;
    for (int i = 0; i < FB_MAX_OWNERS; i++) {
        if (fb_owners[i] == file) {
            slot = &fb_owners[i];
        } else if (!fb_owners[i] && !slot && owns) {
            slot = &fb_owners[i];
        }
    }
    if (slot) {
        *slot = owns ? file : NULL;
    }
    for (int i = 0; i < FB_MAX_OWNERS; i++) {
        count += fb_owners[i] != NULL;
    }
    fb_console_hide(count > 0);
    spin_unlock_irqrestore(&fb_owner_lock, flags);
}

static int fb_close(struct vfs_file *file)
{
    fb_track_owner(file, 0);
    return 0;
}

static int fb_mmap(struct vfs_file *file, uintptr_t vaddr, size_t length, int prot, int flags)
{
    (void)flags;

    if (!fb_info.ptr || fb_info.size == 0) {
        return -ENODEV;
    }

    // Past the framebuffer is ordinary physical memory, so an unclamped length
    // hands the caller the kernel's own pages.
    if (length > fb_info.size) {
        return -EINVAL;
    }

    uintptr_t phys_fb = V2P((uintptr_t)fb_info.ptr);
    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;

    struct process *p = process_current();
    if (!p || !p->user_pgd) {
        return -ESRCH;
    }
    unsigned long *pgd = p->user_pgd;

    // NG, or the entry outlives the address space: asid_free's tlbi aside1is
    // leaves global entries alone.
    unsigned long attrs = MMU_FLAGS_FRAMEBUFFER | MMU_AP_USER | MMU_PTE_NG;
    if (!(prot & PROT_WRITE)) {
        attrs |= MMU_AP_RO;
    }

    for (size_t i = 0; i < pages; i++) {
        int err = mmu_user_map_page(pgd, vaddr + i * PAGE_SIZE, phys_fb + i * PAGE_SIZE, attrs);
        if (err != 0) {
            return err;
        }
    }

    fb_track_owner(file, 1);
    return 0;
}

void fb_init(void)
{
    mbox[0] = 30 * 4;
    mbox[1] = 0;
    mbox[2] = 0x48003; // Physical Width/Height
    mbox[3] = 8;
    mbox[4] = 8;
    mbox[5] = 1024;
    mbox[6] = 768;
    mbox[7] = 0x48004; // Virtual Width/Height
    mbox[8] = 8;
    mbox[9] = 8;
    mbox[10] = 1024;
    mbox[11] = 768;
    mbox[12] = 0x48005; // Depth (32-bit)
    mbox[13] = 4;
    mbox[14] = 4;
    mbox[15] = 32;
    mbox[16] = 0x48006; // Pixel order: BGR in memory, so a 0x00RRGGBB word shows as written
    mbox[17] = 4;
    mbox[18] = 4;
    mbox[19] = 0;
    mbox[20] = 0x40001; // Allocate Buffer
    mbox[21] = 8;
    mbox[22] = 8;
    mbox[23] = 4096;    // Request: alignment / Response: address
    mbox[24] = 0;       // Response: size
    mbox[25] = 0x40008; // Get Pitch
    mbox[26] = 4;
    mbox[27] = 4;
    mbox[28] = 0;
    mbox[29] = 0;

    mbox_call(mbox);

    // Response code 0x80000000 indicates success
    if (mbox[24] != 0 && mbox[1] == 0x80000000) {
        uintptr_t phys_addr = mbox[23] & 0x3FFFFFFF;
        if (phys_addr == 0) {
            pr_err("fb: GPU returned invalid address\n");
            return;
        }

        fb_info.width = mbox[5];
        fb_info.height = mbox[6];
        fb_info.size = mbox[24];
        fb_info.pitch = mbox[28];
        fb_info.ptr = (unsigned char *)P2V(phys_addr);

        // Ensure memory manager knows this region is hardware-owned
        pmm_reserve_range((unsigned long)phys_addr, fb_info.size, "framebuffer");

        pr_info("fb: %dx%d @ %p (%lu MB, pitch %d)\n", fb_info.width, fb_info.height, fb_info.ptr,
                (unsigned long)(fb_info.size / (1024 * 1024)), fb_info.pitch);
    } else {
        pr_err("fb: failed to initialize framebuffer\n");
    }
}

void fb_register_device(void)
{
    fb_vfs_ops.mmap = fb_mmap;
    fb_vfs_ops.close = fb_close;
    devfs_register_device("fb0", &fb_vfs_ops, NULL);
}

void fb_remap_pages(void)
{
    if (!fb_info.ptr || fb_info.size == 0) {
        return;
    }

    unsigned long fb_start = (unsigned long)fb_info.ptr;
    unsigned long fb_end = fb_start + fb_info.size;

    fb_start &= ~(PAGE_SIZE - 1);
    fb_end = (fb_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (unsigned long va = fb_start; va < fb_end; va += PAGE_SIZE) {
        mmu_map_page(va, V2P(va), MMU_FLAGS_FRAMEBUFFER);
    }
}
