/*
 * mandel.c - The Mandelbrot set on /dev/fb0, drawn by forked workers.
 *
 * `mandel` times a full render with 1 to 4 workers. `mandel -e` explores:
 * arrows pan, + and - zoom, 1-4 pick the worker count, r resets, q quits.
 * `mandel --check` renders one view with 1 worker and with 4 and fails unless
 * the screen comes out identical.
 *
 * Worker k draws rows k, k+N, k+2N... straight into the framebuffer mapping it
 * shares with the others. The math is Q4.28 fixed point: floating point traps
 * in userspace.
 */

#include <stddef.h>
#include <stdint.h>

#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/mman.h"
#include "sys/wait.h"
#include "term.h"
#include "unistd.h"

#define FRAC        28
#define ONE         ((int64_t)1 << FRAC)
#define MAX_WORKERS 4
#define INSIDE      0xFF000000u
#define MIN_SCALE   4 // fixed-point units per pixel; finer than this is all rounding
#define LIMIT_BASE  256
#define LIMIT_STEP  64 // more iterations per halving of the view, or deep views go flat
#define LIMIT_MAX   4096
#define CHECK_LIMIT 64

struct view {
    int64_t cx, cy; // centre
    int64_t scale;  // units per pixel
    int limit;
};

static int width = 1024, height = 768, pitch = 4096;
static volatile uint8_t *fb;
static uint32_t palette[256];

// One number from /proc/fb, or fallback if the file or the key is missing.
static int fb_value(const char *text, const char *key, int fallback)
{
    size_t len = strlen(key);
    for (const char *line = text; line && *line;) {
        if (strncmp(line, key, len) == 0 && line[len] == ' ') {
            return atoi(line + len + 1);
        }
        line = strchr(line, '\n');
        line = line ? line + 1 : NULL;
    }
    return fallback;
}

static int map_framebuffer(void)
{
    char text[128] = "";
    int fd = open("/proc/fb", O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, text, sizeof(text) - 1);
        text[n > 0 ? n : 0] = '\0';
        close(fd);
    }
    width = fb_value(text, "width", width);
    height = fb_value(text, "height", height);
    pitch = fb_value(text, "pitch", pitch);

    // Kept open: while it is, the console stays off the screen.
    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "mandel: cannot open /dev/fb0\n");
        return -1;
    }
    void *p = mmap(NULL, (size_t)pitch * (size_t)height, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mandel: cannot map /dev/fb0\n");
        close(fd);
        return -1;
    }
    fb = p;
    return 0;
}

