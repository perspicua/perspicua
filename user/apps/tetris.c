/*
 * tetris.c - Falling-block puzzle on the terminal helper.
 *
 * `tetris --selftest` checks the game rules without touching the terminal.
 */

#include <stddef.h>
#include <stdint.h>

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "term.h"
#include "unistd.h"

#define BOARD_W 10
#define BOARD_H 20
#define PIECES  7

#define BOARD_ROW 2 // screen row of the board's top row
#define BOARD_COL 3 // screen column of its left cell; each cell is two columns wide
#define PANEL_COL 28

// 4x4 masks, bit 15 the top-left cell, one per clockwise rotation.
static const uint16_t shapes[PIECES][4] = {
    {0x0F00, 0x2222, 0x00F0, 0x4444}, // I
    {0x8E00, 0x6440, 0x0E20, 0x44C0}, // J
    {0x2E00, 0x4460, 0x0E80, 0xC440}, // L
    {0x6600, 0x6600, 0x6600, 0x6600}, // O
    {0x6C00, 0x4620, 0x06C0, 0x8C40}, // S
    {0x4E00, 0x4640, 0x0E40, 0x4C40}, // T
    {0xC600, 0x2640, 0x0C60, 0x4C80}, // Z
};

static const int colors[PIECES] = {TERM_CYAN,  TERM_BLUE,    TERM_WHITE, TERM_YELLOW,
                                   TERM_GREEN, TERM_MAGENTA, TERM_RED};

static const int line_score[5] = {0, 100, 300, 500, 800};

struct game {
    uint8_t board[BOARD_H][BOARD_W]; // 0 is empty, otherwise piece + 1
    int piece, rot, x, y;
    int next;
    int bag[PIECES];
    int bag_left;
    unsigned long score;
    int lines;
    int level;
    int over;
    int paused;
};

static int cell(uint16_t mask, int r, int c)
{
    return (mask >> (15 - (r * 4 + c))) & 1;
}

// Cells above the board are allowed, so a piece can enter from the top.
static int fits(const struct game *g, int piece, int rot, int x, int y)
{
    uint16_t mask = shapes[piece][rot];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!cell(mask, r, c)) {
                continue;
            }
            int bx = x + c;
            int by = y + r;
            if (bx < 0 || bx >= BOARD_W || by >= BOARD_H) {
                return 0;
            }
            if (by >= 0 && g->board[by][bx]) {
                return 0;
            }
        }
    }
    return 1;
}

// Deals the seven pieces in a fresh shuffled order every seven draws.
static int bag_draw(struct game *g)
{
    if (g->bag_left == 0) {
        for (int i = 0; i < PIECES; i++) {
            g->bag[i] = i;
        }
        for (int i = PIECES - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int t = g->bag[i];
            g->bag[i] = g->bag[j];
            g->bag[j] = t;
        }
        g->bag_left = PIECES;
    }
    return g->bag[--g->bag_left];
}

static void spawn(struct game *g)
{
    g->piece = g->next;
    g->next = bag_draw(g);
    g->rot = 0;
    g->x = 3;
    g->y = 0;
    if (!fits(g, g->piece, g->rot, g->x, g->y)) {
        g->over = 1;
    }
}

static int clear_lines(struct game *g)
{
    int cleared = 0;
    for (int r = BOARD_H - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < BOARD_W; c++) {
            if (!g->board[r][c]) {
                full = 0;
                break;
            }
        }
        if (!full) {
            continue;
        }
        memmove(g->board[1], g->board[0], (size_t)r * BOARD_W);
        memset(g->board[0], 0, BOARD_W);
        cleared++;
        r++; // the row above moved into this one
    }
    return cleared;
}

static void lock_piece(struct game *g)
{
    uint16_t mask = shapes[g->piece][g->rot];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!cell(mask, r, c)) {
                continue;
            }
            int by = g->y + r;
            if (by < 0) {
                g->over = 1;
                continue;
            }
            g->board[by][g->x + c] = (uint8_t)(g->piece + 1);
        }
    }

    int n = clear_lines(g);
    g->score += (unsigned long)(line_score[n] * g->level);
    g->lines += n;
    g->level = 1 + g->lines / 10;

    if (!g->over) {
        spawn(g);
    }
}

