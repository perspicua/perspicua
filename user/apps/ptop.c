/*
 * ptop.c - A btop-style system monitor: CPU, memory, system and process panels.
 *
 * Every figure is a delta between two samples of /proc: per-core busy and idle
 * milliseconds from /proc/stat, interrupt counts from /proc/interrupts, and
 * each process's CpuTime from its status file. The panels share out whatever
 * size the terminal reports, 80x24 at the least. `ptop --once` takes two
 * samples, prints one frame as plain text and exits.
 */

#include <stdarg.h>
#include <stddef.h>

#include "dirent.h"
#include "errno.h"
#include "fcntl.h"
#include "signal.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "term.h"
#include "unistd.h"

#define FIRST_GAP_MS 300 // between the two samples the first frame is drawn from
#define MESSAGE_MS   3000
#define MAX_CORES    8
#define MAX_PROCS    128
#define HIST_MAX     TERM_MAX_COLS // one sample per graph column, at the widest
#define READ_BUF     1024

// As in btop: + and - add or take 100 ms from the update timer.
#define SIZE_ASK_MS     2000
#define REFRESH_STEP_MS 100
#define REFRESH_MAX_MS  10000

#define CPU_COLOUR  TERM_GREEN
#define MEM_COLOUR  TERM_YELLOW
#define SYS_COLOUR  TERM_MAGENTA
#define PROC_COLOUR TERM_RED
#define KILL_COLOUR TERM_RED

#define ATTR_TITLE    (TERM_FG(TERM_WHITE) | TERM_BOLD)
#define ATTR_DIM      (TERM_FG(TERM_BLACK) | TERM_BOLD)
#define ATTR_SELECTED (TERM_BG(TERM_BLUE) | TERM_FG(TERM_WHITE) | TERM_BOLD)
#define ATTR_WARN     (TERM_FG(TERM_YELLOW) | TERM_BOLD)
#define ATTR_ALERT    (TERM_FG(TERM_RED) | TERM_BOLD)

#define CTRL(c) ((c) & 0x1f)

#define BOX_H  0x2500
#define BOX_V  0x2502
#define BOX_TL 0x256d
#define BOX_TR 0x256e
#define BOX_BL 0x2570
#define BOX_BR 0x256f
#define SQUARE 0x25a0

struct proc {
    int pid, ppid;
    char name[32];
    char state[12];
    unsigned long mem_kb;
    unsigned long cpu_ms;
    unsigned long pct10; // tenths of a percent of one core over the last interval
    unsigned long start; // with the pid, names the process: a freed pid is reused at once
};

struct sample {
    unsigned long at_ms;
    int ncores;
    int core_id[MAX_CORES];
    unsigned long busy[MAX_CORES], idle[MAX_CORES];
    unsigned long irqs, ctxt, forks;
    unsigned long mem_total, mem_free, slab_used, slab_total;
    unsigned long uptime_s;
    int nprocs;
    struct proc procs[MAX_PROCS];
};

enum {
    SORT_CPU,
    SORT_MEM,
    SORT_PID,
    SORT_KEYS
};

static struct sample samples[2];
static struct sample *cur = &samples[0], *prev = &samples[1];
static int sampled;

static int core_pct[MAX_CORES], total_pct;
static unsigned long irq_rate, ctxt_rate, fork_rate;
static long arm_mhz = -1, temp_mc = -1, throttled = -1;

// Newest last; every ring takes one entry per sample, so they share a count.
static unsigned char total_hist[HIST_MAX], mem_hist[HIST_MAX], core_hist[MAX_CORES][HIST_MAX];
static int hist_count;

static unsigned long refresh_ms = 1000;
static unsigned long size_asked;

static int sort_key = SORT_CPU;
static int sel, sel_pid = -1, scroll, list_rows;

static int pending_sig, pending_pid;
static unsigned long pending_start;
static char pending_name[32];
static char message[TERM_MAX_COLS + 1];
static unsigned long message_until;

static void set_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    message_until = term_ms() + MESSAGE_MS;
}

static int read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    int total = 0;
    int n;
    while ((size_t)total < size - 1 && (n = read(fd, buf + total, size - 1 - (size_t)total)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    close(fd);
    return total;
}

// Reads the number at *p, blanks before it skipped, and leaves *p past its digits.
static unsigned long number(const char **p)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned long)(*s++ - '0');
    }
    *p = s;
    return v;
}

static const char *skip_word(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
        p++;
    }
    return p;
}

static const char *next_line(const char *p)
{
    while (*p && *p != '\n') {
        p++;
    }
    return *p ? p + 1 : p;
}