// Dark blue through white and orange, back to blue: no seam where the index wraps.
static void make_palette(void)
{
    static const struct {
        int at;
        int r, g, b;
    } stops[] = {
        {0, 0, 7, 100},     {41, 32, 107, 203}, {108, 237, 255, 255},
        {164, 255, 170, 0}, {219, 0, 2, 0},     {256, 0, 7, 100},
    };
    for (int s = 0; s + 1 < (int)(sizeof(stops) / sizeof(stops[0])); s++) {
        int span = stops[s + 1].at - stops[s].at;
        for (int i = stops[s].at; i < stops[s + 1].at; i++) {
            int t = i - stops[s].at;
            int r = stops[s].r + (stops[s + 1].r - stops[s].r) * t / span;
            int g = stops[s].g + (stops[s + 1].g - stops[s].g) * t / span;
            int b = stops[s].b + (stops[s + 1].b - stops[s].b) * t / span;
            palette[i] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    }
}

// Iterations before z escapes the radius-2 circle, or limit if it never does.
static int escape(int64_t cr, int64_t ci, int limit)
{
    // Past 2 the point escapes on the first step; beyond it the products would overflow 64 bits.
    if (cr > 2 * ONE || cr < -2 * ONE || ci > 2 * ONE || ci < -2 * ONE) {
        return limit > 1 ? 1 : limit;
    }
    int64_t zr = 0, zi = 0;
    for (int i = 0; i < limit; i++) {
        int64_t zr2 = (zr * zr) >> FRAC;
        int64_t zi2 = (zi * zi) >> FRAC;
        if (zr2 + zi2 > 4 * ONE) {
            return i;
        }
        zi = ((zr * zi) >> (FRAC - 1)) + ci;
        zr = zr2 - zi2 + cr;
    }
    return limit;
}

static void draw_rows(const struct view *v, int first, int step)
{
    for (int y = first; y < height; y += step) {
        volatile uint32_t *row = (volatile uint32_t *)(fb + (size_t)y * (size_t)pitch);
        int64_t ci = v->cy - (int64_t)(y - height / 2) * v->scale;
        for (int x = 0; x < width; x++) {
            int64_t cr = v->cx + (int64_t)(x - width / 2) * v->scale;
            int n = escape(cr, ci, v->limit);
            row[x] = n == v->limit ? INSIDE : palette[(n * 4) & 255];
        }
    }
}

// Renders the view with the given number of workers; the time in ms, or -1 if a fork failed.
static long render(const struct view *v, int workers)
{
    unsigned long start = term_ms();
    int pids[MAX_WORKERS];
    int started = 0;
    for (int k = 0; k < workers; k++) {
        int pid = fork();
        if (pid == 0) {
            draw_rows(v, k, workers);
            _exit(0);
        }
        if (pid < 0) {
            break;
        }
        pids[started++] = pid;
    }
    for (int k = 0; k < started; k++) {
        int status;
        waitpid(pids[k], &status, 0);
    }
    return started == workers ? (long)(term_ms() - start) : -1;
}

static struct view home(void)
{
    // 3.5 units across the screen, centred on -0.5: the whole set.
    struct view v = {-(ONE / 2), 0, 7 * ONE / 2 / width, LIMIT_BASE};
    return v;
}

static int benchmark(void)
{
    struct view v = home();
    printf("mandel: %dx%d, %d iterations\n", width, height, v.limit);
    printf("  workers      time   speedup\n");
    long one = 0;
    for (int workers = 1; workers <= MAX_WORKERS; workers++) {
        long ms = render(&v, workers);
        if (ms < 0) {
            printf("  %7d  could not fork\n", workers);
            return 1;
        }
        if (workers == 1) {
            one = ms > 0 ? ms : 1;
        }
        long x100 = one * 100 / (ms > 0 ? ms : 1);
        printf("  %7d  %5ld ms  %5ld.%02ldx\n", workers, ms, x100 / 100, x100 % 100);
    }
    return 0;
}

// FNV-1a over every visible pixel, padding left out.
static uint32_t screen_hash(void)
{
    uint32_t h = 2166136261u;
    for (int y = 0; y < height; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(fb + (size_t)y * (size_t)pitch);
        for (int x = 0; x < width; x++) {
            h = (h ^ row[x]) * 16777619u;
        }
    }
    return h;
}

static void clear_screen(void)
{
    for (int y = 0; y < height; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(fb + (size_t)y * (size_t)pitch);
        for (int x = 0; x < width; x++) {
            row[x] = 0;
        }
    }
}

static int check(void)
{
    struct view v = home();
    v.limit = CHECK_LIMIT;

    // Cleared first, so a row no worker drew shows up as a different hash.
    clear_screen();
    uint32_t blank = screen_hash();
    if (render(&v, 1) < 0) {
        printf("mandel: could not fork\n");
        return 1;
    }
    uint32_t alone = screen_hash();
    clear_screen();
    if (render(&v, MAX_WORKERS) < 0) {
        printf("mandel: could not fork\n");
        return 1;
    }
    uint32_t shared = screen_hash();

    if (alone == blank || alone != shared) {
        printf("mandel: check failed: 1 worker 0x%08x, %d workers 0x%08x, blank 0x%08x\n", alone,
               MAX_WORKERS, shared, blank);
        return 1;
    }
    printf("mandel: check passed (0x%08x)\n", alone);
    return 0;
}

// Writes a Q4.28 number as a decimal with seven places.
static void fmt_fixed(char *out, size_t size, int64_t v)
{
    uint64_t a = v < 0 ? (uint64_t)-v : (uint64_t)v;
    uint64_t frac = ((a & (uint64_t)(ONE - 1)) * 10000000u) >> FRAC;
    snprintf(out, size, "%s%lu.%07lu", v < 0 ? "-" : "", (unsigned long)(a >> FRAC),
             (unsigned long)frac);
}

static void show_status(const struct view *v, int workers, long ms, const char *note)
{
    char x[24], y[24], w[24], line[TERM_COLS + 1];
    fmt_fixed(x, sizeof(x), v->cx);
    fmt_fixed(y, sizeof(y), v->cy);
    fmt_fixed(w, sizeof(w), v->scale * width);
    term_clear(TERM_NORMAL);
    term_print(0, 0, "mandel: arrows pan, + - zoom, 1-4 workers, r reset, q quit", TERM_BOLD);
    snprintf(line, sizeof(line), "centre %s %s   width %s", x, y, w);
    term_print(2, 0, line, TERM_NORMAL);
    snprintf(line, sizeof(line), "%d iterations, %d worker%s, %ld ms", v->limit, workers,
             workers == 1 ? "" : "s", ms);
    term_print(3, 0, line, TERM_NORMAL);
    term_print(5, 0, note, TERM_FG(TERM_YELLOW));
    term_present();
}

// Zooms by a power of two; the iteration limit follows the depth.
static const char *zoom(struct view *v, int in)
{
    if (in && v->scale / 2 < MIN_SCALE) {
        return "as deep as 28-bit fixed point goes";
    }
    if (!in && v->scale * 2 * width > 8 * ONE) {
        return "the whole set is already in view";
    }
    v->scale = in ? v->scale / 2 : v->scale * 2;
    v->limit += in ? LIMIT_STEP : -LIMIT_STEP;
    v->limit = v->limit < LIMIT_BASE ? LIMIT_BASE : v->limit > LIMIT_MAX ? LIMIT_MAX : v->limit;
    return "";
}

static int explore(void)
{
    struct view v = home();
    int workers = MAX_WORKERS;
    const char *note = "";

    if (term_open() != 0) {
        return 1;
    }
    for (;;) {
        long ms = render(&v, workers);
        show_status(&v, workers, ms, ms < 0 ? "could not fork" : note);
        note = "";

        int key = TERM_KEY_NONE;
        while (key == TERM_KEY_NONE) {
            key = term_key(-1);
            if (key == TERM_KEY_NONE) {
                show_status(&v, workers, ms, note);
            }
        }
        // A quarter of the screen per step.
        int64_t step_x = v.scale * width / 4, step_y = v.scale * height / 4;
        switch (key) {
            case 'q':
            case 'Q':
            case TERM_KEY_ESC:
            case TERM_KEY_EOF:
                term_close();
                return 0;
            case TERM_KEY_LEFT:
                v.cx -= step_x;
                break;
            case TERM_KEY_RIGHT:
                v.cx += step_x;
                break;
            case TERM_KEY_UP:
                v.cy += step_y;
                break;
            case TERM_KEY_DOWN:
                v.cy -= step_y;
                break;
            case '+':
            case '=':
                note = zoom(&v, 1);
                break;
            case '-':
                note = zoom(&v, 0);
                break;
            case 'r':
            case 'R':
                v = home();
                break;
            default:
                if (key >= '1' && key <= '0' + MAX_WORKERS) {
                    workers = key - '0';
                }
                break;
        }
        // Panned too far, the numbers outgrow Q4.28.
        v.cx = v.cx < -4 * ONE ? -4 * ONE : v.cx > 4 * ONE ? 4 * ONE : v.cx;
        v.cy = v.cy < -4 * ONE ? -4 * ONE : v.cy > 4 * ONE ? 4 * ONE : v.cy;
    }
}

int main(int argc, char **argv)
{
    int mode = 0;
    if (argc == 2 && strcmp(argv[1], "-e") == 0) {
        mode = 'e';
    } else if (argc == 2 && strcmp(argv[1], "--check") == 0) {
        mode = 'c';
    } else if (argc != 1) {
        fprintf(stderr, "usage: mandel [-e | --check]\n");
        return 2;
    }

    if (map_framebuffer() != 0) {
        return 1;
    }
    make_palette();

    if (mode == 'e') {
        return explore();
    }
    if (mode == 'c') {
        return check();
    }
    return benchmark();
}
