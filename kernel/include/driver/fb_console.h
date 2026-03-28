/*
 * fb_console.h - Public API for the framebuffer-based console driver.
 *
 * This header defines the interface for text rendering and cursor
 * management on the primary display.
 */

#ifndef PERSPICUA_DRIVER_FB_CONSOLE_H
#define PERSPICUA_DRIVER_FB_CONSOLE_H

/*
 * fb_console_init - Clears the display and resets the cursor.
 */
void fb_console_init(void);

/*
 * fb_console_putc - Renders a character and handles scrolling/newlines.
 */
void fb_console_putc(char c);

/*
 * fb_console_puts - Renders a null-terminated string.
 */
void fb_console_puts(const char *s);

#endif /* PERSPICUA_DRIVER_FB_CONSOLE_H */