static const char *find_field(const char *buf, const char *key)
{
    size_t len = strlen(key);
    for (const char *line = buf; *line; line = next_line(line)) {
        if (strncmp(line, key, len) == 0 && line[len] == ':') {
            return line + len + 1;
        }
    }
    return NULL;
}

static unsigned long field_ul(const char *buf, const char *key)
{
    const char *p = find_field(buf, key);
    return p ? number(&p) : 0;
}

static void field_str(const char *buf, const char *key, char *out, size_t size)
{
    const char *p = find_field(buf, key);
    size_t n = 0;
    if (p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        while (*p && *p != '\n' && n < size - 1) {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
}

// The number on a "key value" line, as /proc/clock writes them; -1 if missing.
static long clock_value(const char *text, const char *key)
{
    size_t len = strlen(key);
    for (const char *line = text; *line; line = next_line(line)) {
        if (strncmp(line, key, len) == 0 && line[len] == ' ') {
            const char *p = line + len + 1;
            int base = 10;
            if (p[0] == '0' && p[1] == 'x') {
                base = 16;
                p += 2;
            }
            long v = 0;
            for (;; p++) {
                int d = (*p >= '0' && *p <= '9')   ? *p - '0'
                        : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                                                   : -1;
                if (d < 0 || d >= base) {
                    return v;
                }
                v = v * base + d;
            }
        }
    }
    return -1;
}

static unsigned long delta(unsigned long now, unsigned long then)
{
    return now > then ? now - then : 0;
}

static void read_stat(struct sample *s)
{
    char buf[READ_BUF];
    s->ncores = 0;
    s->ctxt = 0;
    s->forks = 0;
    if (read_file("/proc/stat", buf, sizeof(buf)) < 0) {
        return;
    }
    for (const char *line = buf; *line; line = next_line(line)) {
        const char *p = line;
        if (strncmp(line, "ctxt ", 5) == 0) {
            p += 5;
            s->ctxt = number(&p);
        } else if (strncmp(line, "processes ", 10) == 0) {
            p += 10;
            s->forks = number(&p);
        } else if (strncmp(line, "cpu", 3) == 0 && s->ncores < MAX_CORES) {
            p += 3;
            int i = s->ncores++;
            s->core_id[i] = (int)number(&p);
            s->busy[i] = number(&p);
            s->idle[i] = number(&p);
        }
    }
}

// Sums every per-core count on every line after the header.
static unsigned long read_irqs(void)
{
    char buf[READ_BUF];
    if (read_file("/proc/interrupts", buf, sizeof(buf)) < 0) {
        return 0;
    }
    unsigned long total = 0;
    for (const char *line = next_line(buf); *line; line = next_line(line)) {
        const char *p = skip_word(skip_word(line));
        for (;;) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p < '0' || *p > '9') {
                break;
            }
            total += number(&p);
        }
    }
    return total;
}

static void read_mem(struct sample *s)
{
    char buf[READ_BUF];
    if (read_file("/proc/meminfo", buf, sizeof(buf)) > 0) {
        s->mem_total = field_ul(buf, "MemTotal");
        s->mem_free = field_ul(buf, "MemFree");
        s->slab_used = field_ul(buf, "SlabUsed");
        s->slab_total = field_ul(buf, "SlabTotal");
    }
    if (read_file("/proc/uptime", buf, sizeof(buf)) > 0) {
        const char *p = buf;
        s->uptime_s = number(&p);
    }
}

static void read_clock(void)
{
    char buf[256];
    if (read_file("/proc/clock", buf, sizeof(buf)) > 0) {
        arm_mhz = clock_value(buf, "arm_mhz");
        temp_mc = clock_value(buf, "temp_mc");
        throttled = clock_value(buf, "throttled");
    }
}

