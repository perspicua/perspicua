/*
 * fb_console.h - Public API for the framebuffer-based console driver.
 *
 * This file defines the interface for initializing and printing text
 * to the screen using the framebuffer.
 */

#ifndef PERSPICUA_DRIVER_FB_CONSOLE_H
#define PERSPICUA_DRIVER_FB_CONSOLE_H

/*
 * fb_console_init - Initializes the framebuffer console.
 * Clears the screen and resets the cursor position.
 */
void fb_console_init(void);

/*
 * fb_console_putc - Prints a single character to the console.
 * @c: The character to print.
 * Handles newlines, carriage returns, and scrolling.
 */
void fb_console_putc(char c);

/*
 * fb_console_puts - Prints a null-terminated string to the console.
 * @s: The string to print.
 */
void fb_console_puts(const char *s);

#endif /* PERSPICUA_DRIVER_FB_CONSOLE_H */
