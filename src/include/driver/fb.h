#ifndef _FB_H_
#define _FB_H_

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

#endif // _FB_H_
