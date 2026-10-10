/*
 * snake.c - Steer a growing snake to the food without hitting anything.
 *
 * `snake --selftest` checks the game rules without touching the terminal.
 */

#include <stddef.h>

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "term.h"
#include "unistd.h"

#define FIELD_W   36
#define FIELD_H   20
#define FIELD_ROW 2 // screen row of the field's top row
#define FIELD_COL 4 // screen column of its left cell; each cell is two columns wide
#define CELLS     (FIELD_W * FIELD_H)
#define TURNS_MAX 2

enum {
    UP,
    DOWN,
    LEFT,
    RIGHT
};

static const int step_x[] = {0, 0, -1, 1};
static const int step_y[] = {-1, 1, 0, 0};

struct point {
    int x, y;
};

struct game {
    struct point body[CELLS]; // a ring: the head at body[head], the tail len - 1 behind it
    unsigned char occupied[FIELD_H][FIELD_W];
    int head, len;
    int dir;
    int turns[TURNS_MAX];
    int nturns;
    struct point food;
    int score, best;
    int over, won, paused;
};

static int opposite(int a, int b)
{
    return (a == UP && b == DOWN) || (a == DOWN && b == UP) || (a == LEFT && b == RIGHT)
           || (a == RIGHT && b == LEFT);
}

static struct point cell_at(const struct game *g, int i)
{
    return g->body[(g->head - i + CELLS) % CELLS];
}

static void place_food(struct game *g)
{
    int free_cells = CELLS - g->len;
    if (free_cells == 0) {
        g->won = 1;
        g->over = 1;
        return;
    }
    int pick = rand() % free_cells;
    for (int y = 0; y < FIELD_H; y++) {
        for (int x = 0; x < FIELD_W; x++) {
            if (g->occupied[y][x]) {
                continue;
            }
            if (pick-- == 0) {
                g->food.x = x;
                g->food.y = y;
                return;
            }
        }
    }
}

// Lays the snake head first along cells[], then puts food somewhere free.
static void set_snake(struct game *g, const struct point *cells, int n, int dir)
{
    int best = g->best;
    memset(g, 0, sizeof(*g));
    g->best = best;
    for (int i = 0; i < n; i++) {
        g->body[n - 1 - i] = cells[i];
        g->occupied[cells[i].y][cells[i].x] = 1;
    }
    g->head = n - 1;
    g->len = n;
    g->dir = dir;
    place_food(g);
}

static void game_init(struct game *g)
{
    int y = FIELD_H / 2;
    int x = FIELD_W / 2;
    struct point start[] = {{x, y}, {x - 1, y}, {x - 2, y}};
    set_snake(g, start, 3, RIGHT);
}

// Queues a turn against the last queued direction, so two quick turns both count.
static void queue_turn(struct game *g, int dir)
{
    int last = g->nturns ? g->turns[g->nturns - 1] : g->dir;
    if (dir == last || opposite(dir, last) || g->nturns == TURNS_MAX) {
        return;
    }
    g->turns[g->nturns++] = dir;
}

static void step(struct game *g)
{
    if (g->nturns) {
        g->dir = g->turns[0];
        memmove(&g->turns[0], &g->turns[1], sizeof(int) * (size_t)(g->nturns - 1));
        g->nturns--;
    }

    struct point h = cell_at(g, 0);
    struct point next = {h.x + step_x[g->dir], h.y + step_y[g->dir]};
    if (next.x < 0 || next.x >= FIELD_W || next.y < 0 || next.y >= FIELD_H) {
        g->over = 1;
        return;
    }

    int grow = next.x == g->food.x && next.y == g->food.y;
    // The tail moves out first, so chasing it into its old cell is allowed.
    if (!grow) {
        struct point tail = cell_at(g, g->len - 1);
        g->occupied[tail.y][tail.x] = 0;
        g->len--;
    }
    if (g->occupied[next.y][next.x]) {
        g->over = 1;
        return;
    }

    g->head = (g->head + 1) % CELLS;
    g->body[g->head] = next;
    g->occupied[next.y][next.x] = 1;
    g->len++;

    if (grow) {
        g->score++;
        if (g->score > g->best) {
            g->best = g->score;
        }
        place_food(g);
    }
}

static int step_interval_ms(int score)
{
    int ms = 150 - 5 * score;
    return ms < 60 ? 60 : ms;
}

// Drawing

