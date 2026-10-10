/*
 * term.c - Full-screen terminal programs: decoded keys and a diffed screen.
 */

#include "term.h"

#include <stddef.h>

#include "fcntl.h"
#include "signal.h"
#include "stdlib.h"
#include "string.h"
#include "time.h"
#include "unistd.h"

#define ESC_WAIT_MS 50 // how long a lone ESC waits for the rest of a sequence
#define CSI_MAX     16 // longest escape sequence kept whole; longer ones are dropped
#define OUT_MAX     4096

#define SCREEN_TAKE    "\033[0m\033[2J\033[H\033[?25l"
#define SCREEN_RELEASE "\033[0m\033[2J\033[H\033[?25h"

struct cell {
    uint32_t cp;
    uint16_t attr;
};

static struct cell back[TERM_ROWS][TERM_COLS];
static struct cell front[TERM_ROWS][TERM_COLS];

static int is_open;
static int atexit_done;
static int saved_flags = -1;
static int repaint;

static unsigned char kbuf[CSI_MAX];
static int klen;
static unsigned long esc_since;

static char out_buf[OUT_MAX];
static int out_len;
static int cur_row = -1, cur_col = -1;
static int cur_attr = -1;
static int cursor_row = -1, cursor_col;
static int cursor_shown;

static const int caught[] = {SIGINT, SIGTERM, SIGTSTP, SIGCONT};
static struct sigaction old_actions[sizeof(caught) / sizeof(caught[0])];