static int try_move(struct game *g, int dx, int dy)
{
    if (!fits(g, g->piece, g->rot, g->x + dx, g->y + dy)) {
        return 0;
    }
    g->x += dx;
    g->y += dy;
    return 1;
}

// Tries the turn in place, then nudged sideways, so a piece against a wall still turns.
static int try_rotate(struct game *g)
{
    static const int kicks[] = {0, -1, 1, -2, 2};
    int rot = (g->rot + 1) % 4;
    for (size_t k = 0; k < sizeof(kicks) / sizeof(kicks[0]); k++) {
        if (fits(g, g->piece, rot, g->x + kicks[k], g->y)) {
            g->rot = rot;
            g->x += kicks[k];
            return 1;
        }
    }
    return 0;
}

static int drop_distance(const struct game *g)
{
    int d = 0;
    while (fits(g, g->piece, g->rot, g->x, g->y + d + 1)) {
        d++;
    }
    return d;
}

static void step_down(struct game *g)
{
    if (!try_move(g, 0, 1)) {
        lock_piece(g);
    }
}

static void hard_drop(struct game *g)
{
    int d = drop_distance(g);
    g->y += d;
    g->score += (unsigned long)(2 * d);
    lock_piece(g);
}

static int fall_interval_ms(int level)
{
    int ms = 800 - 60 * (level - 1);
    return ms < 100 ? 100 : ms;
}

static void game_init(struct game *g)
{
    memset(g, 0, sizeof(*g));
    g->level = 1;
    g->next = bag_draw(g);
    spawn(g);
}

// Drawing

static void draw_cell(int r, int c, int piece)
{
    term_print(BOARD_ROW + r, BOARD_COL + 2 * c, "  ", TERM_BG(colors[piece]));
}

static void draw_frame(void)
{
    int top = BOARD_ROW - 1;
    int bottom = BOARD_ROW + BOARD_H;
    int left = BOARD_COL - 1;
    int right = BOARD_COL + 2 * BOARD_W;

    for (int col = left + 1; col < right; col++) {
        term_put(top, col, 0x2500, TERM_NORMAL);
        term_put(bottom, col, 0x2500, TERM_NORMAL);
    }
    for (int row = top + 1; row < bottom; row++) {
        term_put(row, left, 0x2502, TERM_NORMAL);
        term_put(row, right, 0x2502, TERM_NORMAL);
    }
    term_put(top, left, 0x250C, TERM_NORMAL);
    term_put(top, right, 0x2510, TERM_NORMAL);
    term_put(bottom, left, 0x2514, TERM_NORMAL);
    term_put(bottom, right, 0x2518, TERM_NORMAL);
}

static void draw_board(const struct game *g)
{
    for (int r = 0; r < BOARD_H; r++) {
        for (int c = 0; c < BOARD_W; c++) {
            if (g->board[r][c]) {
                draw_cell(r, c, g->board[r][c] - 1);
            } else {
                term_put(BOARD_ROW + r, BOARD_COL + 2 * c + 1, 0x00B7, TERM_NORMAL);
            }
        }
    }

    if (g->over) {
        return;
    }

    uint16_t mask = shapes[g->piece][g->rot];
    int ghost_y = g->y + drop_distance(g);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (cell(mask, r, c) && ghost_y + r >= 0) {
                term_print(BOARD_ROW + ghost_y + r, BOARD_COL + 2 * (g->x + c), "[]",
                           TERM_FG(colors[g->piece]));
            }
        }
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (cell(mask, r, c) && g->y + r >= 0) {
                draw_cell(g->y + r, g->x + c, g->piece);
            }
        }
    }
}