static void read_procs(struct sample *s)
{
    s->nprocs = 0;
    DIR *dir = opendir("/proc");
    if (!dir) {
        return;
    }
    char path[48], buf[READ_BUF];
    struct dirent *ent;
    while (s->nprocs < MAX_PROCS && (ent = readdir(dir)) != NULL) {
        const char *p = ent->d_name;
        if (*p < '0' || *p > '9') {
            continue;
        }
        int pid = (int)number(&p);
        if (*p != '\0') {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        // Gone between the listing and the read.
        if (read_file(path, buf, sizeof(buf)) <= 0) {
            continue;
        }
        struct proc *pr = &s->procs[s->nprocs++];
        pr->pid = pid;
        pr->ppid = (int)field_ul(buf, "PPid");
        pr->mem_kb = field_ul(buf, "VmSize");
        pr->cpu_ms = field_ul(buf, "CpuTime");
        pr->start = field_ul(buf, "StartTicks");
        pr->pct10 = 0;
        field_str(buf, "Name", pr->name, sizeof(pr->name));
        field_str(buf, "State", pr->state, sizeof(pr->state));
        size_t len = strlen(pr->name);
        if (len > 4 && strcmp(pr->name + len - 4, ".elf") == 0) {
            pr->name[len - 4] = '\0';
        }
    }
    closedir(dir);
}

static void push(unsigned char *ring, int value)
{
    if (hist_count == HIST_MAX) {
        memmove(ring, ring + 1, HIST_MAX - 1);
    }
    ring[hist_count == HIST_MAX ? HIST_MAX - 1 : hist_count] = (unsigned char)value;
}

static void derive_rates(void)
{
    unsigned long dt = delta(cur->at_ms, prev->at_ms);
    if (dt == 0) {
        dt = 1;
    }
    irq_rate = delta(cur->irqs, prev->irqs) * 1000 / dt;
    ctxt_rate = delta(cur->ctxt, prev->ctxt) * 1000 / dt;
    fork_rate = delta(cur->forks, prev->forks) * 1000 / dt;

    unsigned long all_busy = 0, all = 0;
    for (int i = 0; i < cur->ncores; i++) {
        unsigned long busy = i < prev->ncores ? delta(cur->busy[i], prev->busy[i]) : 0;
        unsigned long idle = i < prev->ncores ? delta(cur->idle[i], prev->idle[i]) : 0;
        core_pct[i] = busy + idle ? (int)(busy * 100 / (busy + idle)) : 0;
        all_busy += busy;
        all += busy + idle;
        push(core_hist[i], core_pct[i]);
    }
    total_pct = all ? (int)(all_busy * 100 / all) : 0;
    push(total_hist, total_pct);
    unsigned long used = delta(cur->mem_total, cur->mem_free);
    push(mem_hist, cur->mem_total ? (int)(used * 100 / cur->mem_total) : 0);
    if (hist_count < HIST_MAX) {
        hist_count++;
    }

    for (int i = 0; i < cur->nprocs; i++) {
        struct proc *p = &cur->procs[i];
        // A process missing from the last sample was born since, with all its time in this interval.
        unsigned long then = 0;
        for (int j = 0; j < prev->nprocs; j++) {
            if (prev->procs[j].pid == p->pid) {
                then = prev->procs[j].cpu_ms;
                break;
            }
        }
        p->pct10 = delta(p->cpu_ms, then) * 1000 / dt;
    }
}

static int compare(const void *a, const void *b)
{
    const struct proc *x = a;
    const struct proc *y = b;
    if (sort_key == SORT_CPU && x->pct10 != y->pct10) {
        return x->pct10 > y->pct10 ? -1 : 1;
    }
    if (sort_key == SORT_MEM && x->mem_kb != y->mem_kb) {
        return x->mem_kb > y->mem_kb ? -1 : 1;
    }
    return x->pid - y->pid;
}

// Keeps the selection on the same process across a re-sort, or on the same row once it is gone.
static void follow_selection(void)
{
    int n = cur->nprocs;
    for (int i = 0; i < n; i++) {
        if (cur->procs[i].pid == sel_pid) {
            sel = i;
            return;
        }
    }
    if (sel >= n) {
        sel = n - 1;
    }
    if (sel < 0) {
        sel = 0;
    }
    sel_pid = n ? cur->procs[sel].pid : -1;
}

static void resort(void)
{
    qsort(cur->procs, (size_t)cur->nprocs, sizeof(struct proc), compare);
    follow_selection();
}

static void take_sample(void)
{
    struct sample *t = prev;
    prev = cur;
    cur = t;

    cur->at_ms = term_ms();
    read_stat(cur);
    cur->irqs = read_irqs();
    read_mem(cur);
    read_procs(cur);
    read_clock();

    // The first sample has nothing to compare against and is never shown.
    if (sampled) {
        derive_rates();
        resort();
    }
    sampled = 1;
}

// Drawing

static int text_width(const char *s)
{
    int w = 0;
    for (; *s; s++) {
        w += ((unsigned char)*s & 0xc0) != 0x80;
    }
    return w;
}

// Prints at most width columns of s.
static int put_text(int row, int col, int width, const char *s, unsigned attr)
{
    char clipped[TERM_MAX_COLS * 4 + 1];
    size_t n = 0;
    int w = 0;
    for (const char *p = s; *p && n < sizeof(clipped) - 1; p++) {
        int starts = ((unsigned char)*p & 0xc0) != 0x80;
        if (starts && w == width) {
            break;
        }
        w += starts;
        clipped[n++] = *p;
    }
    clipped[n] = '\0';
    return term_print(row, col, clipped, attr);
}

static void fill(int row, int col, int width, unsigned attr)
{
    for (int c = 0; c < width; c++) {
        term_put(row, col + c, ' ', attr);
    }
}

// Green through yellow to red as a share of something climbs.
static unsigned heat(int pct)
{
    return TERM_FG(pct > 80 ? TERM_RED : pct > 50 ? TERM_YELLOW : TERM_GREEN);
}

static void box(int y, int x, int h, int w, int colour, const char *title)
{
    unsigned a = TERM_FG(colour);
    for (int c = 1; c < w - 1; c++) {
        term_put(y, x + c, BOX_H, a);
        term_put(y + h - 1, x + c, BOX_H, a);
    }
    for (int r = 1; r < h - 1; r++) {
        term_put(y + r, x, BOX_V, a);
        term_put(y + r, x + w - 1, BOX_V, a);
    }
    term_put(y, x, BOX_TL, a);
    term_put(y, x + w - 1, BOX_TR, a);
    term_put(y + h - 1, x, BOX_BL, a);
    term_put(y + h - 1, x + w - 1, BOX_BR, a);
    term_put(y, x + 2, ' ', a);
    int n = term_print(y, x + 3, title, ATTR_TITLE);
    term_put(y, x + 3 + n, ' ', a);
}

// Sets labels into a border, right-aligned and a line apart, the last one rightmost.
static void border_right(int row, int right, const char *const *labels, int n, unsigned attr)
{
    int col = right - 1;
    for (int i = n - 1; i >= 0; i--) {
        int w = text_width(labels[i]);
        col -= w + 2;
        term_put(row, col, ' ', TERM_NORMAL);
        term_print(row, col + 1, labels[i], attr);
        term_put(row, col + 1 + w, ' ', TERM_NORMAL);
        col--;
    }
}

// Squares lit along the scale, as btop's meters are; a scale meter also shows the unlit rest.
static void meter(int row, int col, int width, int pct, int scale, unsigned bg)
{
    int lit = (pct * width + 50) / 100;
    for (int i = 0; i < width; i++) {
        if (i < lit) {
            term_put(row, col + i, SQUARE, heat((i + 1) * 100 / width) | bg);
        } else {
            term_put(row, col + i, scale ? SQUARE : ' ', (scale ? ATTR_DIM : TERM_NORMAL) | bg);
        }
    }
}

// Newest sample in the rightmost column; each row adds eight levels of height.
static void graph(int y, int x, int h, int w, const unsigned char *ring)
{
    static const uint32_t levels[] = {' ',    0x2581, 0x2582, 0x2583, 0x2584,
                                      0x2585, 0x2586, 0x2587, 0x2588};
    for (int c = 0; c < w; c++) {
        int i = hist_count - w + c;
        int eighths = i >= 0 ? ring[i] * h * 8 / 100 : 0;
        for (int r = 0; r < h; r++) {
            int level = eighths - r * 8;
            level = level < 0 ? 0 : level > 8 ? 8 : level;
            term_put(y + h - 1 - r, x + c, levels[level], heat((r + 1) * 100 / h));
        }
    }
}

static void draw_cpu(int y, int h, int cols)
{
    char up[32], every[24], text[32];
    unsigned long s = cur->uptime_s;
    snprintf(up, sizeof(up), "up %lu:%02lu:%02lu", s / 3600, s / 60 % 60, s % 60);
    snprintf(every, sizeof(every), "- %lums +", refresh_ms);
    const char *labels[] = {up, every};

    box(y, 0, h, cols, CPU_COLOUR, "cpu");
    border_right(y, cols - 1, labels, 2, TERM_FG(TERM_WHITE));

    // Wide enough, and each core also gets its own history.
    int spark = cols >= 120 ? 20 : 0;
    int core_w = 19 + (spark ? spark + 1 : 0);
    int inner = h - 2;
    int graph_w = cols - 2 - core_w - 2;
    graph(y + 1, 1, inner, graph_w, total_hist);
    for (int r = 0; r < inner; r++) {
        term_put(y + 1 + r, 1 + graph_w, BOX_V, TERM_FG(CPU_COLOUR));
    }

    // The whole machine first, then each core.
    int x = graph_w + 3;
    for (int i = -1; i < cur->ncores && i + 2 < inner; i++) {
        int row = y + 2 + i;
        int pct = i < 0 ? total_pct : core_pct[i];
        if (i < 0) {
            term_print(row, x, "CPU", ATTR_TITLE);
        } else {
            snprintf(text, sizeof(text), "C%d", cur->core_id[i]);
            term_print(row, x, text, TERM_FG(CPU_COLOUR) | TERM_BOLD);
        }
        meter(row, x + 4, 10, pct, 1, 0);
        snprintf(text, sizeof(text), "%3d%%", pct);
        term_print(row, x + 15, text, TERM_NORMAL);
        if (spark) {
            graph(row, x + 20, 1, spark, i < 0 ? total_hist : core_hist[i]);
        }
    }

    int info = y + 2 + cur->ncores;
    if (info >= y + h - 1) {
        return;
    }
    int col = x;
    if (arm_mhz > 0) {
        snprintf(text, sizeof(text), "%ld.%02ld GHz", arm_mhz / 1000, arm_mhz % 1000 / 10);
        col += term_print(info, col, text, TERM_NORMAL) + 2;
    }
    if (temp_mc > 0) {
        int celsius = (int)((temp_mc + 500) / 1000);
        snprintf(text, sizeof(text), "%d°C", celsius);
        unsigned a = celsius >= 80 ? ATTR_ALERT : celsius >= 70 ? ATTR_WARN : TERM_NORMAL;
        col += term_print(info, col, text, a) + 2;
    }
    if (spark) {
        snprintf(text, sizeof(text), "%lu irq/s", irq_rate);
        term_print(info, col, text, TERM_NORMAL);
    }
}

static void mem_line(int row, int col, int width, const char *label, unsigned long used,
                     unsigned long total, const char *unit)
{
    char value[40];
    snprintf(value, sizeof(value), "%lu/%lu %s", used, total, unit);
    int vw = text_width(value);
    int mw = width - 5 - vw - 1;
    term_print(row, col, label, TERM_FG(MEM_COLOUR) | TERM_BOLD);
    if (mw > 0) {
        meter(row, col + 5, mw, total ? (int)(used * 100 / total) : 0, 1, 0);
    }
    term_print(row, col + width - vw, value, TERM_NORMAL);
}

static void draw_mem(int y, int h, int x, int w)
{
    box(y, x, h, w, MEM_COLOUR, "mem");
    unsigned long used = delta(cur->mem_total, cur->mem_free);
    mem_line(y + 1, x + 2, w - 4, "used", used / 1024, cur->mem_total / 1024, "MiB");
    if (h - 2 >= 2) {
        mem_line(y + 2, x + 2, w - 4, "slab", cur->slab_used, cur->slab_total, "KiB");
    }
    if (h - 2 >= 4) {
        graph(y + 3, x + 1, h - 4, w - 2, mem_hist);
    }
}

static void throttle_text(char *out, size_t size, unsigned long bits)
{
    static const char *const reasons[] = {"under-voltage", "capped", "throttled", "hot"};
    out[0] = '\0';
    for (int i = 0; i < 4; i++) {
        if (bits & (1ul << i)) {
            snprintf(out + strlen(out), size - strlen(out), "%s%s", out[0] ? ", " : "", reasons[i]);
        }
    }
}

static void draw_system(int y, int h, int x, int w)
{
    box(y, x, h, w, SYS_COLOUR, "system");

    int zombies = 0;
    for (int i = 0; i < cur->nprocs; i++) {
        zombies += strcmp(cur->procs[i].state, "zombie") == 0;
    }
    char items[5][32];
    unsigned attrs[5];
    int n = 0;
    snprintf(items[n], 32, "%d procs", cur->nprocs);
    attrs[n++] = TERM_NORMAL;
    if (zombies) {
        snprintf(items[n], 32, "%d zombie%s", zombies, zombies == 1 ? "" : "s");
        attrs[n++] = ATTR_WARN;
    }
    snprintf(items[n], 32, "%lu forks/s", fork_rate);
    attrs[n++] = TERM_NORMAL;
    snprintf(items[n], 32, "%lu ctx/s", ctxt_rate);
    attrs[n++] = TERM_NORMAL;
    snprintf(items[n], 32, "%lu irq/s", irq_rate);
    attrs[n++] = TERM_NORMAL;

    // Packed onto as many lines as fit, the last row kept for throttling.
    int width = w - 4, row = y + 1, last = y + h - 2, col = 0;
    for (int i = 0; i < n; i++) {
        int iw = text_width(items[i]);
        if (col && col + 2 + iw > width) {
            row++;
            col = 0;
        }
        if (row > last || (row == last && throttled >= 0)) {
            break;
        }
        col += col ? 2 : 0;
        term_print(row, x + 2 + col, items[i], attrs[i]);
        col += iw;
    }

    if (throttled < 0) {
        return;
    }
    char why[64], text[96];
    if (throttled & 0xf) {
        throttle_text(why, sizeof(why), (unsigned long)throttled & 0xf);
        snprintf(text, sizeof(text), "throttled: %s", why);
        put_text(last, x + 2, width, text, ATTR_ALERT);
    } else if ((throttled >> 16) & 0xf) {
        throttle_text(why, sizeof(why), ((unsigned long)throttled >> 16) & 0xf);
        snprintf(text, sizeof(text), "was throttled: %s", why);
        put_text(last, x + 2, width, text, ATTR_WARN);
    } else {
        put_text(last, x + 2, width, "not throttled", TERM_FG(TERM_GREEN));
    }
}

struct columns {
    int width, pid, name, name_w, state, bar, bar_w, pct, mem, time;
};

// Name and CPU bar share whatever the fixed columns leave.
static struct columns columns_for(int width)
{
    struct columns c;
    int rest = width - 41;
    c.width = width;
    c.name_w = rest / 2 < 16 ? 16 : rest / 2 > 32 ? 32 : rest / 2;
    c.bar_w = rest - c.name_w;
    c.pid = 1;
    c.name = c.pid + 7;
    c.state = c.name + c.name_w + 1;
    c.bar = c.state + 8;
    c.pct = c.bar + c.bar_w;
    c.mem = c.pct + 7;
    c.time = c.mem + 8;
    return c;
}

static void draw_proc_row(int row, const struct columns *c, const struct proc *p, int selected)
{
    char text[40];
    unsigned base = selected                          ? ATTR_SELECTED
                    : strcmp(p->state, "zombie") == 0 ? TERM_FG(TERM_YELLOW)
                                                      : TERM_NORMAL;
    if (selected) {
        fill(row, 1, c->width, base);
    }
    snprintf(text, sizeof(text), "%5d", p->pid);
    term_print(row, c->pid, text, base);
    put_text(row, c->name, c->name_w, p->name, base);
    put_text(row, c->state, 7, p->state, base);

    int pct = (int)(p->pct10 / 10);
    meter(row, c->bar, c->bar_w - 1, pct > 100 ? 100 : pct, 0, selected ? TERM_BG(TERM_BLUE) : 0);
    snprintf(text, sizeof(text), "%5lu.%lu", p->pct10 / 10, p->pct10 % 10);
    term_print(row, c->pct, text, base);
    if (p->mem_kb < 10240) {
        snprintf(text, sizeof(text), "%6luK", p->mem_kb);
    } else {
        snprintf(text, sizeof(text), "%4lu.%luM", p->mem_kb / 1024, p->mem_kb % 1024 * 10 / 1024);
    }
    term_print(row, c->mem, text, base);
    snprintf(text, sizeof(text), "%4lu:%02lu.%02lu", p->cpu_ms / 60000, p->cpu_ms / 1000 % 60,
             p->cpu_ms % 1000 / 10);
    term_print(row, c->time, text, base);
}

// Key hints in the bottom border, or the latest message while it lasts.
static void draw_hints(int row, int col, int width)
{
    static const char *const hints[][2] = {
        {"↑↓", "select"},    {"← →", "sort"},  {"k", "kill"},
        {"- +", "interval"}, {"^L", "redraw"}, {"q", "quit"},
    };
    term_put(row, col - 1, ' ', TERM_NORMAL);
    if (message[0] && term_ms() < message_until) {
        int n = put_text(row, col, width, message, ATTR_WARN);
        term_put(row, col + n, ' ', TERM_NORMAL);
        return;
    }
    int x = col;
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        int need = text_width(hints[i][0]) + 1 + text_width(hints[i][1]) + 2;
        if (x - col + need > width) {
            break;
        }
        x += term_print(row, x, hints[i][0], TERM_FG(PROC_COLOUR) | TERM_BOLD);
        x += term_print(row, x, " ", TERM_NORMAL);
        x += term_print(row, x, hints[i][1], TERM_NORMAL);
        x += term_print(row, x, "  ", TERM_NORMAL);
    }
}

