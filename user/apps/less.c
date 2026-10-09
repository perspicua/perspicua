/*
 * less - page through a file or piped text.
 *
 * Long lines wrap. Keys: arrows/j/k scroll, space/b or PgDn/PgUp page,
 * g/G top and bottom, / searches, n/N next and previous match, q quits.
 */

#include <stddef.h>
#include <stdint.h>

#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/stat.h"
#include "term.h"
#include "unistd.h"

#define VIEW_ROWS  (TERM_ROWS - 1)
#define SEARCH_MAX 64
#define TAB_WIDTH  8

#define STATUS_ATTR (TERM_FG(TERM_BLACK) | TERM_BG(TERM_WHITE))
#define MATCH_ATTR  (TERM_FG(TERM_BLACK) | TERM_BG(TERM_YELLOW))

// One screen row: a slice of the text and the logical line it belongs to.
struct row {
    size_t start, end;
    int line;
};

static char *text;
static size_t text_len;
static struct row *rows;
static int nrows, nlines;
static int top;
static char search[SEARCH_MAX];
static const char *message;

static int read_all(int fd)
{
    size_t cap = 4096;
    text = malloc(cap);
    if (!text) {
        return -1;
    }
    for (;;) {
        if (text_len == cap) {
            char *bigger = realloc(text, cap * 2);
            if (!bigger) {
                return -1;
            }
            text = bigger;
            cap *= 2;
        }
        int n = read(fd, text + text_len, cap - text_len);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            return 0;
        }
        text_len += (size_t)n;
    }
}

static int is_continuation(unsigned char c)
{
    return (c & 0xc0) == 0x80;
}

static int add_row(size_t start, size_t end, int line)
{
    static int cap;
    if (nrows == cap) {
        cap = cap ? cap * 2 : 256;
        struct row *bigger = realloc(rows, (size_t)cap * sizeof(*rows));
        if (!bigger) {
            return -1;
        }
        rows = bigger;
    }
    rows[nrows].start = start;
    rows[nrows].end = end;
    rows[nrows].line = line;
    nrows++;
    return 0;
}

// Cuts the text into screen rows: a newline ends one, and so does running out of columns.
static int build_rows(void)
{
    size_t start = 0;
    int col = 0;
    for (size_t i = 0; i < text_len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') {
            if (add_row(start, i, nlines) < 0) {
                return -1;
            }
            nlines++;
            start = i + 1;
            col = 0;
            continue;
        }
        if (is_continuation(c)) {
            continue;
        }
        int width = c == '\t' ? TAB_WIDTH - col % TAB_WIDTH : 1;
        if (col + width > TERM_COLS) {
            if (add_row(start, i, nlines) < 0) {
                return -1;
            }
            start = i;
            col = 0;
            width = c == '\t' ? TAB_WIDTH : 1;
        }
        col += width;
    }
    if (start < text_len) {
        if (add_row(start, text_len, nlines) < 0) {
            return -1;
        }
        nlines++;
    }
    return 0;
}

static int max_top(void)
{
    return nrows > VIEW_ROWS ? nrows - VIEW_ROWS : 0;
}

static void set_top(int t)
{
    top = t < 0 ? 0 : (t > max_top() ? max_top() : t);
}

static int row_has_match(int r)
{
    size_t len = strlen(search);
    if (!len) {
        return 0;
    }
    for (size_t p = rows[r].start; p + len <= rows[r].end; p++) {
        if (memcmp(text + p, search, len) == 0) {
            return 1;
        }
    }
    return 0;
}

static void find_next(int dir)
{
    if (!search[0]) {
        message = "no search yet: type / first";
        return;
    }
    for (int r = top + dir; r >= 0 && r < nrows; r += dir) {
        if (row_has_match(r)) {
            set_top(r);
            return;
        }
    }
    message = "pattern not found";
}

static void draw_row(int screen_row, const struct row *row)
{
    size_t len = strlen(search);
    size_t highlight_until = 0;
    int col = 0;

    for (size_t p = row->start; p < row->end && col < TERM_COLS;) {
        if (len && p + len <= row->end && memcmp(text + p, search, len) == 0) {
            highlight_until = p + len;
        }
        unsigned attr = p < highlight_until ? MATCH_ATTR : TERM_NORMAL;
        unsigned char c = (unsigned char)text[p];

        if (c == '\t') {
            do {
                term_put(screen_row, col++, ' ', attr);
            } while (col % TAB_WIDTH && col < TERM_COLS);
            p++;
            continue;
        }

        uint32_t cp = c;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra) {
            cp = c & (0x3f >> extra);
        }
        p++;
        for (int k = 0; k < extra && p < row->end && is_continuation((unsigned char)text[p]); k++) {
            cp = (cp << 6) | ((unsigned char)text[p++] & 0x3f);
        }
        term_put(screen_row, col++, cp, attr);
    }
}

