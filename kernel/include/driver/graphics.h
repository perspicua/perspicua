/*
 * graphics.h - Public API for the kernel graphics primitives.
 *
 * This file defines basic drawing functions for pixels, rectangles,
 * characters, and strings, operating directly on the system framebuffer.
 */

#ifndef PERSPICUA_DRIVER_GRAPHICS_H
#define PERSPICUA_DRIVER_GRAPHICS_H

#include "types.h"

/*
 * graphics_put_pixel - Draws a single pixel at the specified x and y
 * coordinates using the provided 32-bit ARGB color value.
 */
void graphics_put_pixel(unsigned int x, unsigned int y, uint32_t color);

/*
 * graphics_draw_rect - Draws a rectangle starting at (x, y) with the given
 * width and height. If fill is non-zero, the entire rectangle area is painted
 * with the specified color; otherwise, only the outline is drawn.
 */
void graphics_draw_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
                        uint32_t color, int fill);

/*
 * graphics_draw_char - Renders a single 8x8 character from the font set at
 * the specified coordinates. The fg color is used for the glyph, while bg
 * is used for the background (unless bg is 0xFFFFFFFF, which implies transparency).
 */
void graphics_draw_char(unsigned int x, unsigned int y, char c, uint32_t fg, uint32_t bg);

/*
 * graphics_draw_string - Renders a null-terminated string starting at the
 * given coordinates using the specified foreground and background colors.
 */
void graphics_draw_string(unsigned int x, unsigned int y, const char *s, uint32_t fg, uint32_t bg);

/*
 * graphics_clear - Fills the entire screen with a single specified color
 * by overwriting the entire framebuffer.
 */
void graphics_clear(uint32_t color);

#endif /* PERSPICUA_DRIVER_GRAPHICS_H */
