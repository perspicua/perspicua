/*
 * fb.h - Public API for the framebuffer driver.
 *
 * This header defines the structures and functions used to manage the
 * system's display buffer and pixel metadata.
 */

#ifndef PERSPICUA_DRIVER_FB_H
#define PERSPICUA_DRIVER_FB_H

#include "types.h"

/* --- Data Structures --- */

/*
 * struct fb_info_struct - Hardware framebuffer state.
 */
struct fb_info_struct {
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int size;
    unsigned char *ptr;
};

/* Global framebuffer state accessible by other drivers (e.g., graphics) */
extern struct fb_info_struct fb_info;

/* --- Function Prototypes --- */

/*
 * fb_init - Configures the resolution and allocates the GPU display buffer.
 */
void fb_init(void);

/*
 * fb_register_device - Exposes the framebuffer through devfs (/dev/fb0).
 */
void fb_register_device(void);

/*
 * remap_framebuffer_pages - Applies MMU attributes for device memory.
 */
void remap_framebuffer_pages(void);

#endif /* PERSPICUA_DRIVER_FB_H */