static void draw_procs(int y, int h, int cols, int interactive)
{
    static const char *const sort_names[] = {"cpu ↓", "mem ↓", "pid ↑"};
    char tasks[24];
    snprintf(tasks, sizeof(tasks), "%d tasks", cur->nprocs);
    const char *labels[] = {sort_names[sort_key], tasks};

    box(y, 0, h, cols, PROC_COLOUR, "proc");
    border_right(y, cols - 1, labels, 2, TERM_FG(TERM_WHITE));

    struct columns c = columns_for(cols - 2);
    int head = y + 1;
    unsigned plain = ATTR_TITLE, sorted = TERM_FG(PROC_COLOUR) | TERM_BOLD;
    term_print(head, c.pid + 2, "PID", sort_key == SORT_PID ? sorted : plain);
    term_print(head, c.name, "NAME", plain);
    term_print(head, c.state, "STATE", plain);
    term_print(head, c.pct + 3, "CPU%", sort_key == SORT_CPU ? sorted : plain);
    term_print(head, c.mem + 4, "MEM", sort_key == SORT_MEM ? sorted : plain);
    term_print(head, c.time + 6, "TIME", plain);

    int rows = h - 3, n = cur->nprocs;
    list_rows = rows;
    if (sel < scroll) {
        scroll = sel;
    }
    if (sel >= scroll + rows) {
        scroll = sel - rows + 1;
    }
    if (scroll > n - rows) {
        scroll = n - rows;
    }
    if (scroll < 0) {
        scroll = 0;
    }
    for (int i = 0; i < rows && scroll + i < n; i++) {
        draw_proc_row(head + 1 + i, &c, &cur->procs[scroll + i], interactive && scroll + i == sel);
    }
    if (interactive) {
        draw_hints(y + h - 1, 2, cols - 6);
    }
}

