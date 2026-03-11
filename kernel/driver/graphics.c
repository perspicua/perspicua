#include "driver/graphics.h"
#include "driver/font8x8.h"
#include "driver/fb.h"

void graphics_put_pixel(unsigned int x, unsigned int y, uint32_t color)
{
    if (x >= fb_info.width || y >= fb_info.height)
        return;

    uint32_t* pixel_ptr = (uint32_t*)fb_info.ptr;
    uint32_t pixels_per_row = fb_info.pitch / 4;
    pixel_ptr[y * pixels_per_row + x] = color;
}

void graphics_draw_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, uint32_t color, int fill)
{
    if (!fill)
    {
        for (unsigned int j = y; j < y + h; j++)
        {
            graphics_put_pixel(x, j, color);
            graphics_put_pixel(x + w - 1, j, color);
        }

        for (unsigned int i = x; i < x + w; i++)
        {
            graphics_put_pixel(i, y, color);
            graphics_put_pixel(i, y + h - 1, color);
        }
        return;
    }

    for (unsigned int j = y; j < y + h; j++)
    {
        for (unsigned int i = x; i < x + w; i++)
        {
            graphics_put_pixel(i, j, color);
        }
    }
}

void graphics_draw_char(unsigned int x, unsigned int y, char c, uint32_t fg, uint32_t bg)
{
    if (c < 0 || c >= 128)
        return;

    for (int row = 0; row < 8; row++)
    {
        unsigned char row_data = font8x8_basic[(int)c][row];
        for (int col = 0; col < 8; col++)
        {
            if ((row_data >> col) & 1)
            {
                graphics_put_pixel(x + col, y + row, fg);
            }
            else if (bg != 0xFFFFFFFF)
            {
                graphics_put_pixel(x + col, y + row, bg);
            }
        }
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
    for (unsigned int y = 0; y < fb_info.height; y++)
        for (unsigned int x = 0; x < fb_info.width; x++)
            graphics_put_pixel(x, y, color);
}
