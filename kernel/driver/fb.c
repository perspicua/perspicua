/*
 * fb.c - Implementation of the framebuffer driver.
 *
 * This file handles the low-level mailbox communication with the
 * VideoCore GPU to set up and manage the display buffer.
 */

#include "driver/fb.h"

#include "types.h"
#include "stdio.h"
#include "mm/addr.h"

#include "mm/pmm.h"

#include "driver/mailbox.h"

/* The mailbox communication buffer for GPU requests. */
static __attribute__((aligned(16))) unsigned int mbox[36];

/* The global framebuffer device information. */
struct fb_info_struct fb_info;

/*
 * fb_init - Initializes the Raspberry Pi 4 framebuffer.
 */
void fb_init(void)
{
    /* 1. Set Physical Width/Height (1024x768) */
    mbox[0] = 8 * 4;
    mbox[1] = 0;
    mbox[2] = 0x48003;
    mbox[3] = 8;
    mbox[4] = 8;
    mbox[5] = 1024;
    mbox[6] = 768;
    mbox[7] = 0;
    mbox_call(mbox);

    /* 2. Set Virtual Width/Height (1024x4096 for hardware scrolling) */
    mbox[0] = 8 * 4;
    mbox[1] = 0;
    mbox[2] = 0x48004;
    mbox[3] = 8;
    mbox[4] = 8;
    mbox[5] = 1024;
    mbox[6] = 4096;
    mbox[7] = 0;
    mbox_call(mbox);

    /* 3. Set Depth (32-bit) */
    mbox[0] = 7 * 4;
    mbox[1] = 0;
    mbox[2] = 0x48005;
    mbox[3] = 4;
    mbox[4] = 4;
    mbox[5] = 32;
    mbox[6] = 0;
    mbox_call(mbox);

    /* 4. Allocate Buffer */
    mbox[0] = 8 * 4;
    mbox[1] = 0;
    mbox[2] = 0x40001;
    mbox[3] = 8;
    mbox[4] = 8;
    mbox[5] = 4096;
    mbox[6] = 0;
    mbox[7] = 0;
    mbox_call(mbox);

    if (mbox[1] == 0x80000000 && mbox[6] != 0)
    {
        uintptr_t phys_addr = mbox[5] & 0x3FFFFFFF;
        unsigned int fb_size = mbox[6];

        /* 5. Get Pitch */
        mbox[0] = 7 * 4;
        mbox[1] = 0;
        mbox[2] = 0x40008;
        mbox[3] = 4;
        mbox[4] = 4;
        mbox[5] = 0;
        mbox[6] = 0;
        mbox_call(mbox);

        fb_info.width = 1024;
        fb_info.height = 768;
        fb_info.v_width = 1024;
        fb_info.v_height = 4096;
        fb_info.pitch = mbox[5];
        fb_info.size = fb_size;
        fb_info.x_offset = 0;
        fb_info.y_offset = 0;
        fb_info.ptr = (unsigned char*)P2V(phys_addr);

        pmm_reserve_range((unsigned long)phys_addr, fb_info.size, "framebuffer");
    }
    else
    {
        printf("[   FB ] Error: VideoCore failed to allocate framebuffer\n");
    }
}

/*
 * fb_set_hardware_offset - Sets the virtual offset of the framebuffer.
 */
void fb_set_hardware_offset(unsigned int x, unsigned int y)
{
    __attribute__((aligned(16))) unsigned int mbox_off[8];

    mbox_off[0] = 8 * 4;
    mbox_off[1] = 0;
    mbox_off[2] = 0x48009;  // SET_VIRTUAL_OFFSET
    mbox_off[3] = 8;
    mbox_off[4] = 8;
    mbox_off[5] = x;
    mbox_off[6] = y;
    mbox_off[7] = 0;

    mbox_call(mbox_off);
}

/*
 * fb_set_offset - Updates the framebuffer info and then calls fb_set_hardware_offset.
 */
void fb_set_offset(unsigned int x, unsigned int y)
{
    fb_info.x_offset = x;
    fb_info.y_offset = y;
    fb_set_hardware_offset(x, y);
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
        mmu_map_page(va, V2P(va), MMU_FLAGS_DEVICE_RW);
    }
}
