/*
 * mines.c - Minesweeper: open every safe cell without touching a mine.
 *
 * `mines 1|2|3` picks the board size; `mines --selftest` checks the rules
 * without touching the terminal.
 */

#include <stddef.h>
#include <stdint.h>

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "term.h"
#include "unistd.h"

#define MAX_W     30
#define MAX_H     16
#define FIELD_ROW 2

struct level {
    int w, h, mines;
};

static const struct level levels[] = {{9, 9, 10}, {16, 16, 40}, {30, 16, 99}};

struct game {
    int w, h, mines;
    uint8_t mine[MAX_H][MAX_W];
    uint8_t open[MAX_H][MAX_W];
    uint8_t flag[MAX_H][MAX_W];
    uint8_t count[MAX_H][MAX_W];
    int cx, cy;
    int placed; // mines are laid at the first open, around it
    int opened;
    int flags;
    int lost, won;
    unsigned long start_ms, end_ms;
};

static int in_bounds(const struct game *g, int x, int y)
{
    return x >= 0 && x < g->w && y >= 0 && y < g->h;
}

static void count_neighbours(struct game *g)
{
    for (int y = 0; y < g->h; y++) {
        for (int x = 0; x < g->w; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if ((dx || dy) && in_bounds(g, x + dx, y + dy)) {
                        n += g->mine[y + dy][x + dx];
                    }
                }
            }
            g->count[y][x] = (uint8_t)n;
        }
    }
}

// The 3x3 around the first open stays clear, so it always opens an area.
static void place_mines(struct game *g, int safe_x, int safe_y)
{
    int laid = 0;
    while (laid < g->mines) {
        int x = rand() % g->w;
        int y = rand() % g->h;
        int near = x >= safe_x - 1 && x <= safe_x + 1 && y >= safe_y - 1 && y <= safe_y + 1;
        if (near || g->mine[y][x]) {
            continue;
        }
        g->mine[y][x] = 1;
        laid++;
    }
    count_neighbours(g);
    g->placed = 1;
}

static void lose(struct game *g)
{
    g->lost = 1;
    g->end_ms = term_ms();
}

// Opens a cell; a zero opens its whole connected area and the numbers that border it.
static void open_cell(struct game *g, int x, int y)
{
    if (!in_bounds(g, x, y) || g->open[y][x] || g->flag[y][x] || g->lost || g->won) {
        return;
    }
    if (!g->placed) {
        place_mines(g, x, y);
        g->start_ms = term_ms();
    }
    if (g->mine[y][x]) {
        g->open[y][x] = 1;
        lose(g);
        return;
    }

    // Opened as pushed, so each cell enters the stack at most once.
    static int stack[MAX_W * MAX_H];
    int top = 0;
    g->open[y][x] = 1;
    g->opened++;
    if (!g->count[y][x]) {
        stack[top++] = y * MAX_W + x;
    }
    while (top > 0) {
        int at = stack[--top];
        int cy = at / MAX_W;
        int cx = at % MAX_W;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = cx + dx;
                int ny = cy + dy;
                if (!in_bounds(g, nx, ny) || g->open[ny][nx] || g->flag[ny][nx]) {
                    continue;
                }
                g->open[ny][nx] = 1;
                g->opened++;
                if (!g->count[ny][nx]) {
                    stack[top++] = ny * MAX_W + nx;
                }
            }
        }
    }

    if (g->opened == g->w * g->h - g->mines) {
        g->won = 1;
        g->end_ms = term_ms();
    }
}

static void toggle_flag(struct game *g, int x, int y)
{
    if (g->open[y][x] || g->lost || g->won) {
        return;
    }
    g->flag[y][x] = !g->flag[y][x];
    g->flags += g->flag[y][x] ? 1 : -1;
}

// On an open number with that many flags around it, opens the rest of its neighbours.
static void chord(struct game *g, int x, int y)
{
    if (!g->open[y][x] || !g->count[y][x]) {
        return;
    }
    int flagged = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (in_bounds(g, x + dx, y + dy)) {
                flagged += g->flag[y + dy][x + dx];
            }
        }
    }
    if (flagged != g->count[y][x]) {
        return;
    }
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            open_cell(g, x + dx, y + dy);
        }
    }
}

static void game_init(struct game *g, const struct level *lv)
{
    memset(g, 0, sizeof(*g));
    g->w = lv->w;
    g->h = lv->h;
    g->mines = lv->mines;
    g->cx = g->w / 2;
    g->cy = g->h / 2;
}

