#include "driver/graphics.h"
#include "driver/font8x8.h"
#include "driver/fb.h"
#include "string.h"

void graphics_put_pixel(unsigned int x, unsigned int y, uint32_t color)
{
    if (x >= fb_info.width || y >= fb_info.height)
        return;

    uint32_t* fb = (uint32_t*)fb_info.ptr;
    fb[y * (fb_info.pitch >> 2) + x] = color;
}

void graphics_draw_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, uint32_t color, int fill)
{
    if (x >= fb_info.width || y >= fb_info.height)
        return;

    // Use safer comparisons to prevent integer overflow
    if (w > fb_info.width - x)
        w = fb_info.width - x;
    if (h > fb_info.height - y)
        h = fb_info.height - y;

    if (w == 0 || h == 0)
        return;

    uint32_t* fb = (uint32_t*)fb_info.ptr;
    uint32_t stride = fb_info.pitch >> 2;

    if (!fill)
    {
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

void graphics_draw_char(unsigned int x, unsigned int y, char c, uint32_t fg, uint32_t bg)
{
    if ((unsigned char)c >= 128)
        return;

    // Safer bounds check
    if (x > fb_info.width - 8 || y > fb_info.height - 8 || fb_info.width < 8 || fb_info.height < 8)
        return;

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
                dest[col] = bg;
            }
        }
        dest += stride;
    }
}

void graphics_draw_string(unsigned int x, unsigned int y, const char* s, uint32_t fg, uint32_t bg)
{
    while (*s)
    {
        graphics_draw_char(x, y, *s, fg, bg);
        x += 8;
        s++;
    }
}

void graphics_clear(uint32_t color)
{
    uint32_t* fb = (uint32_t*)fb_info.ptr;
    uint32_t count = fb_info.size >> 2;

    for (uint32_t i = 0; i < count; i++)
    {
        fb[i] = color;
    }
}
