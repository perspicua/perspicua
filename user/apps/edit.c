/*
 * edit.c - A small text editor: move, insert and delete, search, save.
 *
 * The file is held as an array of lines, each its own growable buffer, and
 * long lines scroll sideways rather than wrap. Ctrl-S saves, Ctrl-Q quits,
 * Ctrl-F searches and Ctrl-G goes to a line. A save writes FILE.tmp and
 * renames it over FILE, so a crash leaves either the old file or the new one.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "errno.h"
#include "fcntl.h"
#include "signal.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "term.h"
#include "unistd.h"

#define TEXT_ROWS   (TERM_ROWS - 2)
#define STATUS_ROW  (TERM_ROWS - 2)
#define MESSAGE_ROW (TERM_ROWS - 1)
#define TAB_WIDTH   8
#define PATH_LEN    256
#define INPUT_LEN   128
#define WRITE_BUF   4096

#define CTRL(c) ((c) & 0x1f)

#define STATUS_ATTR (TERM_FG(TERM_BLACK) | TERM_BG(TERM_WHITE))
#define MATCH_ATTR  (TERM_FG(TERM_BLACK) | TERM_BG(TERM_YELLOW))
#define TILDE_ATTR  TERM_FG(TERM_BLUE)

#define HELP "Ctrl-S save   Ctrl-Q quit   Ctrl-F find   Ctrl-G go to line"

struct line {
    char *s;
    size_t len, cap;
};

static struct line *lines;
static int nlines, lines_cap;

static int cy;            // cursor line
static size_t cx;         // cursor byte within the line
static int goal_col = -1; // the column up and down aim for, kept across shorter lines
static int row_off, col_off;

static char filename[PATH_LEN];
static int dirty;
static char message[TERM_COLS + 1];
static int prompt_col = -1; // the cursor's column on the message row while a prompt is open

static const char *highlight; // the search text, while a search is open
static char search_label[32];
static int search_y;
static size_t search_x;

static void set_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
}

static int reserve(struct line *l, size_t need)
{
    if (need <= l->cap) {
        return 0;
    }
    size_t cap = l->cap ? l->cap : 16;
    while (cap < need) {
        cap *= 2;
    }
    char *bigger = realloc(l->s, cap);
    if (!bigger) {
        return -1;
    }
    l->s = bigger;
    l->cap = cap;
    return 0;
}

static int line_insert(struct line *l, size_t at, const char *s, size_t n)
{
    if (n == 0) {
        return 0;
    }
    if (reserve(l, l->len + n) < 0) {
        return -1;
    }
    memmove(l->s + at + n, l->s + at, l->len - at);
    memcpy(l->s + at, s, n);
    l->len += n;
    return 0;
}

static void line_erase(struct line *l, size_t at, size_t n)
{
    memmove(l->s + at, l->s + at + n, l->len - at - n);
    l->len -= n;
}

static int insert_line(int at, const char *s, size_t n)
{
    if (nlines == lines_cap) {
        int cap = lines_cap ? lines_cap * 2 : 64;
        struct line *bigger = realloc(lines, (size_t)cap * sizeof(*lines));
        if (!bigger) {
            return -1;
        }
        lines = bigger;
        lines_cap = cap;
    }
    struct line l = {0};
    if (line_insert(&l, 0, s, n) < 0) {
        return -1;
    }
    memmove(&lines[at + 1], &lines[at], (size_t)(nlines - at) * sizeof(*lines));
    lines[at] = l;
    nlines++;
    return 0;
}

static void delete_line(int at)
{
    free(lines[at].s);
    memmove(&lines[at], &lines[at + 1], (size_t)(nlines - at - 1) * sizeof(*lines));
    nlines--;
}

static int is_continuation(char c)
{
    return ((unsigned char)c & 0xc0) == 0x80;
}

static size_t prev_char(const struct line *l, size_t x)
{
    while (x > 0) {
        x--;
        if (!is_continuation(l->s[x])) {
            break;
        }
    }
    return x;
}

static size_t next_char(const struct line *l, size_t x)
{
    if (x >= l->len) {
        return l->len;
    }
    do {
        x++;
    } while (x < l->len && is_continuation(l->s[x]));
    return x;
}

static int char_width(char c, int col)
{
    return c == '\t' ? TAB_WIDTH - col % TAB_WIDTH : 1;
}

// The column byte x of l is drawn at, before scrolling.
static int column_of(const struct line *l, size_t x)
{
    int col = 0;
    for (size_t i = 0; i < x && i < l->len; i++) {
        if (!is_continuation(l->s[i])) {
            col += char_width(l->s[i], col);
        }
    }
    return col;
}

// The last character of l that starts at or before column want.
static size_t byte_at_column(const struct line *l, int want)
{
    int col = 0;
    size_t x = 0;
    while (x < l->len) {
        int width = char_width(l->s[x], col);
        if (col + width > want) {
            break;
        }
        col += width;
        x = next_char(l, x);
    }
    return x;
}

// Returns 1 if the file was read, 0 if it does not exist yet, -1 on an error left in errno.
static int load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    int n = 0;
    while (buf) {
        if (len == cap) {
            char *bigger = realloc(buf, cap * 2);
            if (!bigger) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = bigger;
            cap *= 2;
        }
        n = read(fd, buf + len, cap - len);
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
    }
    close(fd);
    if (!buf) {
        errno = ENOMEM;
        return -1;
    }
    if (n < 0) {
        free(buf);
        return -1;
    }

    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        // A final newline ends the last line rather than starting another.
        if ((i == len && start < len) || (i < len && buf[i] == '\n')) {
            if (insert_line(nlines, buf + start, i - start) < 0) {
                free(buf);
                errno = ENOMEM;
                return -1;
            }
            start = i + 1;
        }
    }
    free(buf);
    return 1;
}

static char wbuf[WRITE_BUF];
static size_t wlen;

static int write_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        int w = write(fd, s, n);
        if (w <= 0) {
            return -1;
        }
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

static int put(int fd, const char *s, size_t n)
{
    while (n > 0) {
        if (wlen == sizeof(wbuf)) {
            if (write_all(fd, wbuf, wlen) < 0) {
                return -1;
            }
            wlen = 0;
        }
        size_t chunk = n < sizeof(wbuf) - wlen ? n : sizeof(wbuf) - wlen;
        memcpy(wbuf + wlen, s, chunk);
        wlen += chunk;
        s += chunk;
        n -= chunk;
    }
    return 0;
}

static int prompt(const char *label, char *buf, size_t size, void (*on_key)(const char *, int));

static void save(void)
{
    if (!filename[0]) {
        char name[PATH_LEN] = "";
        if (prompt("save as: ", name, sizeof(name), NULL) != 0 || !name[0]) {
            set_message("not saved");
            return;
        }
        strcpy(filename, name);
    }

    char tmp[PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", filename);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        set_message("cannot write %s: %s", tmp, strerror(errno));
        return;
    }

    unsigned long bytes = 0;
    int ok = 1;
    wlen = 0;
    // An empty buffer is an empty file, not one blank line.
    if (nlines > 1 || lines[0].len > 0) {
        for (int i = 0; i < nlines && ok; i++) {
            ok = put(fd, lines[i].s, lines[i].len) == 0 && put(fd, "\n", 1) == 0;
            bytes += lines[i].len + 1;
        }
    }
    ok = ok && write_all(fd, wbuf, wlen) == 0 && fsync(fd) == 0;
    int err = errno;
    close(fd);
    if (ok && rename(tmp, filename) != 0) {
        ok = 0;
        err = errno;
    }
    if (!ok) {
        unlink(tmp);
        set_message("save failed: %s", strerror(err));
        return;
    }
    dirty = 0;
    set_message("wrote %d lines, %lu bytes to %s", nlines, bytes, filename);
}

static void scroll(void)
{
    if (cy < row_off) {
        row_off = cy;
    }
    if (cy >= row_off + TEXT_ROWS) {
        row_off = cy - TEXT_ROWS + 1;
    }
    int col = column_of(&lines[cy], cx);
    if (col < col_off) {
        col_off = col;
    }
    if (col >= col_off + TERM_COLS) {
        col_off = col - TERM_COLS + 1;
    }
}

static void put_cell(int row, int col, uint32_t cp, unsigned attr)
{
    if (col >= col_off) {
        term_put(row, col - col_off, cp, attr);
    }
}

static void draw_line(int row, const struct line *l)
{
    size_t n = highlight ? strlen(highlight) : 0;
    size_t lit_until = 0;
    int col = 0;

    for (size_t p = 0; p < l->len && col < col_off + TERM_COLS;) {
        if (n && p + n <= l->len && memcmp(l->s + p, highlight, n) == 0) {
            lit_until = p + n;
        }
        unsigned attr = p < lit_until ? MATCH_ATTR : TERM_NORMAL;
        unsigned char c = (unsigned char)l->s[p];

        if (c == '\t') {
            int width = char_width('\t', col);
            for (int i = 0; i < width; i++) {
                put_cell(row, col++, ' ', attr);
            }
            p++;
            continue;
        }

        uint32_t cp = c;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra) {
            cp = c & (0x3f >> extra);
        }
        p++;
        for (int k = 0; k < extra && p < l->len && is_continuation(l->s[p]); k++) {
            cp = (cp << 6) | ((unsigned char)l->s[p++] & 0x3f);
        }
        put_cell(row, col++, cp, attr);
    }
}

static void draw_status(void)
{
    char left[TERM_COLS + 1], right[48];
    for (int c = 0; c < TERM_COLS; c++) {
        term_put(STATUS_ROW, c, ' ', STATUS_ATTR);
    }
    snprintf(left, sizeof(left), " %s%s", filename[0] ? filename : "[no name]",
             dirty ? " [modified]" : "");
    snprintf(right, sizeof(right), "line %d/%d  col %d ", cy + 1, nlines,
             column_of(&lines[cy], cx) + 1);
    term_print(STATUS_ROW, 0, left, STATUS_ATTR);
    term_print(STATUS_ROW, TERM_COLS - (int)strlen(right), right, STATUS_ATTR);
}

static void refresh(void)
{
    scroll();
    term_clear(TERM_NORMAL);
    for (int r = 0; r < TEXT_ROWS; r++) {
        if (row_off + r < nlines) {
            draw_line(r, &lines[row_off + r]);
        } else {
            term_put(r, 0, '~', TILDE_ATTR);
        }
    }
    draw_status();
    term_print(MESSAGE_ROW, 0, message, TERM_NORMAL);
    if (prompt_col >= 0) {
        term_cursor(MESSAGE_ROW, prompt_col);
    } else {
        term_cursor(cy - row_off, column_of(&lines[cy], cx) - col_off);
    }
    term_present();
}

// Reads a line on the message row, showing each keystroke to on_key; 0 on Enter, -1 on Esc.
static int prompt(const char *label, char *buf, size_t size, void (*on_key)(const char *, int))
{
    size_t len = strlen(buf);
    for (;;) {
        set_message("%s%s", label, buf);
        prompt_col = (int)strlen(message);
        refresh();

        int key = term_key(-1);
        if (key == TERM_KEY_NONE) {
            continue;
        }
        int result = 1;
        if (key == TERM_KEY_ENTER) {
            result = 0;
        } else if (key == TERM_KEY_ESC || key == TERM_KEY_EOF) {
            result = -1;
        } else if (key == TERM_KEY_BACKSPACE) {
            if (len) {
                buf[--len] = '\0';
            }
        } else if (key >= ' ' && key < 0x7f && len < size - 1) {
            buf[len++] = (char)key;
            buf[len] = '\0';
        }
        if (on_key) {
            on_key(buf, key);
        }
        if (result != 1) {
            prompt_col = -1;
            message[0] = '\0';
            return result;
        }
    }
}

static int match_at(const struct line *l, size_t p, const char *q, size_t n)
{
    return p + n <= l->len && memcmp(l->s + p, q, n) == 0;
}

// Moves to q at or after (y, x) going forward, or before (y, x) going back, wrapping round once.
static int find(const char *q, int y, size_t x, int dir)
{
    size_t n = strlen(q);
    if (n == 0) {
        return 0;
    }
    for (int i = 0; i <= nlines; i++) {
        int ly = ((y + dir * i) % nlines + nlines) % nlines;
        const struct line *l = &lines[ly];
        if (l->len < n) {
            continue;
        }
        size_t last = l->len - n;
        // The first pass starts at x; the wrapped last pass on line y covers the rest of it.
        size_t lo = 0, hi = last + 1;
        if (dir > 0) {
            lo = i == 0 ? x : 0;
            hi = i == nlines ? (x < hi ? x : hi) : hi;
        } else {
            hi = i == 0 ? (x < hi ? x : hi) : hi;
            lo = i == nlines ? x + 1 : 0;
        }
        for (size_t k = 0; lo + k < hi; k++) {
            size_t p = dir > 0 ? lo + k : hi - 1 - k;
            if (match_at(l, p, q, n)) {
                cy = ly;
                cx = p;
                return 1;
            }
        }
    }
    return 0;
}

static void on_search_key(const char *typed, int key)
{
    if (key == TERM_KEY_ENTER || key == TERM_KEY_ESC || key == TERM_KEY_EOF) {
        return;
    }
    int found;
    if (key == TERM_KEY_DOWN) {
        found = find(typed, cy, cx + 1, 1);
    } else if (key == TERM_KEY_UP) {
        found = find(typed, cy, cx, -1);
    } else {
        cy = search_y;
        cx = search_x;
        found = find(typed, cy, cx, 1);
    }
    strcpy(search_label, found || !typed[0] ? "find: " : "find (no match): ");
}

static void find_interactive(void)
{
    int y = cy, ro = row_off, co = col_off;
    size_t x = cx;
    char q[INPUT_LEN] = "";

    search_y = y;
    search_x = x;
    highlight = q;
    strcpy(search_label, "find: ");
    if (prompt(search_label, q, sizeof(q), on_search_key) != 0) {
        cy = y;
        cx = x;
        row_off = ro;
        col_off = co;
    }
    highlight = NULL;
}

static void go_to_line(void)
{
    char num[16] = "";
    if (prompt("go to line: ", num, sizeof(num), NULL) != 0 || !num[0]) {
        return;
    }
    int n = atoi(num);
    cy = n < 1 ? 0 : n > nlines ? nlines - 1 : n - 1;
    cx = 0;
}

static void move_vertical(int by)
{
    if (goal_col < 0) {
        goal_col = column_of(&lines[cy], cx);
    }
    cy += by;
    if (cy < 0) {
        cy = 0;
    }
    if (cy >= nlines) {
        cy = nlines - 1;
    }
    cx = byte_at_column(&lines[cy], goal_col);
}

static void page(int by)
{
    int max_off = nlines > TEXT_ROWS ? nlines - TEXT_ROWS : 0;
    row_off += by;
    row_off = row_off < 0 ? 0 : row_off > max_off ? max_off : row_off;
    move_vertical(by);
}

static void move_left(void)
{
    if (cx > 0) {
        cx = prev_char(&lines[cy], cx);
    } else if (cy > 0) {
        cy--;
        cx = lines[cy].len;
    }
}

static void move_right(void)
{
    if (cx < lines[cy].len) {
        cx = next_char(&lines[cy], cx);
    } else if (cy + 1 < nlines) {
        cy++;
        cx = 0;
    }
}

static void insert_byte(char c)
{
    if (line_insert(&lines[cy], cx, &c, 1) < 0) {
        set_message("out of memory");
        return;
    }
    cx++;
    dirty = 1;
}

static void split_line(void)
{
    if (insert_line(cy + 1, lines[cy].s + cx, lines[cy].len - cx) < 0) {
        set_message("out of memory");
        return;
    }
    lines[cy].len = cx;
    cy++;
    cx = 0;
    dirty = 1;
}

// Appends line at + 1 to line at and removes it.
static int join_next(int at)
{
    if (line_insert(&lines[at], lines[at].len, lines[at + 1].s, lines[at + 1].len) < 0) {
        set_message("out of memory");
        return -1;
    }
    delete_line(at + 1);
    dirty = 1;
    return 0;
}

static void backspace(void)
{
    if (cx > 0) {
        size_t start = prev_char(&lines[cy], cx);
        line_erase(&lines[cy], start, cx - start);
        cx = start;
        dirty = 1;
    } else if (cy > 0) {
        size_t join = lines[cy - 1].len;
        if (join_next(cy - 1) == 0) {
            cy--;
            cx = join;
        }
    }
}

static void delete_forward(void)
{
    struct line *l = &lines[cy];
    if (cx < l->len) {
        line_erase(l, cx, next_char(l, cx) - cx);
        dirty = 1;
    } else if (cy + 1 < nlines) {
        join_next(cy);
    }
}

// Returns 1 when the key ends the session.
static int handle_key(int key)
{
    static int quit_armed;
    int armed = 0, vertical = 0;

    switch (key) {
        case CTRL('q'):
            if (dirty && !quit_armed) {
                set_message("unsaved changes: Ctrl-Q again discards them, Ctrl-S saves");
                armed = 1;
                break;
            }
            return 1;
        case TERM_KEY_EOF:
            return 1;
        case CTRL('s'):
            save();
            break;
        case CTRL('f'):
            find_interactive();
            break;
        case CTRL('g'):
            go_to_line();
            break;
        case CTRL('c'):
            set_message("Ctrl-Q quits");
            break;
        case TERM_KEY_UP:
            move_vertical(-1);
            vertical = 1;
            break;
        case TERM_KEY_DOWN:
            move_vertical(1);
            vertical = 1;
            break;
        case TERM_KEY_PGUP:
            page(-TEXT_ROWS);
            vertical = 1;
            break;
        case TERM_KEY_PGDN:
            page(TEXT_ROWS);
            vertical = 1;
            break;
        case TERM_KEY_LEFT:
            move_left();
            break;
        case TERM_KEY_RIGHT:
            move_right();
            break;
        case TERM_KEY_HOME:
            cx = 0;
            break;
        case TERM_KEY_END:
            cx = lines[cy].len;
            break;
        case TERM_KEY_ENTER:
            split_line();
            break;
        case TERM_KEY_BACKSPACE:
            backspace();
            break;
        case TERM_KEY_DELETE:
            delete_forward();
            break;
        default:
            // Bytes of 0x80 and up are UTF-8, typed one byte at a time.
            if (key == '\t' || (key >= ' ' && key < 0x100 && key != 0x7f)) {
                insert_byte((char)key);
            }
            break;
    }
    quit_armed = armed;
    if (!vertical) {
        goal_col = -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "usage: edit [FILE]\n");
        return 2;
    }

    int loaded = 1;
    if (argc == 2) {
        if (strlen(argv[1]) >= sizeof(filename)) {
            fprintf(stderr, "edit: file name too long\n");
            return 1;
        }
        strcpy(filename, argv[1]);
        loaded = load(filename);
        if (loaded < 0) {
            fprintf(stderr, "edit: cannot read %s: %s\n", filename, strerror(errno));
            return 1;
        }
    }
    if (nlines == 0 && insert_line(0, "", 0) < 0) {
        fprintf(stderr, "edit: out of memory\n");
        return 1;
    }

    if (term_open() != 0) {
        return 1;
    }
    // A stray Ctrl-C must not throw away unsaved work.
    signal(SIGINT, SIG_IGN);
    set_message("%s" HELP, loaded ? "" : "new file   ");

    for (;;) {
        refresh();
        int key = term_key(-1);
        if (key == TERM_KEY_NONE) {
            continue;
        }
        message[0] = '\0';
        if (handle_key(key)) {
            break;
        }
    }
    term_close();
    return 0;
}
