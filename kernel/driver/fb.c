/*
 * fb.c - Implementation of the framebuffer driver.
 *
 * This module handles mailbox communication with the VideoCore GPU to
 * initialize and manage the display buffer.
 */

#include "driver/fb.h"

#include "stdio.h"
#include "types.h"

#include "uapi/errors.h"

#include "driver/mailbox.h"
#include "fs/devfs.h"
#include "mm/addr.h"
#include "mm/pmm.h"
#include "mm/mmu.h"
#include "sched/process.h"

/* Mailbox message buffer for GPU requests (must be 16-byte aligned) */
static __attribute__((aligned(16))) unsigned int mbox[36];

/* VFS operations for the /dev/fb0 device */
static struct vfs_vnode_ops fb_vfs_ops;

struct fb_info_struct fb_info;

/*
 * fb_mmap - Maps the physical framebuffer into a user process address space.
 */
static int fb_mmap(struct vfs_file *file, uintptr_t vaddr, size_t length, int prot, int flags)
{
    (void)file;
    (void)prot;
    (void)flags;

    uintptr_t phys_fb = V2P((uintptr_t)fb_info.ptr);
    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;

    struct process *p = process_current();
    if (!p || !p->user_pgd) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }
    unsigned long *pgd = p->user_pgd;

    for (size_t i = 0; i < pages; i++) {
        mmu_user_map_page(pgd, vaddr + i * PAGE_SIZE, phys_fb + i * PAGE_SIZE,
                          MMU_FLAGS_FRAMEBUFFER | MMU_AP_USER);
    }

    return 0;
}

/*
 * fb_init - Requests a 32-bit RGBA display buffer from the GPU.
 */
void fb_init(void)
{
    mbox[0] = 26 * 4;
    mbox[1] = 0;
    mbox[2] = 0x48003; /* Physical Width/Height */
    mbox[3] = 8;
    mbox[4] = 8;
    mbox[5] = 1024;
    mbox[6] = 768;
    mbox[7] = 0x48004; /* Virtual Width/Height */
    mbox[8] = 8;
    mbox[9] = 8;
    mbox[10] = 1024;
    mbox[11] = 768;
    mbox[12] = 0x48005; /* Depth (32-bit) */
    mbox[13] = 4;
    mbox[14] = 4;
    mbox[15] = 32;
    mbox[16] = 0x40001; /* Allocate Buffer */
    mbox[17] = 8;
    mbox[18] = 8;
    mbox[19] = 4096;    /* Request: alignment / Response: address */
    mbox[20] = 0;       /* Response: size */
    mbox[21] = 0x40008; /* Get Pitch */
    mbox[22] = 4;
    mbox[23] = 4;
    mbox[24] = 0;
    mbox[25] = 0;

    mbox_call(mbox);

    /* Response code 0x80000000 indicates success */
    if (mbox[20] != 0 && mbox[1] == 0x80000000) {
        uintptr_t phys_addr = mbox[19] & 0x3FFFFFFF;
        if (phys_addr == 0) {
            pr_err("fb: GPU returned invalid address\n");
            return;
        }

        fb_info.width = mbox[5];
        fb_info.height = mbox[6];
        fb_info.size = mbox[20];
        fb_info.pitch = mbox[24];
        fb_info.ptr = (unsigned char *)P2V(phys_addr);

        /* Ensure memory manager knows this region is hardware-owned */
        pmm_reserve_range((unsigned long)phys_addr, fb_info.size, "framebuffer");

        pr_info("fb: %dx%d @ %p (%lu MB, pitch %d)\n", fb_info.width, fb_info.height, fb_info.ptr,
                (unsigned long)(fb_info.size / (1024 * 1024)), fb_info.pitch);
    } else {
        pr_err("fb: failed to initialize framebuffer\n");
    }
}

/*
 * fb_register_device - Links the framebuffer to the device filesystem.
 */
void fb_register_device(void)
{
    fb_vfs_ops.mmap = fb_mmap;
    devfs_register_device("fb0", &fb_vfs_ops, NULL);
}

/*
 * remap_framebuffer_pages - Updates MMU mappings with device memory attributes.
 */
void remap_framebuffer_pages(void)
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
