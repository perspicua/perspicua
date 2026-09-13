/*
 * graphics.h - Public API for kernel graphics primitives.
 */

#ifndef PERSPICUA_DRIVER_GRAPHICS_H
#define PERSPICUA_DRIVER_GRAPHICS_H

#include "types.h"

void graphics_put_pixel(unsigned int x, unsigned int y, uint32_t color);

void graphics_draw_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
                        uint32_t color, int fill);

/*
 * graphics_draw_char - Renders an 8x8 glyph. bg=0xFFFFFFFF implies transparency.
 */
void graphics_draw_char(unsigned int x, unsigned int y, char c, uint32_t fg, uint32_t bg);

void graphics_draw_string(unsigned int x, unsigned int y, const char *s, uint32_t fg, uint32_t bg);

void graphics_clear(uint32_t color);

#endif // PERSPICUA_DRIVER_GRAPHICS_H