static void draw_kill_dialog(int rows, int cols)
{
    char question[80];
    snprintf(question, sizeof(question), "send %s to %d (%s)?",
             pending_sig == SIGKILL ? "SIGKILL" : "SIGTERM", pending_pid, pending_name);
    int qw = text_width(question);
    int w = qw + 8 < 34 ? 34 : qw + 8;
    int h = 5, x = (cols - w) / 2, y = (rows - h) / 2;
    for (int r = 0; r < h; r++) {
        fill(y + r, x, w, TERM_NORMAL);
    }
    box(y, x, h, w, KILL_COLOUR, "kill");
    term_print(y + 1, x + (w - qw) / 2, question, ATTR_TITLE);
    int col = x + (w - 13) / 2;
    col += term_print(y + 3, col, "y", ATTR_ALERT);
    col += term_print(y + 3, col, " yes    ", TERM_NORMAL);
    col += term_print(y + 3, col, "n", TERM_FG(TERM_GREEN) | TERM_BOLD);
    term_print(y + 3, col, " no", TERM_NORMAL);
}

static void draw_too_small(int rows, int cols)
{
    char text[64];
    snprintf(text, sizeof(text), "terminal %dx%d, ptop needs %dx%d", cols, rows, TERM_COLS,
             TERM_ROWS);
    int x = (cols - text_width(text)) / 2;
    put_text(rows / 2, x < 0 ? 0 : x, cols, text, ATTR_WARN);
}

