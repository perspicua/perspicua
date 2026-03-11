#include "driver/graphics.h"
#include "driver/fb.h"
#include "string.h"

#define CONSOLE_Y_OFFSET 20
#define CHAR_WIDTH 8
#define CHAR_HEIGHT 8

static unsigned int cursor_x = 0;
static unsigned int cursor_y = CONSOLE_Y_OFFSET;

void fb_console_init(void)
{
    cursor_x = 0;
    cursor_y = CONSOLE_Y_OFFSET;
    graphics_clear(0x00000000); // black
}

static void fb_console_scroll(void)
{
    unsigned int bytes_to_move = (fb_info.height - CONSOLE_Y_OFFSET - CHAR_HEIGHT) * fb_info.pitch;
    void* dst = (void*)((uintptr_t)fb_info.ptr + (CONSOLE_Y_OFFSET * fb_info.pitch));
    void* src = (void*)((uintptr_t)fb_info.ptr + ((CONSOLE_Y_OFFSET + CHAR_HEIGHT) * fb_info.pitch));

    memmove(dst, src, bytes_to_move);

    unsigned int bottom_y = fb_info.height - CHAR_HEIGHT;
    graphics_draw_rect(0, bottom_y, fb_info.width, CHAR_HEIGHT, 0x00000000, 1);

    cursor_y = bottom_y;
}

void fb_console_putc(char c)
{
    if (c == '\n')
    {
        cursor_x = 0;
        cursor_y += CHAR_HEIGHT;
    }
    else if (c == '\b' || c == 127)
    {
        if (cursor_x >= CHAR_WIDTH)
        {
            cursor_x -= CHAR_WIDTH;
            graphics_draw_char(cursor_x, cursor_y, ' ', 0xFFFFFFFF, 0x00000000);
        }
    }
    else
    {
        graphics_draw_char(cursor_x, cursor_y, c, 0xFFFFFFFF, 0x00000000);
        cursor_x += CHAR_WIDTH;

        if (cursor_x + CHAR_WIDTH > fb_info.width)
        {
            cursor_x = 0;
            cursor_y += CHAR_HEIGHT;
        }
    }

    if (cursor_y + CHAR_HEIGHT > fb_info.height)
    {
        fb_console_scroll();
    }
}

void fb_console_puts(const char* s)
{
    while (*s)
    {
        fb_console_putc(*s++);
    }
}
