/*
 * fb_console.h - Public API for the framebuffer-based console driver.
 */

#ifndef PERSPICUA_DRIVER_FB_CONSOLE_H
#define PERSPICUA_DRIVER_FB_CONSOLE_H

void fb_console_init(void);

void fb_console_putc(char c);

// Keeps console text off the screen while a program draws there; the serial side is unaffected.
void fb_console_hide(int hidden);

#ifdef CONFIG_TESTS
// Feeds s to the console with the cursor at (*x, *y), and leaves where it ended there.
void fb_console_test_feed(const char *s, unsigned int *x, unsigned int *y);
#endif

#endif // PERSPICUA_DRIVER_FB_CONSOLE_H