// Drawing

static int field_col(const struct game *g)
{
    return (TERM_COLS - 2 * g->w) / 2;
}

static const int number_colors[9] = {0,           TERM_BLUE, TERM_GREEN, TERM_RED,  TERM_MAGENTA,
                                     TERM_YELLOW, TERM_CYAN, TERM_WHITE, TERM_WHITE};

static void draw_cell(const struct game *g, int x, int y)
{
    int row = FIELD_ROW + y;
    int col = field_col(g) + 2 * x;
    int cursor = x == g->cx && y == g->cy && !g->won && !g->lost;
    unsigned attr;
    const char *text;
    char num[3];

    if (g->open[y][x] && g->mine[y][x]) {
        text = " *";
        attr = TERM_FG(TERM_WHITE) | TERM_BG(TERM_RED) | TERM_BOLD;
    } else if (g->lost && g->flag[y][x] && !g->mine[y][x]) {
        text = " x";
        attr = TERM_FG(TERM_RED) | TERM_BOLD;
    } else if (g->lost && g->mine[y][x] && !g->flag[y][x]) {
        text = " *";
        attr = TERM_BOLD;
    } else if (g->flag[y][x]) {
        text = " F";
        attr = TERM_FG(TERM_RED) | TERM_BG(TERM_WHITE) | TERM_BOLD;
    } else if (!g->open[y][x]) {
        text = "  ";
        attr = TERM_BG(TERM_WHITE);
    } else if (g->count[y][x]) {
        snprintf(num, sizeof(num), " %d", g->count[y][x]);
        text = num;
        attr = TERM_FG(number_colors[g->count[y][x]]) | TERM_BOLD;
    } else {
        text = "  ";
        attr = TERM_NORMAL;
    }

    if (cursor) {
        attr = TERM_FG(TERM_BLACK) | TERM_BG(TERM_YELLOW) | TERM_BOLD;
        if (!g->open[y][x] && !g->flag[y][x]) {
            text = "[]";
        }
    }
    term_print(row, col, text, attr);
}

static void draw(const struct game *g, int level)
{
    char line[80];
    unsigned long now = g->won || g->lost ? g->end_ms : term_ms();
    unsigned long secs = g->placed ? (now - g->start_ms) / 1000 : 0;

    term_clear(TERM_NORMAL);
    snprintf(line, sizeof(line), "M I N E S   level %d   mines left %d   time %lus", level,
             g->mines - g->flags, secs);
    term_print(0, field_col(g) - 1, line, TERM_BOLD);

    for (int y = 0; y < g->h; y++) {
        for (int x = 0; x < g->w; x++) {
            draw_cell(g, x, y);
        }
    }

    int below = FIELD_ROW + g->h + 1;
    if (g->won) {
        term_print(below, field_col(g), " cleared!  r again  q quit ",
                   TERM_FG(TERM_BLACK) | TERM_BG(TERM_GREEN));
    } else if (g->lost) {
        term_print(below, field_col(g), " boom.  r again  q quit ",
                   TERM_FG(TERM_WHITE) | TERM_BG(TERM_RED));
    } else {
        term_print(below, field_col(g) - 1, "arrows/wasd move  space open  f flag  q quit",
                   TERM_NORMAL);
    }
}

// Play

static void play(struct game *g, int level)
{
    for (;;) {
        draw(g, level);
        term_present();

        // Waking each half second keeps the clock moving while nothing is pressed.
        int key = term_key(500);
        if (key == TERM_KEY_NONE) {
            continue;
        }
        if (key == 'q' || key == 'Q' || key == TERM_KEY_EOF) {
            return;
        }
        if (key == 'r' || key == 'R') {
            game_init(g, &levels[level - 1]);
            continue;
        }
        if (g->lost || g->won) {
            continue;
        }

        switch (key) {
            case TERM_KEY_LEFT:
            case 'a':
            case 'A':
                g->cx = g->cx > 0 ? g->cx - 1 : g->cx;
                break;
            case TERM_KEY_RIGHT:
            case 'd':
            case 'D':
                g->cx = g->cx < g->w - 1 ? g->cx + 1 : g->cx;
                break;
            case TERM_KEY_UP:
            case 'w':
            case 'W':
                g->cy = g->cy > 0 ? g->cy - 1 : g->cy;
                break;
            case TERM_KEY_DOWN:
            case 's':
            case 'S':
                g->cy = g->cy < g->h - 1 ? g->cy + 1 : g->cy;
                break;
            case ' ':
            case TERM_KEY_ENTER:
                if (g->open[g->cy][g->cx]) {
                    chord(g, g->cx, g->cy);
                } else {
                    open_cell(g, g->cx, g->cy);
                }
                break;
            case 'f':
            case 'F':
                toggle_flag(g, g->cx, g->cy);
                break;
        }
    }
}

