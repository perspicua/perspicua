/*
 * fb.c - Implementation of the framebuffer driver.
 *
 * This file handles the low-level mailbox communication with the
 * VideoCore GPU to set up and manage the display buffer.
 */

#include "driver/fb.h"

#include "fs/devfs.h"
#include "sched/process.h"
#include "types.h"
#include "stdio.h"
#include "mm/addr.h"

#include "mm/pmm.h"

#include "driver/mailbox.h"

/* The mailbox communication buffer for GPU requests. */
static __attribute__((aligned(16))) unsigned int mbox[36];

/* The global framebuffer device information. */
struct fb_info_struct fb_info;
struct vfs_vnode_ops fb_vfs_ops = {0};

int fb_mmap(struct vfs_file* file, uintptr_t vaddr, size_t length, int prot, int flags)
{
    (void)file;
    (void)prot;
    (void)flags;

    uintptr_t phys_fb = V2P((uintptr_t)fb_info.ptr);

    size_t pages = length / PAGE_SIZE;
    if (length % PAGE_SIZE != 0)
        pages++;

    int pid = process_find_current();
    unsigned long* pgd = process_table[pid].user_pgd;

    for (size_t i = 0; i < pages; i++)
    {
        mmu_user_map_page(pgd, vaddr + i * PAGE_SIZE, phys_fb + i * PAGE_SIZE, MMU_FLAGS_FRAMEBUFFER | MMU_AP_USER);
    }

    return 0;
}

/*
 * fb_init - Initializes the Raspberry Pi 4 framebuffer.
 */
void fb_init(void)
{
    mbox[0] = 26 * 4;   // Total size of this message (26 elements * 4 bytes)
    mbox[1] = 0;        // Request
    mbox[2] = 0x48003;  // Set Physical Width/Height
    mbox[3] = 8;
    mbox[4] = 8;
    mbox[5] = 1024;
    mbox[6] = 768;
    mbox[7] = 0x48004;  // Set Virtual Width/Height
    mbox[8] = 8;
    mbox[9] = 8;
    mbox[10] = 1024;
    mbox[11] = 768;
    mbox[12] = 0x48005;  // Set Depth (32-bit)
    mbox[13] = 4;
    mbox[14] = 4;
    mbox[15] = 32;
    mbox[16] = 0x40001;  // ALLOCATE BUFFER (Action tag)
    mbox[17] = 8;
    mbox[18] = 8;
    mbox[19] = 4096;     // Request: Alignment. Response: Framebuffer Address
    mbox[20] = 0;        // Response: Framebuffer Size
    mbox[21] = 0x40008;  // GET PITCH (Bytes per line)
    mbox[22] = 4;
    mbox[23] = 4;
    mbox[24] = 0;  // Response will go here
    mbox[25] = 0;  // End Tag

    mbox_call(mbox);

    if (mbox[20] != 0 && mbox[1] == 0x80000000)
    {
        uintptr_t phys_addr = mbox[19] & 0x3FFFFFFF;
        if (phys_addr == 0)
        {
            printf("[   FB ] Error: VideoCore returned NULL or invalid address!\n");
            return;
        }

        fb_info.width = mbox[5];
        fb_info.height = mbox[6];

        fb_info.size = mbox[20];
        fb_info.pitch = mbox[24];

        fb_info.ptr = (unsigned char*)P2V(phys_addr);

        // Reserve framebuffer pages in the PMM before they are allocated elsewhere.
        pmm_reserve_range((unsigned long)phys_addr, fb_info.size, "framebuffer");

        printf("[   FB ] Framebuffer initialized: %dx%d @ %p (phys 0x%lx, size %d, pitch %d)\n",
               fb_info.width,
               fb_info.height,
               fb_info.ptr,
               (unsigned long)phys_addr,
               fb_info.size,
               fb_info.pitch);
    }
    else
    {
        printf("[   FB ] Error: Could not initialize framebuffer!\n");
    }
}

void fb_register_device(void)
{
    fb_vfs_ops.mmap = fb_mmap;
    devfs_register_device("fb0", &fb_vfs_ops, NULL);
}
/*
 * remap_framebuffer_pages - Updates the MMU mapping for the framebuffer.
 */
void remap_framebuffer_pages(void)
{
    if (!fb_info.ptr || fb_info.size == 0)
    {
        return;
    }

    unsigned long fb_start = (unsigned long)fb_info.ptr;
    unsigned long fb_end = fb_start + fb_info.size;

    fb_start &= ~(PAGE_SIZE - 1);
    fb_end = (fb_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (unsigned long va = fb_start; va < fb_end; va += PAGE_SIZE)
    {
        mmu_map_page(va, V2P(va), MMU_FLAGS_FRAMEBUFFER);
    }
}
