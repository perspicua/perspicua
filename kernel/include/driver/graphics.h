#ifndef _GRAPHICS_H_
#define _GRAPHICS_H_

#include "types.h"

void graphics_put_pixel(unsigned int x, unsigned int y, uint32_t color);
void graphics_draw_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, uint32_t color, int fill);
void graphics_draw_char(unsigned int x, unsigned int y, char c, uint32_t fg, uint32_t bg);
void graphics_draw_string(unsigned int x, unsigned int y, const char* s, uint32_t fg, uint32_t bg);
void graphics_clear(uint32_t color);

#endif // _GRAPHICS_H_