static void draw_status(const char *name, const char *prompt)
{
    char line[TERM_COLS + 1];
    for (int c = 0; c < TERM_COLS; c++) {
        term_put(VIEW_ROWS, c, ' ', STATUS_ATTR);
    }

    if (prompt) {
        snprintf(line, sizeof(line), "/%s", prompt);
    } else if (message) {
        snprintf(line, sizeof(line), "%s", message);
    } else {
        int first = nrows ? rows[top].line + 1 : 0;
        int last_row = top + VIEW_ROWS - 1 < nrows ? top + VIEW_ROWS - 1 : nrows - 1;
        int last = nrows ? rows[last_row].line + 1 : 0;
        int pct = top >= max_top() ? 100 : (top + VIEW_ROWS) * 100 / nrows;
        snprintf(line, sizeof(line), "%s  lines %d-%d of %d  %d%%   q quit  / search  n next", name,
                 first, last, nlines, pct);
    }
    term_print(VIEW_ROWS, 0, line, STATUS_ATTR);
}

static void draw(const char *name, const char *prompt)
{
    term_clear(TERM_NORMAL);
    for (int r = 0; r < VIEW_ROWS; r++) {
        if (top + r < nrows) {
            draw_row(r, &rows[top + r]);
        } else {
            term_put(r, 0, '~', TERM_FG(TERM_BLUE));
        }
    }
    draw_status(name, prompt);
    term_present();
}

// Reads a search pattern on the status line; Enter keeps it, Esc cancels.
static int read_pattern(const char *name)
{
    char typed[SEARCH_MAX] = "";
    size_t len = 0;
    for (;;) {
        draw(name, typed);
        int key = term_key(-1);
        if (key == TERM_KEY_ENTER) {
            if (len) {
                memcpy(search, typed, len + 1);
            }
            return len > 0;
        }
        if (key == TERM_KEY_ESC) {
            return 0;
        }
        if (key == TERM_KEY_BACKSPACE) {
            if (len) {
                typed[--len] = '\0';
            }
        } else if (key >= ' ' && key < 0x7f && len < sizeof(typed) - 1) {
            typed[len++] = (char)key;
            typed[len] = '\0';
        }
    }
}

int main(int argc, char **argv)
{
    const char *name = "(stdin)";

    if (argc > 1) {
        name = argv[1];
        int fd = open(name, O_RDONLY);
        if (fd < 0 || read_all(fd) < 0) {
            fprintf(stderr, "less: cannot read %s\n", name);
            return 1;
        }
        close(fd);
    } else {
        struct stat st;
        if (fstat(0, &st) == 0 && S_ISCHR(st.st_mode)) {
            fprintf(stderr, "usage: less FILE, or: command | less\n");
            return 2;
        }
        if (read_all(0) < 0) {
            fprintf(stderr, "less: cannot read stdin\n");
            return 1;
        }
        // Piped text used up stdin, so keys come from the console instead.
        int fd = open("/dev/console", O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "less: cannot open /dev/console for keys\n");
            return 1;
        }
        dup2(fd, 0);
        close(fd);
    }

    if (build_rows() < 0) {
        fprintf(stderr, "less: out of memory\n");
        return 1;
    }

    term_open();
    for (;;) {
        draw(name, NULL);
        message = NULL;

        int key = term_key(-1);
        switch (key) {
            case 'q':
            case 'Q':
                term_close();
                return 0;
            case 'j':
            case TERM_KEY_DOWN:
            case TERM_KEY_ENTER:
                set_top(top + 1);
                break;
            case 'k':
            case TERM_KEY_UP:
                set_top(top - 1);
                break;
            case ' ':
            case 'f':
            case TERM_KEY_PGDN:
                set_top(top + VIEW_ROWS);
                break;
            case 'b':
            case TERM_KEY_PGUP:
                set_top(top - VIEW_ROWS);
                break;
            case 'g':
            case TERM_KEY_HOME:
                set_top(0);
                break;
            case 'G':
            case TERM_KEY_END:
                set_top(max_top());
                break;
            case '/':
                if (read_pattern(name)) {
                    find_next(1);
                }
                break;
            case 'n':
                find_next(1);
                break;
            case 'N':
                find_next(-1);
                break;
        }
    }
}
