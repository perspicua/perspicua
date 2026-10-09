/*
 * 2048.c - Slide numbered tiles together until one reaches 2048.
 *
 * `2048 --selftest` checks the merge rules without touching the terminal.
 */

#include <stddef.h>

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "term.h"
#include "unistd.h"

#define N        4
#define TILE_W   7
#define TILE_H   3
#define GRID_ROW 3
#define GRID_COL 24

enum {
    LEFT,
    RIGHT,
    UP,
    DOWN
};

struct game {
    int tile[N][N];
    unsigned long score, best;
    int won;   // reached 2048 at least once
    int shown; // the win banner was already dismissed
    int over;
};

// Slides one line toward index 0, merging each pair once; returns whether anything moved.
static int slide_line(int line[N], unsigned long *score)
{
    int out[N] = {0};
    int n = 0;
    int last_merged = 0;

    for (int i = 0; i < N; i++) {
        if (!line[i]) {
            continue;
        }
        if (n > 0 && !last_merged && out[n - 1] == line[i]) {
            out[n - 1] *= 2;
            *score += (unsigned long)out[n - 1];
            last_merged = 1;
        } else {
            out[n++] = line[i];
            last_merged = 0;
        }
    }

    int moved = memcmp(out, line, sizeof(out)) != 0;
    memcpy(line, out, sizeof(out));
    return moved;
}

// The k-th cell of line i when reading toward the edge the tiles slide to.
static int *cell_of(struct game *g, int dir, int i, int k)
{
    switch (dir) {
        case LEFT:
            return &g->tile[i][k];
        case RIGHT:
            return &g->tile[i][N - 1 - k];
        case UP:
            return &g->tile[k][i];
        default:
            return &g->tile[N - 1 - k][i];
    }
}

static int can_move(const struct game *g)
{
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            if (!g->tile[r][c]) {
                return 1;
            }
            if (c + 1 < N && g->tile[r][c] == g->tile[r][c + 1]) {
                return 1;
            }
            if (r + 1 < N && g->tile[r][c] == g->tile[r + 1][c]) {
                return 1;
            }
        }
    }
    return 0;
}

static void spawn(struct game *g)
{
    int empty = 0;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            empty += !g->tile[r][c];
        }
    }
    if (!empty) {
        return;
    }
    int pick = rand() % empty;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            if (!g->tile[r][c] && pick-- == 0) {
                g->tile[r][c] = rand() % 10 == 0 ? 4 : 2;
            }
        }
    }
}

// A move that changes nothing spawns nothing.
static int move(struct game *g, int dir)
{
    int moved = 0;
    for (int i = 0; i < N; i++) {
        int line[N];
        for (int k = 0; k < N; k++) {
            line[k] = *cell_of(g, dir, i, k);
        }
        moved |= slide_line(line, &g->score);
        for (int k = 0; k < N; k++) {
            *cell_of(g, dir, i, k) = line[k];
            if (line[k] >= 2048) {
                g->won = 1;
            }
        }
    }

    if (g->score > g->best) {
        g->best = g->score;
    }
    if (moved) {
        spawn(g);
    }
    g->over = !can_move(g);
    return moved;
}

static void game_init(struct game *g)
{
    unsigned long best = g->best;
    memset(g, 0, sizeof(*g));
    g->best = best;
    spawn(g);
    spawn(g);
}

// Drawing

static unsigned tile_attr(int value)
{
    switch (value) {
        case 2:
            return TERM_FG(TERM_BLACK) | TERM_BG(TERM_WHITE);
        case 4:
            return TERM_FG(TERM_BLACK) | TERM_BG(TERM_YELLOW);
        case 8:
            return TERM_FG(TERM_WHITE) | TERM_BG(TERM_RED) | TERM_BOLD;
        case 16:
            return TERM_FG(TERM_YELLOW) | TERM_BG(TERM_RED) | TERM_BOLD;
        case 32:
            return TERM_FG(TERM_WHITE) | TERM_BG(TERM_MAGENTA) | TERM_BOLD;
        case 64:
            return TERM_FG(TERM_YELLOW) | TERM_BG(TERM_MAGENTA) | TERM_BOLD;
        case 128:
            return TERM_FG(TERM_WHITE) | TERM_BG(TERM_BLUE) | TERM_BOLD;
        case 256:
            return TERM_FG(TERM_YELLOW) | TERM_BG(TERM_BLUE) | TERM_BOLD;
        case 512:
            return TERM_FG(TERM_BLACK) | TERM_BG(TERM_CYAN);
        case 1024:
            return TERM_FG(TERM_BLACK) | TERM_BG(TERM_GREEN);
        case 2048:
            return TERM_FG(TERM_YELLOW) | TERM_BG(TERM_GREEN) | TERM_BOLD;
        default:
            return TERM_FG(TERM_WHITE) | TERM_BG(TERM_BLACK) | TERM_BOLD;
    }
}

static void draw_tile(int r, int c, int value)
{
    int top = GRID_ROW + r * (TILE_H + 1);
    int left = GRID_COL + c * (TILE_W + 1);

    if (!value) {
        term_print(top + TILE_H / 2, left + TILE_W / 2, "·", TERM_NORMAL);
        return;
    }

    unsigned attr = tile_attr(value);
    for (int y = 0; y < TILE_H; y++) {
        for (int x = 0; x < TILE_W; x++) {
            term_put(top + y, left + x, ' ', attr);
        }
    }
    char num[12];
    int len = snprintf(num, sizeof(num), "%d", value);
    term_print(top + TILE_H / 2, left + (TILE_W - len) / 2, num, attr);
}

static void draw_banner(int row, const char *text)
{
    int width = N * (TILE_W + 1) - 1;
    int col = GRID_COL + (width - (int)strlen(text)) / 2;
    term_print(row, col, text, TERM_FG(TERM_BLACK) | TERM_BG(TERM_WHITE));
}

