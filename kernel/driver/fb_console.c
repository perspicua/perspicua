/*
 * fb_console.c - Terminal console implementation over the framebuffer.
 */

#include "driver/fb_console.h"

#include <stdint.h>

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
// Never taken from IRQ context, so a scroll moving megabytes leaves IRQs on.
static spinlock_t fb_console_lock = SPINLOCK_INIT;
static int console_hidden;

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

#define CSI_PARAMS    4
#define CSI_PARAM_MAX 9999

static int ansi_state; // 0 text, 1 after ESC, 2 inside a CSI sequence
static int csi_private;
static int csi_count;
static unsigned int csi_params[CSI_PARAMS];

static void clear_area(unsigned int x, unsigned int y, unsigned int w, unsigned int h)
{
    graphics_draw_rect(x, y, w, h, 0x00000000, 1);
}

static unsigned int last_row_y(void)
{
    unsigned int rows =
        fb_info.height > CONSOLE_Y_OFFSET ? (fb_info.height - CONSOLE_Y_OFFSET) / CHAR_HEIGHT : 0;
    return CONSOLE_Y_OFFSET + (rows ? rows - 1 : 0) * CHAR_HEIGHT;
}

static unsigned int last_col_x(void)
{
    unsigned int cols = fb_info.width / CHAR_WIDTH;
    return (cols ? cols - 1 : 0) * CHAR_WIDTH;
}

// Acts on a whole CSI sequence by its final byte; ones with nothing to draw only end.
static void csi_dispatch(char final)
{
    unsigned int mode = csi_params[0];
    unsigned int n = mode ? mode : 1;
    unsigned int line_end = cursor_y + CHAR_HEIGHT;

    if (csi_private) {
        return;
    }
    switch (final) {
        case 'A':
            cursor_y = cursor_y >= CONSOLE_Y_OFFSET + n * CHAR_HEIGHT ? cursor_y - n * CHAR_HEIGHT
                                                                      : CONSOLE_Y_OFFSET;
            break;
        case 'B':
            cursor_y = cursor_y + n * CHAR_HEIGHT < last_row_y() ? cursor_y + n * CHAR_HEIGHT
                                                                 : last_row_y();
            break;
        case 'C':
            cursor_x =
                cursor_x + n * CHAR_WIDTH < last_col_x() ? cursor_x + n * CHAR_WIDTH : last_col_x();
            break;
        case 'D':
            cursor_x = cursor_x >= n * CHAR_WIDTH ? cursor_x - n * CHAR_WIDTH : 0;
            break;
        case 'H':
        case 'f': {
            unsigned int row = csi_params[0] ? csi_params[0] - 1 : 0;
            unsigned int col = csi_count >= 1 && csi_params[1] ? csi_params[1] - 1 : 0;
            cursor_y = CONSOLE_Y_OFFSET + row * CHAR_HEIGHT;
            cursor_x = col * CHAR_WIDTH;
            cursor_y = cursor_y < last_row_y() ? cursor_y : last_row_y();
            cursor_x = cursor_x < last_col_x() ? cursor_x : last_col_x();
            break;
        }
        case 'J':
            if (mode == 0) {
                clear_area(cursor_x, cursor_y, fb_info.width, CHAR_HEIGHT);
                clear_area(0, line_end, fb_info.width, fb_info.height);
            } else if (mode == 1) {
                clear_area(0, CONSOLE_Y_OFFSET, fb_info.width, cursor_y - CONSOLE_Y_OFFSET);
                clear_area(0, cursor_y, cursor_x + CHAR_WIDTH, CHAR_HEIGHT);
            } else {
                // The dashboard above the console area stays.
                clear_area(0, CONSOLE_Y_OFFSET, fb_info.width, fb_info.height);
            }
            break;
        case 'K':
            if (mode == 0) {
                clear_area(cursor_x, cursor_y, fb_info.width, CHAR_HEIGHT);
            } else if (mode == 1) {
                clear_area(0, cursor_y, cursor_x + CHAR_WIDTH, CHAR_HEIGHT);
            } else {
                clear_area(0, cursor_y, fb_info.width, CHAR_HEIGHT);
            }
            break;
    }
}

static void fb_console_putc_unlocked(char c)
{
    if (console_hidden) {
        return;
    }
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
        // Anything but CSI is a two-byte escape such as ESC 7, with nothing to draw.
        ansi_state = c == '[' ? 2 : 0;
        csi_private = 0;
        csi_count = 0;
        memset(csi_params, 0, sizeof(csi_params));
    } else if (c >= '0' && c <= '9') {
        if (csi_count < CSI_PARAMS) {
            unsigned int v = csi_params[csi_count] * 10 + (unsigned int)(c - '0');
            csi_params[csi_count] = v < CSI_PARAM_MAX ? v : CSI_PARAM_MAX;
        }
    } else if (c == ';' || c == ':') {
        csi_count++;
    } else if (c >= 0x3c && c <= 0x3f) {
        csi_private = 1;
    } else if (c >= 0x40 && c <= 0x7e) {
        csi_dispatch(c);
        ansi_state = 0;
    } else if (c < 0x20 || c > 0x2f) {
        // A control byte cuts the sequence off; intermediates (0x20-0x2f) are skipped.
        ansi_state = 0;
    }

    if (cursor_y + CHAR_HEIGHT > fb_info.height) {
        fb_console_scroll();
    }
}

void fb_console_init(void)
{
    spin_lock(&fb_console_lock);
    cursor_x = 0;
    cursor_y = CONSOLE_Y_OFFSET;
    graphics_clear(0x00000000);
    spin_unlock(&fb_console_lock);

    pr_info("fb: console initialized\n");
}

void fb_console_putc(char c)
{
    spin_lock(&fb_console_lock);
    fb_console_putc_unlocked(c);
    spin_unlock(&fb_console_lock);
}

void fb_console_puts(const char *s)
{
    spin_lock(&fb_console_lock);
    while (*s) {
        fb_console_putc_unlocked(*s++);
    }
    spin_unlock(&fb_console_lock);
}

#ifdef CONFIG_TESTS
void fb_console_test_feed(const char *s, unsigned int *x, unsigned int *y)
{
    spin_lock(&fb_console_lock);
    unsigned int saved_x = cursor_x, saved_y = cursor_y;
    int hidden = console_hidden;
    console_hidden = 0;
    ansi_state = 0;
    cursor_x = *x;
    cursor_y = *y;
    while (*s) {
        fb_console_putc_unlocked(*s++);
    }
    *x = cursor_x;
    *y = cursor_y;
    cursor_x = saved_x;
    cursor_y = saved_y;
    console_hidden = hidden;
    ansi_state = 0;
    spin_unlock(&fb_console_lock);
}
#endif

void fb_console_hide(int hidden)
{
    spin_lock(&fb_console_lock);
    console_hidden = hidden;
    // A sequence cut off while hidden must not swallow what comes after it.
    ansi_state = 0;
    spin_unlock(&fb_console_lock);
}
