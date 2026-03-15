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
    if (x >= fb_info.width || y >= fb_info.height)
    {
        return;
    }

    uint32_t* fb = (uint32_t*)fb_info.ptr;
    fb[y * (fb_info.pitch >> 2) + x] = color;
}

/*
 * graphics_draw_rect - Renders a rectangle at the given coordinates with width
 * and height dimensions. If the fill flag is set, the interior of the rectangle
 * is painted; otherwise, only the one-pixel wide boundary is drawn.
 */
void graphics_draw_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, uint32_t color, int fill)
{
    if (x >= fb_info.width || y >= fb_info.height)
    {
        return;
    }

    // Adjust width and height to fit within framebuffer bounds to prevent overflow
    if (w > fb_info.width - x)
    {
        w = fb_info.width - x;
    }
    if (h > fb_info.height - y)
    {
        h = fb_info.height - y;
    }

    if (w == 0 || h == 0)
    {
        return;
    }

    uint32_t* fb = (uint32_t*)fb_info.ptr;
    uint32_t stride = fb_info.pitch >> 2;

    if (!fill)
    {
        // Render hollow rectangle outline
        for (unsigned int i = 0; i < w; i++)
        {
            fb[y * stride + (x + i)] = color;
            fb[(y + h - 1) * stride + (x + i)] = color;
        }
        for (unsigned int j = 0; j < h; j++)
        {
            fb[(y + j) * stride + x] = color;
            fb[(y + j) * stride + (x + w - 1)] = color;
        }
        return;
    }

    // Render solid filled rectangle row by row
    uint32_t* line = &fb[y * stride + x];
    for (unsigned int j = 0; j < h; j++)
    {
        for (unsigned int i = 0; i < w; i++)
        {
            line[i] = color;
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
    if ((unsigned char)c >= 128)
    {
        return;
    }

    // Ensure the entire character glyph fits within the screen boundaries
    if (x > fb_info.width - 8 || y > fb_info.height - 8 || fb_info.width < 8 || fb_info.height < 8)
    {
        return;
    }

    uint32_t* fb = (uint32_t*)fb_info.ptr;
    uint32_t stride = fb_info.pitch >> 2;
    uint32_t* dest = &fb[y * stride + x];

    for (int row = 0; row < 8; row++)
    {
        unsigned char row_data = font8x8_basic[(int)c][row];
        for (int col = 0; col < 8; col++)
        {
            if ((row_data >> col) & 1)
            {
                dest[col] = fg;
            }
            else if (bg != 0xFFFFFFFF)
            {
                // Background color transparency check
                dest[col] = bg;
            }
        }
        dest += stride;
    }
}

/*
 * graphics_draw_string - Iteratively renders characters from a null-terminated
 * string. Each character is advanced by 8 pixels horizontally.
 */
void graphics_draw_string(unsigned int x, unsigned int y, const char* s, uint32_t fg, uint32_t bg)
{
    while (*s)
    {
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
    uint32_t* fb = (uint32_t*)fb_info.ptr;
    uint32_t count = fb_info.size >> 2;

    for (uint32_t i = 0; i < count; i++)
    {
        fb[i] = color;
    }
}