static void draw(const struct game *g)
{
    char line[64];

    term_clear(TERM_NORMAL);
    term_print(0, GRID_COL, "2 0 4 8", TERM_BOLD);
    snprintf(line, sizeof(line), "score %lu   best %lu", g->score, g->best);
    term_print(1, GRID_COL, line, TERM_NORMAL);

    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            draw_tile(r, c, g->tile[r][c]);
        }
    }

    int below = GRID_ROW + N * (TILE_H + 1);
    term_print(below, GRID_COL, "arrows/wasd slide  r restart  q quit", TERM_NORMAL);

    int middle = GRID_ROW + N * (TILE_H + 1) / 2 - 1;
    if (g->over) {
        draw_banner(middle, " no moves left ");
        draw_banner(middle + 1, " r again  q quit ");
    } else if (g->won && !g->shown) {
        draw_banner(middle, " 2048! ");
        draw_banner(middle + 1, " any key: keep going ");
    }
}

// Play

static void play(struct game *g)
{
    for (;;) {
        draw(g);
        term_present();

        int key = term_key(-1);
        if (key == 'q' || key == 'Q') {
            return;
        }
        if (key == 'r' || key == 'R') {
            game_init(g);
            continue;
        }
        if (g->won && !g->shown) {
            g->shown = 1;
            continue;
        }
        if (g->over) {
            continue;
        }

        switch (key) {
            case TERM_KEY_LEFT:
            case 'a':
            case 'A':
                move(g, LEFT);
                break;
            case TERM_KEY_RIGHT:
            case 'd':
            case 'D':
                move(g, RIGHT);
                break;
            case TERM_KEY_UP:
            case 'w':
            case 'W':
                move(g, UP);
                break;
            case TERM_KEY_DOWN:
            case 's':
            case 'S':
                move(g, DOWN);
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

static int slides_to(int a, int b, int c, int d, int ea, int eb, int ec, int ed,
                     unsigned long points)
{
    int line[N] = {a, b, c, d};
    unsigned long score = 0;
    slide_line(line, &score);
    return line[0] == ea && line[1] == eb && line[2] == ec && line[3] == ed && score == points;
}

static int selftest(void)
{
    struct game g;

    // each pair merges once, from the edge the tiles slide toward
    ST_CHECK(slides_to(2, 2, 2, 2, 4, 4, 0, 0, 8));
    ST_CHECK(slides_to(2, 2, 4, 0, 4, 4, 0, 0, 4));
    ST_CHECK(slides_to(4, 0, 4, 4, 8, 4, 0, 0, 8));
    ST_CHECK(slides_to(2, 0, 0, 2, 4, 0, 0, 0, 4));
    ST_CHECK(slides_to(0, 0, 0, 2, 2, 0, 0, 0, 0));
    ST_CHECK(slides_to(8, 8, 16, 16, 16, 32, 0, 0, 48));

    int stuck[N] = {2, 4, 8, 16};
    unsigned long score = 0;
    ST_CHECK(!slide_line(stuck, &score));

    // every direction slides toward its own edge (spawn may refill the cells left behind)
    memset(&g, 0, sizeof(g));
    g.tile[1][0] = 2;
    g.tile[1][1] = 2;
    move(&g, RIGHT);
    ST_CHECK(g.tile[1][3] == 4 && g.score == 4);

    memset(&g, 0, sizeof(g));
    g.tile[0][2] = 8;
    g.tile[2][2] = 8;
    move(&g, DOWN);
    ST_CHECK(g.tile[3][2] == 16 && g.score == 16);

    memset(&g, 0, sizeof(g));
    g.tile[3][1] = 4;
    move(&g, UP);
    ST_CHECK(g.tile[0][1] == 4 && g.score == 0);

    // a move that changes nothing spawns nothing
    memset(&g, 0, sizeof(g));
    g.tile[0][0] = 2;
    g.tile[0][1] = 4;
    ST_CHECK(!move(&g, LEFT));
    int tiles = 0;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            tiles += g.tile[r][c] != 0;
        }
    }
    ST_CHECK(tiles == 2);

    // one that does spawns exactly one 2 or 4 on an empty cell
    ST_CHECK(move(&g, RIGHT));
    tiles = 0;
    int odd = 0;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            tiles += g.tile[r][c] != 0;
            if (r != 0 && g.tile[r][c] && g.tile[r][c] != 2 && g.tile[r][c] != 4) {
                odd = 1;
            }
        }
    }
    ST_CHECK(tiles == 3 && !odd);

    // a full board with no equal neighbours has no moves left
    static const int checker[N][N] = {{2, 4, 2, 4}, {4, 2, 4, 2}, {2, 4, 2, 4}, {4, 2, 4, 2}};
    memset(&g, 0, sizeof(g));
    memcpy(g.tile, checker, sizeof(checker));
    ST_CHECK(!can_move(&g));
    g.tile[0][1] = 2;
    ST_CHECK(can_move(&g));

    // reaching 2048 marks the win
    memset(&g, 0, sizeof(g));
    g.tile[2][0] = 1024;
    g.tile[2][1] = 1024;
    move(&g, LEFT);
    ST_CHECK(g.won && g.tile[2][0] == 2048);

    printf("2048 selftest: %s (%d failed)\n", st_failed ? "FAIL" : "ok", st_failed);
    return st_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        return selftest();
    }

    srand((unsigned)(term_ms() ^ ((unsigned long)getpid() << 16)));

    struct game g = {0};
    game_init(&g);

    term_open();
    play(&g);
    term_close();

    printf("2048: score %lu, best %lu\n", g.score, g.best);
    return 0;
}
