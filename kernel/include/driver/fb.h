/*
 * fb.h - Public API for the framebuffer driver.
 */

#ifndef PERSPICUA_DRIVER_FB_H
#define PERSPICUA_DRIVER_FB_H

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

// Global framebuffer state accessible by other drivers (e.g., graphics)
extern struct fb_info_struct fb_info;

void fb_init(void);

void fb_register_device(void);

void fb_remap_pages(void);

#endif // PERSPICUA_DRIVER_FB_H
