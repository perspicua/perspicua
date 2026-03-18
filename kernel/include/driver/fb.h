/*
 * fb.h - Public API for the framebuffer driver.
 *
 * This file defines the structures and functions used to manage the
 * system's framebuffer, providing access to pixel data and display metrics.
 */

#ifndef PERSPICUA_DRIVER_FB_H
#define PERSPICUA_DRIVER_FB_H

#include "mm/mmu.h"

/*
 * fb_info_struct - Structure containing the hardware framebuffer state.
 */
struct fb_info_struct
{
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int size;
    unsigned char* ptr;

    /* Virtual dimensions and offsets for hardware scrolling. */
    unsigned int v_width;
    unsigned int v_height;
    unsigned int x_offset;
    unsigned int y_offset;
};

/* The global framebuffer device information. */
extern struct fb_info_struct fb_info;

/*
 * fb_init - Initializes the Raspberry Pi 4 framebuffer via mailbox.
 * This sets up the resolution, depth, and allocates the display buffer.
 */
void fb_init(void);

/*
 * fb_set_offset - Sets the virtual offset of the framebuffer.
 * This is used for hardware scrolling by shifting the visible window.
 */
void fb_set_offset(unsigned int x, unsigned int y);

/*
 * remap_framebuffer_pages - Updates the MMU mapping for the framebuffer.
 * Ensures the framebuffer memory is mapped with device-specific attributes.
 */
void remap_framebuffer_pages(void);

#endif /* PERSPICUA_DRIVER_FB_H */