static void write_all(const char *s, size_t n)
{
    while (n > 0) {
        int w = write(1, s, n);
        if (w <= 0) {
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

// Signal handlers write straight to the fd: out_buf may be half full when one fires.
static void write_str(const char *s)
{
    write_all(s, strlen(s));
}

static void flush(void)
{
    write_all(out_buf, (size_t)out_len);
    out_len = 0;
}

static void emit(const char *s, int n)
{
    if (out_len + n > OUT_MAX) {
        flush();
    }
    memcpy(out_buf + out_len, s, (size_t)n);
    out_len += n;
}

static void emit_num(int v)
{
    char digits[12];
    int n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    while (n > 0) {
        emit(&digits[--n], 1);
    }
}

static void emit_utf8(uint32_t cp)
{
    char b[4];
    int n;
    if (cp < 0x20 || cp == 0x7f || cp > 0x10ffff) {
        cp = '?';
    }
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xc0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3f));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xe0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        b[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        b[0] = (char)(0xf0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        b[3] = (char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    emit(b, n);
}

static void blank(struct cell grid[TERM_ROWS][TERM_COLS], unsigned attr)
{
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            grid[r][c].cp = ' ';
            grid[r][c].attr = (uint16_t)attr;
        }
    }
}

static void take_terminal(void)
{
    if (saved_flags >= 0) {
        fcntl(0, F_SETFL, saved_flags | O_NONBLOCK);
    }
    write_str(SCREEN_TAKE);
    cursor_shown = 0;
    repaint = 1;
}

static void release_terminal(void)
{
    write_str(SCREEN_RELEASE);
    if (saved_flags >= 0) {
        fcntl(0, F_SETFL, saved_flags);
    }
}

static void set_handler(int sig, sighandler_t handler, struct sigaction *old)
{
    struct sigaction act = {0};
    act.sa_handler = handler;
    act.sa_flags = SA_RESTART;
    sigaction(sig, &act, old);
}

// The signal stays blocked until this handler returns, then takes its default action.
static void on_leave(int sig)
{
    release_terminal();
    set_handler(sig, SIG_DFL, NULL);
    kill(getpid(), sig);
}

static void on_continue(int sig)
{
    (void)sig;
    set_handler(SIGTSTP, on_leave, NULL);
    take_terminal();
}

int term_open(void)
{
    if (is_open) {
        return -1;
    }

    saved_flags = fcntl(0, F_GETFL, 0);
    blank(back, TERM_NORMAL);
    klen = 0;
    esc_since = 0;
    out_len = 0;
    cursor_row = -1;

    for (size_t i = 0; i < sizeof(caught) / sizeof(caught[0]); i++) {
        set_handler(caught[i], caught[i] == SIGCONT ? on_continue : on_leave, &old_actions[i]);
    }
    if (!atexit_done) {
        atexit(term_close);
        atexit_done = 1;
    }

    take_terminal();
    is_open = 1;
    return 0;
}

void term_close(void)
{
    if (!is_open) {
        return;
    }
    is_open = 0;

    for (size_t i = 0; i < sizeof(caught) / sizeof(caught[0]); i++) {
        sigaction(caught[i], &old_actions[i], NULL);
    }
    flush();
    release_terminal();
}

unsigned long term_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000 + (unsigned long)ts.tv_nsec / 1000000;
}

static int csi_key(int param, unsigned char final)
{
    switch (final) {
        case 'A':
            return TERM_KEY_UP;
        case 'B':
            return TERM_KEY_DOWN;
        case 'C':
            return TERM_KEY_RIGHT;
        case 'D':
            return TERM_KEY_LEFT;
        case 'H':
            return TERM_KEY_HOME;
        case 'F':
            return TERM_KEY_END;
        case '~':
            switch (param) {
                case 1:
                case 7:
                    return TERM_KEY_HOME;
                case 2:
                    return TERM_KEY_INSERT;
                case 3:
                    return TERM_KEY_DELETE;
                case 4:
                case 8:
                    return TERM_KEY_END;
                case 5:
                    return TERM_KEY_PGUP;
                case 6:
                    return TERM_KEY_PGDN;
            }
            break;
    }
    return TERM_KEY_NONE;
}

int term_decode(const unsigned char *buf, int len, int *used)
{
    *used = 0;
    if (len <= 0) {
        return TERM_KEY_NONE;
    }

    if (buf[0] != 27) {
        *used = 1;
        if (buf[0] == '\r' || buf[0] == '\n') {
            return TERM_KEY_ENTER;
        }
        if (buf[0] == 127 || buf[0] == '\b') {
            return TERM_KEY_BACKSPACE;
        }
        return buf[0];
    }

    if (len < 2) {
        return TERM_KEY_NONE;
    }

    // ESC O x: cursor keys in application mode.
    if (buf[1] == 'O') {
        if (len < 3) {
            return TERM_KEY_NONE;
        }
        *used = 3;
        return csi_key(0, buf[2]);
    }

    if (buf[1] != '[') {
        *used = 1;
        return TERM_KEY_ESC;
    }

    // ESC [ params intermediates final: only the first parameter matters here.
    int i = 2;
    int param = 0;
    int in_first = 1;
    while (i < len && buf[i] >= 0x30 && buf[i] <= 0x3f) {
        if (buf[i] >= '0' && buf[i] <= '9' && in_first) {
            param = param * 10 + (buf[i] - '0');
        } else {
            in_first = 0;
        }
        i++;
    }
    while (i < len && buf[i] >= 0x20 && buf[i] <= 0x2f) {
        i++;
    }

    if (i >= len) {
        if (len >= CSI_MAX) {
            *used = len;
        }
        return TERM_KEY_NONE;
    }
    if (buf[i] < 0x40 || buf[i] > 0x7e) {
        *used = 1;
        return TERM_KEY_ESC;
    }

    *used = i + 1;
    return csi_key(param, buf[i]);
}

int term_key(int timeout_ms)
{
    unsigned long start = term_ms();

    for (;;) {
        // After Ctrl-Z and fg the screen is blank; a caller waiting forever would never redraw it.
        if (repaint && timeout_ms < 0) {
            return TERM_KEY_NONE;
        }

        while (klen > 0) {
            int used;
            int key = term_decode(kbuf, klen, &used);
            if (used == 0) {
                break;
            }
            memmove(kbuf, kbuf + used, (size_t)(klen - used));
            klen -= used;
            esc_since = 0;
            if (key != TERM_KEY_NONE) {
                return key;
            }
        }

        // An ESC nothing followed in time was the Esc key itself.
        if (klen > 0) {
            if (esc_since == 0) {
                esc_since = term_ms();
            } else if (term_ms() - esc_since >= ESC_WAIT_MS) {
                memmove(kbuf, kbuf + 1, (size_t)(klen - 1));
                klen--;
                esc_since = 0;
                return TERM_KEY_ESC;
            }
        }

        int n = read(0, kbuf + klen, (size_t)(CSI_MAX - klen));
        if (n > 0) {
            klen += n;
            continue;
        }
        if (n == 0) {
            // Nothing more will come, so a pending ESC stands alone.
            if (klen > 0) {
                memmove(kbuf, kbuf + 1, (size_t)(klen - 1));
                klen--;
                esc_since = 0;
                return TERM_KEY_ESC;
            }
            return TERM_KEY_EOF;
        }

        if (timeout_ms >= 0 && term_ms() - start >= (unsigned long)timeout_ms) {
            return TERM_KEY_NONE;
        }
        usleep(2000);
    }
}

void term_clear(unsigned attr)
{
    blank(back, attr);
}

void term_put(int row, int col, uint32_t codepoint, unsigned attr)
{
    if (row < 0 || row >= TERM_ROWS || col < 0 || col >= TERM_COLS) {
        return;
    }
    back[row][col].cp = codepoint;
    back[row][col].attr = (uint16_t)attr;
}

int term_print(int row, int col, const char *utf8, unsigned attr)
{
    const unsigned char *s = (const unsigned char *)utf8;
    int start = col;

    while (*s && col < TERM_COLS) {
        uint32_t cp;
        int extra;
        if (*s < 0x80) {
            cp = *s;
            extra = 0;
        } else if ((*s & 0xe0) == 0xc0) {
            cp = *s & 0x1f;
            extra = 1;
        } else if ((*s & 0xf0) == 0xe0) {
            cp = *s & 0x0f;
            extra = 2;
        } else if ((*s & 0xf8) == 0xf0) {
            cp = *s & 0x07;
            extra = 3;
        } else {
            cp = '?';
            extra = 0;
        }
        s++;
        for (int k = 0; k < extra; k++) {
            if ((*s & 0xc0) != 0x80) {
                cp = '?';
                break;
            }
            cp = (cp << 6) | (*s & 0x3f);
            s++;
        }
        term_put(row, col++, cp, attr);
    }
    return col - start;
}

static void emit_move(int row, int col)
{
    emit("\033[", 2);
    emit_num(row + 1);
    emit(";", 1);
    emit_num(col + 1);
    emit("H", 1);
}

static int color_code(unsigned field)
{
    return field ? (int)field - 1 : TERM_DEFAULT;
}

static void emit_attr(unsigned attr)
{
    emit("\033[0;3", 5);
    emit_num(color_code(attr & 0xf));
    emit(";4", 2);
    emit_num(color_code((attr >> 4) & 0xf));
    if (attr & TERM_BOLD) {
        emit(";1", 2);
    }
    emit("m", 1);
}

void term_present(void)
{
    if (repaint) {
        emit("\033[0m\033[2J", 8);
        blank(front, TERM_NORMAL);
        cur_attr = TERM_NORMAL;
        cur_row = -1;
        repaint = 0;
    }

    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            // The last cell is left alone: writing it can scroll the whole screen.
            if (r == TERM_ROWS - 1 && c == TERM_COLS - 1) {
                continue;
            }
            struct cell *want = &back[r][c];
            struct cell *have = &front[r][c];
            if (want->cp == have->cp && want->attr == have->attr) {
                continue;
            }
            if (r != cur_row || c != cur_col) {
                emit_move(r, c);
            }
            if (want->attr != cur_attr) {
                emit_attr(want->attr);
                cur_attr = want->attr;
            }
            emit_utf8(want->cp);
            *have = *want;
            cur_row = r;
            cur_col = c + 1;
            if (cur_col == TERM_COLS) {
                cur_row = -1;
            }
        }
    }

    if (cursor_row >= 0 && cursor_row < TERM_ROWS && cursor_col >= 0 && cursor_col < TERM_COLS) {
        if (cursor_row != cur_row || cursor_col != cur_col) {
            emit_move(cursor_row, cursor_col);
            cur_row = cursor_row;
            cur_col = cursor_col;
        }
        if (!cursor_shown) {
            emit("\033[?25h", 6);
            cursor_shown = 1;
        }
    } else if (cursor_shown) {
        emit("\033[?25l", 6);
        cursor_shown = 0;
    }
    flush();
}

void term_redraw(void)
{
    repaint = 1;
}

void term_cursor(int row, int col)
{
    cursor_row = row;
    cursor_col = col;
}

static int row_end(int r)
{
    int end = TERM_COLS;
    while (end > 0 && (back[r][end - 1].cp == ' ' || back[r][end - 1].cp == 0)) {
        end--;
    }
    return end;
}

void term_dump(void)
{
    int rows = TERM_ROWS;
    while (rows > 0 && row_end(rows - 1) == 0) {
        rows--;
    }
    for (int r = 0; r < rows; r++) {
        int end = row_end(r);
        for (int c = 0; c < end; c++) {
            emit_utf8(back[r][c].cp ? back[r][c].cp : ' ');
        }
        emit("\n", 1);
    }
    flush();
}