static void draw_frame(void)
{
    int top = FIELD_ROW - 1;
    int bottom = FIELD_ROW + FIELD_H;
    int left = FIELD_COL - 1;
    int right = FIELD_COL + 2 * FIELD_W;

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

static void draw_banner(const char *text)
{
    int col = FIELD_COL + FIELD_W - (int)strlen(text) / 2;
    term_print(FIELD_ROW + FIELD_H / 2, col, text, TERM_FG(TERM_BLACK) | TERM_BG(TERM_WHITE));
}

static void draw(const struct game *g)
{
    char line[64];

    term_clear(TERM_NORMAL);
    term_print(0, FIELD_COL - 1, "S N A K E", TERM_BOLD);
    snprintf(line, sizeof(line), "score %d   best %d", g->score, g->best);
    term_print(0, FIELD_COL + 12, line, TERM_NORMAL);
    term_print(0, FIELD_COL + 2 * FIELD_W - 25, "p pause  r restart  q quit", TERM_NORMAL);
    draw_frame();

    term_print(FIELD_ROW + g->food.y, FIELD_COL + 2 * g->food.x, "()",
               TERM_FG(TERM_RED) | TERM_BOLD);
    for (int i = g->len - 1; i >= 0; i--) {
        struct point p = cell_at(g, i);
        int color = i == 0 ? TERM_YELLOW : TERM_GREEN;
        term_print(FIELD_ROW + p.y, FIELD_COL + 2 * p.x, "  ", TERM_BG(color));
    }

    if (g->won) {
        draw_banner(" THE SNAKE FILLS THE FIELD ");
    } else if (g->over) {
        draw_banner(" GAME OVER   r again  q quit ");
    } else if (g->paused) {
        draw_banner(" PAUSED ");
    }
}

// Play

static void play(struct game *g)
{
    unsigned long next_step = term_ms() + (unsigned long)step_interval_ms(g->score);

    for (;;) {
        draw(g);
        term_present();

        int wait = -1;
        if (!g->over && !g->paused) {
            long left = (long)(next_step - term_ms());
            wait = left > 0 ? (int)left : 0;
        }

        int key = term_key(wait);
        if (key == TERM_KEY_NONE) {
            step(g);
            next_step = term_ms() + (unsigned long)step_interval_ms(g->score);
            continue;
        }

        switch (key) {
            case 'q':
            case 'Q':
            case TERM_KEY_EOF:
                return;
            case 'r':
            case 'R':
                if (g->over) {
                    game_init(g);
                    next_step = term_ms() + (unsigned long)step_interval_ms(g->score);
                }
                break;
            case 'p':
            case 'P':
                if (!g->over) {
                    g->paused = !g->paused;
                    next_step = term_ms() + (unsigned long)step_interval_ms(g->score);
                }
                break;
            case TERM_KEY_UP:
            case 'w':
            case 'W':
                queue_turn(g, UP);
                break;
            case TERM_KEY_DOWN:
            case 's':
            case 'S':
                queue_turn(g, DOWN);
                break;
            case TERM_KEY_LEFT:
            case 'a':
            case 'A':
                queue_turn(g, LEFT);
                break;
            case TERM_KEY_RIGHT:
            case 'd':
            case 'D':
                queue_turn(g, RIGHT);
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

static int selftest(void)
{
    static struct game g;

    // a fresh snake moves the way it faces
    game_init(&g);
    struct point h = cell_at(&g, 0);
    g.food.x = 0;
    g.food.y = 0;
    step(&g);
    ST_CHECK(!g.over && cell_at(&g, 0).x == h.x + 1 && cell_at(&g, 0).y == h.y);
    ST_CHECK(g.len == 3);

    // the wall ends the game
    struct point at_wall[] = {{FIELD_W - 1, 5}, {FIELD_W - 2, 5}, {FIELD_W - 3, 5}};
    set_snake(&g, at_wall, 3, RIGHT);
    step(&g);
    ST_CHECK(g.over);

    // so does turning into your own body
    struct point coil[] = {{5, 5}, {6, 5}, {6, 6}, {5, 6}, {4, 6}};
    set_snake(&g, coil, 5, LEFT);
    g.food.x = 0;
    g.food.y = 0;
    queue_turn(&g, DOWN);
    step(&g);
    ST_CHECK(g.over);

    // but not following the tail into the cell it is leaving
    struct point loop[] = {{1, 1}, {1, 0}, {0, 0}, {0, 1}};
    set_snake(&g, loop, 4, DOWN);
    g.food.x = 9;
    g.food.y = 9;
    queue_turn(&g, UP);
    ST_CHECK(g.nturns == 0); // straight back is ignored
    queue_turn(&g, LEFT);
    step(&g);
    ST_CHECK(!g.over && cell_at(&g, 0).x == 0 && cell_at(&g, 0).y == 1);

    // two quick turns are both taken, one per step
    struct point line[] = {{10, 10}, {9, 10}, {8, 10}};
    set_snake(&g, line, 3, RIGHT);
    g.food.x = 0;
    g.food.y = 0;
    queue_turn(&g, UP);
    queue_turn(&g, LEFT);
    step(&g);
    ST_CHECK(cell_at(&g, 0).x == 10 && cell_at(&g, 0).y == 9);
    step(&g);
    ST_CHECK(cell_at(&g, 0).x == 9 && cell_at(&g, 0).y == 9);

    // food grows the snake and scores
    set_snake(&g, line, 3, RIGHT);
    g.food.x = 11;
    g.food.y = 10;
    step(&g);
    ST_CHECK(g.len == 4 && g.score == 1);
    ST_CHECK(!g.occupied[g.food.y][g.food.x]);

    // food never lands on the snake
    struct point snake_long[CELLS / 2];
    for (int i = 0; i < CELLS / 2; i++) {
        int row = i / FIELD_W;
        snake_long[i].y = row;
        snake_long[i].x = (row % 2) ? FIELD_W - 1 - i % FIELD_W : i % FIELD_W;
    }
    set_snake(&g, snake_long, CELLS / 2, RIGHT);
    int clear = 1;
    for (int i = 0; i < 500; i++) {
        place_food(&g);
        clear &= !g.occupied[g.food.y][g.food.x];
    }
    ST_CHECK(clear);

    // speed rises with the score and stops at its floor
    ST_CHECK(step_interval_ms(0) == 150);
    ST_CHECK(step_interval_ms(4) == 130);
    ST_CHECK(step_interval_ms(100) == 60);

    printf("snake selftest: %s (%d failed)\n", st_failed ? "FAIL" : "ok", st_failed);
    return st_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        return selftest();
    }

    srand((unsigned)(term_ms() ^ ((unsigned long)getpid() << 16)));

    static struct game g;
    game_init(&g);

    term_open();
    play(&g);
    term_close();

    printf("snake: score %d, best %d\n", g.score, g.best);
    return 0;
}
