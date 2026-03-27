/*
 * graphics.c - Implementation of the kernel graphics primitives.
 *
 * This file contains the implementation of low-level drawing functions,
 * interacting directly with the system's framebuffer for visual rendering.
 */

#include "driver/graphics.h"

#include "string.h"

#include "driver/font8x8.h"
#include "driver/fb.h"

/*
 * graphics_put_pixel - Draws a single pixel at (x, y) with the specified color.
 * Coordinates are checked to ensure they fall within the current framebuffer bounds.
 */
void graphics_put_pixel(unsigned int x, unsigned int y, uint32_t color)
{
    if (x >= fb_info.width || y >= fb_info.height) {
        return;
    }

    uint32_t *fb = (uint32_t *)fb_info.ptr;
    fb[y * (fb_info.pitch >> 2) + x] = color;
}

/*
 * graphics_draw_rect - Renders a rectangle at the given coordinates with width
 * and height dimensions. If the fill flag is set, the interior of the rectangle
 * is painted; otherwise, only the one-pixel wide boundary is drawn.
 */
void graphics_draw_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
                        uint32_t color, int fill)
{
    if (x >= fb_info.width || y >= fb_info.height) {
        return;
    }

    // Adjust width and height to fit within framebuffer bounds to prevent overflow
    if (w > fb_info.width - x) {
        w = fb_info.width - x;
    }
    if (h > fb_info.height - y) {
        h = fb_info.height - y;
    }

    if (w == 0 || h == 0) {
        return;
    }

    uint32_t *fb = (uint32_t *)fb_info.ptr;
    uint32_t stride = fb_info.pitch >> 2;

    if (!fill) {
        // Render hollow rectangle outline
        for (unsigned int i = 0; i < w; i++) {
            fb[y * stride + (x + i)] = color;
            fb[(y + h - 1) * stride + (x + i)] = color;
        }
        for (unsigned int j = 0; j < h; j++) {
            fb[(y + j) * stride + x] = color;
            fb[(y + j) * stride + (x + w - 1)] = color;
        }
        return;
    }

    // Render solid filled rectangle row by row
    uint32_t *line = &fb[y * stride + x];
    uint64_t c64 = ((uint64_t)color << 32) | color;

    for (unsigned int j = 0; j < h; j++) {
        uint32_t *p32 = line;
        unsigned int i = 0;

        // Align to 8-byte boundary
        if ((uintptr_t)p32 & 4) {
            *p32++ = color;
            i++;
        }

        // Write 64-bit blocks
        uint64_t *p64 = (uint64_t *)p32;
        while (i + 1 < w) {
            *p64++ = c64;
            i += 2;
        }

        // Handle trailing 32-bit pixel
        if (i < w) {
            p32 = (uint32_t *)p64;
            *p32 = color;
        }

        line += stride;
    }
}

/*
 * graphics_draw_char - Renders a character glyph from the basic font set.
 * Characters are drawn as 8x8 bitmaps with specified foreground and background
 * colors. If bg is set to 0xFFFFFFFF, the background will be transparent.
 */
void graphics_draw_char(unsigned int x, unsigned int y, char c, uint32_t fg, uint32_t bg)
{
    if ((unsigned char)c >= 128) {
        return;
    }

    // Ensure the entire character glyph fits within the screen boundaries
    if (x > fb_info.width - 8 || y > fb_info.height - 8 || fb_info.width < 8
        || fb_info.height < 8) {
        return;
    }

    uint32_t *fb = (uint32_t *)fb_info.ptr;
    uint32_t stride = fb_info.pitch >> 2;
    uint32_t *dest = &fb[y * stride + x];

    for (int row = 0; row < 8; row++) {
        unsigned char row_data = font8x8_basic[(int)c][row];

        if (bg != 0xFFFFFFFF) {
            dest[0] = (row_data & 0x01) ? fg : bg;
            dest[1] = (row_data & 0x02) ? fg : bg;
            dest[2] = (row_data & 0x04) ? fg : bg;
            dest[3] = (row_data & 0x08) ? fg : bg;
            dest[4] = (row_data & 0x10) ? fg : bg;
            dest[5] = (row_data & 0x20) ? fg : bg;
            dest[6] = (row_data & 0x40) ? fg : bg;
            dest[7] = (row_data & 0x80) ? fg : bg;
        } else {
            /* Standard transparent path */
            for (int col = 0; col < 8; col++) {
                if ((row_data >> col) & 1) {
                    dest[col] = fg;
                }
            }
        }
        dest += stride;
    }
}

/*
 * graphics_draw_string - Iteratively renders characters from a null-terminated
 * string. Each character is advanced by 8 pixels horizontally.
 */
void graphics_draw_string(unsigned int x, unsigned int y, const char *s, uint32_t fg, uint32_t bg)
{
    while (*s) {
        graphics_draw_char(x, y, *s, fg, bg);
        x += 8;
        s++;
    }
}

/*
 * graphics_clear - Fills the entire display area with the given color
 * effectively clearing the screen.
 */
void graphics_clear(uint32_t color)
{
    uint64_t *fb64 = (uint64_t *)fb_info.ptr;
    uint32_t count64 = fb_info.size >> 3;
    uint64_t c64 = ((uint64_t)color << 32) | color;

    for (uint32_t i = 0; i < count64; i++) {
        fb64[i] = c64;
    }

    /* Handle remainder if size is not a multiple of 8 */
    if (fb_info.size & 4) {
        uint32_t *fb32 = (uint32_t *)fb_info.ptr;
        fb32[fb_info.size / 4 - 1] = color;
    }
}