static void draw_panel(const struct game *g)
{
    char line[32];

    term_print(1, PANEL_COL, "T E T R I S", TERM_NORMAL | TERM_BOLD);

    term_print(3, PANEL_COL, "Next", TERM_NORMAL);
    uint16_t mask = shapes[g->next][0];
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 4; c++) {
            if (cell(mask, r, c)) {
                term_print(4 + r, PANEL_COL + 2 * c, "  ", TERM_BG(colors[g->next]));
            }
        }
    }

    snprintf(line, sizeof(line), "Score  %lu", g->score);
    term_print(8, PANEL_COL, line, TERM_NORMAL);
    snprintf(line, sizeof(line), "Lines  %d", g->lines);
    term_print(9, PANEL_COL, line, TERM_NORMAL);
    snprintf(line, sizeof(line), "Level  %d", g->level);
    term_print(10, PANEL_COL, line, TERM_NORMAL);

    static const char *help[] = {
        "← →  a d   move",       "↑     w     rotate", "↓     s     soft drop",
        "space       hard drop", "p           pause",  "q           quit",
    };
    for (size_t i = 0; i < sizeof(help) / sizeof(help[0]); i++) {
        term_print(13 + (int)i, PANEL_COL, help[i], TERM_NORMAL);
    }
}

static void draw_banner(int row, const char *text)
{
    int col = BOARD_COL + BOARD_W - (int)strlen(text) / 2;
    term_print(row, col, text, TERM_FG(TERM_BLACK) | TERM_BG(TERM_WHITE));
}

static void draw(const struct game *g)
{
    term_clear(TERM_NORMAL);
    draw_frame();
    draw_board(g);
    draw_panel(g);
    if (g->over) {
        draw_banner(BOARD_ROW + 8, " GAME OVER ");
        draw_banner(BOARD_ROW + 10, " r again  q quit ");
    } else if (g->paused) {
        draw_banner(BOARD_ROW + 9, " PAUSED ");
    }
}

// Play

