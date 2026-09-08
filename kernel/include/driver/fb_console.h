/*
 * fb_console.h - Public API for the framebuffer-based console driver.
 */

#ifndef PERSPICUA_DRIVER_FB_CONSOLE_H
#define PERSPICUA_DRIVER_FB_CONSOLE_H

void fb_console_init(void);

void fb_console_putc(char c);

void fb_console_puts(const char *s);

#endif // PERSPICUA_DRIVER_FB_CONSOLE_H
