#ifndef _FB_H_
#define _FB_H_

#include "mmu.h"
struct framebuffer
{
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int size;
    unsigned char* ptr;
};
extern struct framebuffer fb_info;

void fb_init(void);
void remap_framebuffer_pages(void);
#endif // _FB_H_