static void play(struct game *g)
{
    unsigned long next_fall = term_ms() + (unsigned long)fall_interval_ms(g->level);

    for (;;) {
        draw(g);
        term_present();

        int wait = -1;
        if (!g->over && !g->paused) {
            long left = (long)(next_fall - term_ms());
            wait = left > 0 ? (int)left : 0;
        }

        int key = term_key(wait);
        if (key == TERM_KEY_NONE) {
            step_down(g);
            next_fall = term_ms() + (unsigned long)fall_interval_ms(g->level);
            continue;
        }
        if (key == 'q' || key == 'Q' || key == TERM_KEY_EOF) {
            return;
        }

        if (g->over) {
            if (key == 'r' || key == 'R') {
                game_init(g);
                next_fall = term_ms() + (unsigned long)fall_interval_ms(g->level);
            }
            continue;
        }
        if (g->paused) {
            if (key == 'p' || key == 'P') {
                g->paused = 0;
                next_fall = term_ms() + (unsigned long)fall_interval_ms(g->level);
            }
            continue;
        }

        switch (key) {
            case TERM_KEY_LEFT:
            case 'a':
            case 'A':
                try_move(g, -1, 0);
                break;
            case TERM_KEY_RIGHT:
            case 'd':
            case 'D':
                try_move(g, 1, 0);
                break;
            case TERM_KEY_UP:
            case 'w':
            case 'W':
                try_rotate(g);
                break;
            case TERM_KEY_DOWN:
            case 's':
            case 'S':
                if (try_move(g, 0, 1)) {
                    g->score++;
                } else {
                    lock_piece(g);
                }
                next_fall = term_ms() + (unsigned long)fall_interval_ms(g->level);
                break;
            case ' ':
                hard_drop(g);
                next_fall = term_ms() + (unsigned long)fall_interval_ms(g->level);
                break;
            case 'p':
            case 'P':
                g->paused = 1;
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

static void empty_game(struct game *g)
{
    memset(g, 0, sizeof(*g));
    g->level = 1;
}

static void fill_row(struct game *g, int r, int from_col)
{
    for (int c = from_col; c < BOARD_W; c++) {
        g->board[r][c] = 1;
    }
}

// Drops an upright I into column 0.
static void drop_upright_i(struct game *g)
{
    g->piece = 0;
    g->rot = 1;
    g->x = -2;
    g->y = 0;
    g->y += drop_distance(g);
    lock_piece(g);
}

static int selftest(void)
{
    struct game g;

    // walls and floor; cells above the board are allowed
    empty_game(&g);
    ST_CHECK(fits(&g, 0, 0, 6, 0));
    ST_CHECK(!fits(&g, 0, 0, 7, 0));
    ST_CHECK(fits(&g, 0, 1, -2, 0));
    ST_CHECK(!fits(&g, 0, 1, -3, 0));
    ST_CHECK(fits(&g, 0, 1, 0, BOARD_H - 4));
    ST_CHECK(!fits(&g, 0, 1, 0, BOARD_H - 3));
    ST_CHECK(fits(&g, 0, 1, 0, -2));

    // one line: scored, and the rest of the piece moves down with the board
    empty_game(&g);
    fill_row(&g, BOARD_H - 1, 1);
    drop_upright_i(&g);
    ST_CHECK(g.lines == 1);
    ST_CHECK(g.score == 100);
    ST_CHECK(g.board[BOARD_H - 1][0] && g.board[BOARD_H - 3][0] && !g.board[BOARD_H - 4][0]);
    ST_CHECK(!g.board[BOARD_H - 1][1]);

    // four lines score 800 times the level they were cleared on
    empty_game(&g);
    g.level = 2;
    for (int r = BOARD_H - 4; r < BOARD_H; r++) {
        fill_row(&g, r, 1);
    }
    drop_upright_i(&g);
    ST_CHECK(g.lines == 4);
    ST_CHECK(g.score == 1600);
    int empty = 1;
    for (int r = 0; r < BOARD_H; r++) {
        for (int c = 0; c < BOARD_W; c++) {
            empty &= !g.board[r][c];
        }
    }
    ST_CHECK(empty);

    // a cleared row pulls everything above it down
    empty_game(&g);
    fill_row(&g, BOARD_H - 2, 0);
    g.board[BOARD_H - 3][4] = 1;
    ST_CHECK(clear_lines(&g) == 1);
    ST_CHECK(g.board[BOARD_H - 2][4] && !g.board[BOARD_H - 3][4]);

    // an upright I against the left wall still turns, nudged right
    empty_game(&g);
    g.piece = 0;
    g.rot = 1;
    g.x = -2;
    g.y = 5;
    ST_CHECK(try_rotate(&g));
    ST_CHECK(g.rot == 2 && g.x == 0);

    // every piece comes up exactly once in each run of seven
    empty_game(&g);
    for (int round = 0; round < 3; round++) {
        int seen[PIECES] = {0};
        for (int i = 0; i < PIECES; i++) {
            seen[bag_draw(&g)]++;
        }
        for (int i = 0; i < PIECES; i++) {
            ST_CHECK(seen[i] == 1);
        }
    }

    // the game ends when a new piece has nowhere to go
    empty_game(&g);
    fill_row(&g, 0, 0);
    fill_row(&g, 1, 0);
    g.next = 0;
    spawn(&g);
    ST_CHECK(g.over);

    // the fall speeds up with the level and stops at its floor
    ST_CHECK(fall_interval_ms(1) == 800);
    ST_CHECK(fall_interval_ms(2) == 740);
    ST_CHECK(fall_interval_ms(50) == 100);

    printf("tetris selftest: %s (%d failed)\n", st_failed ? "FAIL" : "ok", st_failed);
    return st_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        return selftest();
    }

    srand((unsigned)(term_ms() ^ ((unsigned long)getpid() << 16)));

    struct game g;
    game_init(&g);

    term_open();
    play(&g);
    term_close();

    printf("tetris: %lu points, %d lines, level %d\n", g.score, g.lines, g.level);
    return 0;
}