static void draw(int once)
{
    int rows = term_rows(), cols = term_cols();
    term_clear(TERM_NORMAL);
    if (rows < TERM_ROWS || cols < TERM_COLS) {
        draw_too_small(rows, cols);
        return;
    }

    // The CPU panel takes about a third of the height, memory and system a fifth, processes the rest.
    int cores = cur->ncores > 0 ? cur->ncores : 1;
    int cpu_h = rows * 30 / 100 < cores + 4 ? cores + 4 : rows * 30 / 100;
    int mid_h = rows * 20 / 100 < 4 ? 4 : rows * 20 / 100;
    draw_cpu(0, cpu_h, cols);
    draw_mem(cpu_h, mid_h, 0, cols / 2);
    draw_system(cpu_h, mid_h, cols / 2, cols - cols / 2);
    draw_procs(cpu_h + mid_h, rows - cpu_h - mid_h, cols, !once);
    if (pending_sig && !once) {
        draw_kill_dialog(rows, cols);
    }
}

static void move_selection(int by)
{
    if (cur->nprocs == 0) {
        return;
    }
    sel += by;
    if (sel >= cur->nprocs) {
        sel = cur->nprocs - 1;
    }
    if (sel < 0) {
        sel = 0;
    }
    sel_pid = cur->procs[sel].pid;
}

