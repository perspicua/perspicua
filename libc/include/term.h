/*
 * term.h - Full-screen terminal programs: decoded keys and a diffed screen.
 *
 * The console is a raw 80x24 terminal. term_open() takes it over, and
 * term_close(), exit(), Ctrl-C and Ctrl-Z all hand it back as they found it.
 * Drawing goes to an off-screen buffer; term_present() sends only the cells
 * that changed, which is what keeps a game playable over a 115200 baud line.
 */

#ifndef PERSPICUA_LIBC_TERM_H
#define PERSPICUA_LIBC_TERM_H

#include <stdint.h>

#define TERM_ROWS 24
#define TERM_COLS 80

// Colors for TERM_FG and TERM_BG; TERM_DEFAULT is the terminal's own.
#define TERM_BLACK   0
#define TERM_RED     1
#define TERM_GREEN   2
#define TERM_YELLOW  3
#define TERM_BLUE    4
#define TERM_MAGENTA 5
#define TERM_CYAN    6
#define TERM_WHITE   7
#define TERM_DEFAULT 9

#define TERM_FG(c)  ((unsigned)(c))
#define TERM_BG(c)  ((unsigned)(c) << 4)
#define TERM_BOLD   0x100u
#define TERM_NORMAL (TERM_FG(TERM_DEFAULT) | TERM_BG(TERM_DEFAULT))

// What term_key() returns besides plain bytes.
#define TERM_KEY_NONE      (-1)
#define TERM_KEY_ENTER     '\r'
#define TERM_KEY_ESC       27
#define TERM_KEY_BACKSPACE 127
#define TERM_KEY_UP        0x100
#define TERM_KEY_DOWN      0x101
#define TERM_KEY_RIGHT     0x102
#define TERM_KEY_LEFT      0x103
#define TERM_KEY_HOME      0x104
#define TERM_KEY_END       0x105
#define TERM_KEY_PGUP      0x106
#define TERM_KEY_PGDN      0x107
#define TERM_KEY_DELETE    0x108
#define TERM_KEY_INSERT    0x109

// Takes over the terminal; -1 if this program already has it.
int term_open(void);
void term_close(void);

// Waits up to timeout_ms for a key (0 polls, negative waits forever).
int term_key(int timeout_ms);

void term_clear(unsigned attr);
void term_put(int row, int col, uint32_t codepoint, unsigned attr);
// Writes UTF-8 text from (row, col), clipped at the right edge; returns the columns used.
int term_print(int row, int col, const char *utf8, unsigned attr);
void term_present(void);
// Makes the next term_present() repaint the whole screen.
void term_redraw(void);

// Milliseconds on a monotonic clock, for frame timing.
unsigned long term_ms(void);

// Decodes one key from buf; *used is 0 while buf holds only the start of a sequence.
int term_decode(const unsigned char *buf, int len, int *used);

#endif // PERSPICUA_LIBC_TERM_H