// Self-test

static int st_failed;

#define ST_CHECK(cond)                                       \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("  FAIL line %d: %s\n", __LINE__, #cond); \
            st_failed++;                                     \
        }                                                    \
    } while (0)

// Lays out a fixed board from rows of '*' (mine) and '.' (safe).
static void layout(struct game *g, const char *rows[], int h)
{
    struct level lv = {(int)strlen(rows[0]), h, 0};
    game_init(g, &lv);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < g->w; x++) {
            g->mine[y][x] = rows[y][x] == '*';
            g->mines += g->mine[y][x];
        }
    }
    count_neighbours(g);
    g->placed = 1;
}

static int selftest(void)
{
    static struct game g;

    // the first open is always safe and always opens an area, even on the densest board
    int safe = 1;
    for (int round = 0; round < 40; round++) {
        game_init(&g, &levels[2]);
        open_cell(&g, g.cx, g.cy);
        safe &= !g.lost && g.count[g.cy][g.cx] == 0;
        int laid = 0;
        for (int y = 0; y < g.h; y++) {
            for (int x = 0; x < g.w; x++) {
                laid += g.mine[y][x];
            }
        }
        safe &= laid == levels[2].mines;
    }
    ST_CHECK(safe);

    // a zero opens its connected area and the numbers that border it, nothing further
    static const char *corner[] = {
        "....*",
        "....*",
        "...**",
        "*....",
    };
    layout(&g, corner, 4);
    open_cell(&g, 0, 0);
    ST_CHECK(g.open[0][0] && g.open[1][2] && g.open[0][3]); // zeros and border numbers
    ST_CHECK(g.opened == 11);
    ST_CHECK(!g.open[3][1] && !g.open[3][2]); // never past a number
    ST_CHECK(!g.open[0][4] && !g.lost);

    // opening a mine loses
    layout(&g, corner, 4);
    open_cell(&g, 4, 0);
    ST_CHECK(g.lost);

    // a flag protects its cell
    layout(&g, corner, 4);
    toggle_flag(&g, 4, 0);
    open_cell(&g, 4, 0);
    ST_CHECK(!g.lost && g.flags == 1);
    toggle_flag(&g, 4, 0);
    ST_CHECK(g.flags == 0);

    // a number with its flags in place opens the rest; with a wrong flag, it hits the mine
    static const char *pair[] = {
        "*..",
        "...",
        "...",
    };
    layout(&g, pair, 3);
    g.open[1][1] = 1;
    g.opened = 1;
    toggle_flag(&g, 0, 0);
    chord(&g, 1, 1);
    ST_CHECK(!g.lost && g.open[0][1] && g.open[2][2]);

    layout(&g, pair, 3);
    g.open[1][1] = 1;
    g.opened = 1;
    toggle_flag(&g, 1, 0);
    chord(&g, 1, 1);
    ST_CHECK(g.lost);

    // opening every safe cell wins
    layout(&g, pair, 3);
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++) {
            if (!g.mine[y][x]) {
                open_cell(&g, x, y);
            }
        }
    }
    ST_CHECK(g.won && !g.lost);

    printf("mines selftest: %s (%d failed)\n", st_failed ? "FAIL" : "ok", st_failed);
    return st_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        return selftest();
    }

    int level = 2;
    if (argc > 1) {
        level = atoi(argv[1]);
        if (level < 1 || level > 3) {
            printf("usage: mines [1|2|3]   (9x9, 16x16 or 30x16)\n");
            return 1;
        }
    }

    srand((unsigned)(term_ms() ^ ((unsigned long)getpid() << 16)));

    static struct game g;
    game_init(&g, &levels[level - 1]);

    term_open();
    play(&g, level);
    term_close();

    if (g.won) {
        printf("mines: cleared level %d in %lus\n", level, (g.end_ms - g.start_ms) / 1000);
    }
    return 0;
}
