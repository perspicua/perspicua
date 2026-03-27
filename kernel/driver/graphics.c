/*
 * graphics.c - Implementation of kernel graphics primitives.
 *
 * This module provides low-level drawing functions that interact
 * directly with the primary system framebuffer.
 */

#include "driver/graphics.h"

#include "string.h"

#include "driver/fb.h"
#include "driver/font8x8.h"

/* --- Public API Implementations --- */

/*
 * graphics_put_pixel - Updates a single memory location in the framebuffer.
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
 * graphics_draw_rect - Renders solid or hollow boxes with clipping.
 */
void graphics_draw_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
                        uint32_t color, int fill)
{
    if (x >= fb_info.width || y >= fb_info.height) {
        return;
    }

    /* Clip dimensions to prevent screen overflow */
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

    /* Solid fill optimized with 64-bit writes where aligned */
    uint32_t *line = &fb[y * stride + x];
    uint64_t c64 = ((uint64_t)color << 32) | color;

    for (unsigned int j = 0; j < h; j++) {
        uint32_t *p32 = line;
        unsigned int i = 0;

        if ((uintptr_t)p32 & 4) {
            *p32++ = color;
            i++;
        }

        uint64_t *p64 = (uint64_t *)p32;
        while (i + 1 < w) {
            *p64++ = c64;
            i += 2;
        }

        if (i < w) {
            p32 = (uint32_t *)p64;
            *p32 = color;
        }

        line += stride;
    }
}

/*
 * graphics_draw_char - Renders a fixed-size character from font8x8.
 */
void graphics_draw_char(unsigned int x, unsigned int y, char c, uint32_t fg, uint32_t bg)
{
    if ((unsigned char)c >= 128) {
        return;
    }

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
            /* Optimized transparency path */
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
 * graphics_draw_string - Maps a character array to the framebuffer.
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
 * graphics_clear - Wipes the display area with a solid color.
 */
void graphics_clear(uint32_t color)
{
    uint64_t *fb64 = (uint64_t *)fb_info.ptr;
    uint32_t count64 = fb_info.size >> 3;
    uint64_t c64 = ((uint64_t)color << 32) | color;

    for (uint32_t i = 0; i < count64; i++) {
        fb64[i] = c64;
    }

    if (fb_info.size & 4) {
        uint32_t *fb32 = (uint32_t *)fb_info.ptr;
        fb32[fb_info.size / 4 - 1] = color;
    }
}
