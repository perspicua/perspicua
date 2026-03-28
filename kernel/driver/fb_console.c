/*
 * fb_console.c - Implementation of the framebuffer-based console driver.
 *
 * This module handles character rendering, cursor tracking, and
 * vertical scrolling for the system console.
 */

#include "driver/fb_console.h"

#include "stdio.h"
#include "string.h"

#include "core/lock.h"
#include "driver/graphics.h"
#include "driver/fb.h"

#define CONSOLE_Y_OFFSET 20
#define CHAR_WIDTH       8
#define CHAR_HEIGHT      12

static unsigned int cursor_x = 0;
static unsigned int cursor_y = CONSOLE_Y_OFFSET;
static spinlock_t fb_console_lock = SPINLOCK_INIT;

/*
 * fb_console_scroll - Moves existing lines up and clears the bottom line.
 */
static void fb_console_scroll(void)
{
    unsigned int bytes_to_move = (fb_info.height - CONSOLE_Y_OFFSET - CHAR_HEIGHT) * fb_info.pitch;

    void *dst = (void *)((uintptr_t)fb_info.ptr + (CONSOLE_Y_OFFSET * fb_info.pitch));
    void *src =
        (void *)((uintptr_t)fb_info.ptr + ((CONSOLE_Y_OFFSET + CHAR_HEIGHT) * fb_info.pitch));

    memmove(dst, src, bytes_to_move);

    unsigned int bottom_y = fb_info.height - CHAR_HEIGHT;
    graphics_draw_rect(0, bottom_y, fb_info.width, CHAR_HEIGHT, 0x00000000, 1);

    cursor_y = bottom_y;
}

/*
 * fb_console_putc_unlocked - Internal rendering logic without synchronization.
 */
static int ansi_state = 0;
static void fb_console_putc_unlocked(char c)
{
    if (ansi_state == 0) {
        if (c == '\033') {
            ansi_state = 1;
        } else if (c == '\n') {
            cursor_x = 0;
            cursor_y += CHAR_HEIGHT;
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\b' || c == 127) {
            if (cursor_x >= CHAR_WIDTH) {
                cursor_x -= CHAR_WIDTH;
                graphics_draw_char(cursor_x, cursor_y, ' ', 0xFFFFFFFF, 0x00000000);
            }
        } else {
            graphics_draw_char(cursor_x, cursor_y, c, 0xFFFFFFFF, 0x00000000);
            cursor_x += CHAR_WIDTH;

            if (cursor_x + CHAR_WIDTH > fb_info.width) {
                cursor_x = 0;
                cursor_y += CHAR_HEIGHT;
            }
        }
    } else if (ansi_state == 1) {
        if (c == '[') {
            ansi_state = 2;
        } else {
            ansi_state = 0;
        }
    } else if (ansi_state == 2) {
        if (c == 'H') {
            cursor_x = 0;
            cursor_y = CONSOLE_Y_OFFSET;
            ansi_state = 0;
        } else if (c == '2' || c == '3') {
            /* Support for 2J and 3J */
            ansi_state = 3;
        } else if (c == 'J') {
            /* Just J? Clear from cursor to end. For now, we only care about 2J/3J */
            ansi_state = 0;
        } else {
            /* Any other sequence? Just reset state for now */
            ansi_state = 0;
        }
    } else if (ansi_state == 3) {
        if (c == 'J') {
            /* Clear screen area (but not dashboard) */
            graphics_draw_rect(0, CONSOLE_Y_OFFSET, fb_info.width,
                               fb_info.height - CONSOLE_Y_OFFSET, 0x00000000, 1);
        }
        ansi_state = 0;
    }

    if (cursor_y + CHAR_HEIGHT > fb_info.height) {
        fb_console_scroll();
    }
}

/*
 * fb_console_init - Resets the cursor and wipes the screen.
 */
void fb_console_init(void)
{
    spin_lock(&fb_console_lock);
    cursor_x = 0;
    cursor_y = CONSOLE_Y_OFFSET;
    graphics_clear(0x00000000);
    spin_unlock(&fb_console_lock);

    pr_info("fb: console initialized\n");
}

/*
 * fb_console_putc - Thread-safe character output.
 */
void fb_console_putc(char c)
{
    unsigned long flags = spin_lock_irqsave(&fb_console_lock);
    fb_console_putc_unlocked(c);
    spin_unlock_irqrestore(&fb_console_lock, flags);
}

/*
 * fb_console_puts - Thread-safe string output.
 */
void fb_console_puts(const char *s)
{
    unsigned long flags = spin_lock_irqsave(&fb_console_lock);
    while (*s) {
        fb_console_putc_unlocked(*s++);
    }
    spin_unlock_irqrestore(&fb_console_lock, flags);
}
