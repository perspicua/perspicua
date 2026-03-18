/*
 * fb_console.c - Implementation of the framebuffer-based console driver.
 *
 * This file handles character rendering, cursor management, and
 * vertical scrolling for the system console.
 */

#include "driver/fb_console.h"

#include "string.h"
#include "core/lock.h"

#include "driver/graphics.h"
#include "driver/fb.h"
#include "driver/dashboard.h"

/* Console dimensions and rendering offsets. */
#define CONSOLE_Y_OFFSET 20
#define CHAR_WIDTH       8
#define CHAR_HEIGHT      12

/* Cursor state and access protection. */
static unsigned int cursor_x = 0;
static unsigned int cursor_y = CONSOLE_Y_OFFSET;
static spinlock_t fb_console_lock = SPINLOCK_INIT;

/*
 * fb_console_scroll - Scrolls the console up by one character line.
 */
static void fb_console_scroll(void)
{
    unsigned int next_y_offset = fb_info.y_offset + CHAR_HEIGHT;

    if (next_y_offset + fb_info.height <= fb_info.v_height)
    {
        fb_set_offset(0, next_y_offset);

        graphics_draw_rect(
            0, fb_info.y_offset + fb_info.height - CHAR_HEIGHT, fb_info.width, CHAR_HEIGHT, 0x00000000, 1);
    }
    else
    {
        void* dst = (void*)((uintptr_t)fb_info.ptr);
        void* src = (void*)((uintptr_t)fb_info.ptr + (fb_info.y_offset * fb_info.pitch));
        unsigned int size = fb_info.height * fb_info.pitch;

        memmove(dst, src, size);

        fb_set_offset(0, 0);

        dashboard_update();

        fb_set_offset(0, CHAR_HEIGHT);
        graphics_draw_rect(
            0, fb_info.y_offset + fb_info.height - CHAR_HEIGHT, fb_info.width, CHAR_HEIGHT, 0x00000000, 1);
    }

    cursor_y = fb_info.y_offset + fb_info.height - CHAR_HEIGHT;
}

/*
 * fb_console_init - Initializes the framebuffer console.
 */
void fb_console_init(void)
{
    spin_lock(&fb_console_lock);
    fb_set_offset(0, 0);
    cursor_x = 0;
    cursor_y = CONSOLE_Y_OFFSET;
    graphics_clear(0x00000000);  // black
    dashboard_update();
    spin_unlock(&fb_console_lock);
}

/*
 * fb_console_putc - Prints a single character to the console.
 */
void fb_console_putc(char c)
{
    if (!fb_info.ptr)
    {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&fb_console_lock);

    if (c == '\n')
    {
        cursor_x = 0;
        cursor_y += CHAR_HEIGHT;
    }
    else if (c == '\r')
    {
        cursor_x = 0;
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

    if (cursor_y + CHAR_HEIGHT > fb_info.y_offset + fb_info.height)
    {
        fb_console_scroll();
    }

    spin_unlock_irqrestore(&fb_console_lock, flags);
}

/*
 * fb_console_puts - Prints a null-terminated string to the console.
 */
void fb_console_puts(const char* s)
{
    while (*s)
    {
        fb_console_putc(*s++);
    }
}