static void ask_kill(int sig)
{
    if (cur->nprocs == 0) {
        return;
    }
    pending_sig = sig;
    pending_pid = cur->procs[sel].pid;
    pending_start = cur->procs[sel].start;
    strncpy(pending_name, cur->procs[sel].name, sizeof(pending_name) - 1);
    pending_name[sizeof(pending_name) - 1] = '\0';
}

// Whether the pid still names the process the dialog asked about, not one that took it since.
static int still_pending(void)
{
    char path[48], buf[READ_BUF];
    snprintf(path, sizeof(path), "/proc/%d/status", pending_pid);
    return read_file(path, buf, sizeof(buf)) > 0 && field_ul(buf, "StartTicks") == pending_start;
}

static void answer_kill(int key)
{
    const char *name = pending_sig == SIGKILL ? "SIGKILL" : "SIGTERM";
    if (key != 'y' && key != 'Y') {
        message[0] = '\0';
    } else if (!still_pending()) {
        set_message("%d (%s) has exited; nothing sent", pending_pid, pending_name);
    } else if (kill(pending_pid, pending_sig) == 0) {
        set_message("sent %s to %d (%s)", name, pending_pid, pending_name);
    } else {
        set_message("kill %d: %s", pending_pid, strerror(errno));
    }
    pending_sig = 0;
}

static void set_sort(int key)
{
    sort_key = (key + SORT_KEYS) % SORT_KEYS;
    resort();
}

// Returns 1 when the key asks to quit.
static int handle_key(int key)
{
    if (pending_sig) {
        if (key != TERM_KEY_RESIZE) {
            answer_kill(key);
        }
        return 0;
    }
    switch (key) {
        case 'q':
        case 'Q':
        case TERM_KEY_ESC:
        case TERM_KEY_EOF:
            return 1;
        case 'c':
            set_sort(SORT_CPU);
            break;
        case 'm':
            set_sort(SORT_MEM);
            break;
        case 'p':
            set_sort(SORT_PID);
            break;
        case TERM_KEY_RIGHT:
            set_sort(sort_key + 1);
            break;
        case TERM_KEY_LEFT:
            set_sort(sort_key - 1);
            break;
        case '+':
        case '=':
            if (refresh_ms + REFRESH_STEP_MS > REFRESH_MAX_MS) {
                set_message("the longest interval is %dms", REFRESH_MAX_MS);
            } else {
                refresh_ms += REFRESH_STEP_MS;
            }
            break;
        case '-':
        case '_':
            if (refresh_ms <= REFRESH_STEP_MS) {
                set_message("the shortest interval is %dms", REFRESH_STEP_MS);
            } else {
                refresh_ms -= REFRESH_STEP_MS;
            }
            break;
        case CTRL('l'):
            term_redraw();
            break;
        case TERM_KEY_UP:
            move_selection(-1);
            break;
        case TERM_KEY_DOWN:
            move_selection(1);
            break;
        case TERM_KEY_PGUP:
            move_selection(-list_rows);
            break;
        case TERM_KEY_PGDN:
            move_selection(list_rows);
            break;
        case TERM_KEY_HOME:
            move_selection(-MAX_PROCS);
            break;
        case TERM_KEY_END:
            move_selection(MAX_PROCS);
            break;
        case 'k':
            ask_kill(SIGTERM);
            break;
        case 'K':
            ask_kill(SIGKILL);
            break;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int once = argc == 2 && strcmp(argv[1], "--once") == 0;
    if (argc > 1 && !once) {
        printf("usage: ptop [--once]\n");
        return 1;
    }

    take_sample();
    usleep(FIRST_GAP_MS * 1000);
    take_sample();

    if (once) {
        draw(1);
        term_dump();
        return 0;
    }

    if (term_open() != 0) {
        return 1;
    }
    // The next sample is due one interval after the last, so a new interval applies at once.
    for (;;) {
        draw(0);
        term_present();
        long left = (long)(cur->at_ms + refresh_ms - term_ms());
        int key = left > 0 ? term_key((int)left) : TERM_KEY_NONE;
        if (key != TERM_KEY_NONE && handle_key(key)) {
            break;
        }
        if ((long)(cur->at_ms + refresh_ms - term_ms()) <= 0) {
            take_sample();
            // Nothing announces a resized window over a serial line, so ask now and then.
            if (cur->at_ms - size_asked >= SIZE_ASK_MS) {
                term_request_size();
                size_asked = cur->at_ms;
            }
        }
    }
    term_close();
    return 0;
}
