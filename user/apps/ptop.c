/*
 * ptop.c - btop-style system monitor: per-core load, memory and a process list.
 *
 * Every figure is a delta between two samples of /proc: per-core busy and idle
 * milliseconds from /proc/stat, interrupt counts from /proc/interrupts, and
 * each process's CpuTime from its status file. `ptop --once` takes two samples,
 * prints one frame as plain text and exits.
 */

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

#define REFRESH_MS   1000
#define FIRST_GAP_MS 300 // between the two samples the first frame is drawn from
#define MESSAGE_MS   3000
#define MAX_CORES    8
#define MAX_PROCS    128
#define HISTORY      20
#define BAR_W        20
#define READ_BUF     1024
#define VALUE_COL    29 // where the text after a bar starts
#define GRAPH_COL    35

#define ATTR_TITLE    (TERM_BG(TERM_BLUE) | TERM_FG(TERM_WHITE) | TERM_BOLD)
#define ATTR_HEAD     (TERM_BG(TERM_GREEN) | TERM_FG(TERM_BLACK))
#define ATTR_SORTED   (TERM_BG(TERM_CYAN) | TERM_FG(TERM_BLACK) | TERM_BOLD)
#define ATTR_SELECTED (TERM_BG(TERM_CYAN) | TERM_FG(TERM_BLACK))
#define ATTR_GRAPH    TERM_FG(TERM_CYAN)
#define ATTR_PROMPT   (TERM_FG(TERM_YELLOW) | TERM_BOLD)

struct proc {
    int pid, ppid;
    char name[32];
    char state[12];
    unsigned long mem_kb;
    unsigned long cpu_ms;
    unsigned long pct10; // tenths of a percent of one core over the last interval
};

struct sample {
    unsigned long at_ms;
    int ncores;
    int core_id[MAX_CORES];
    unsigned long busy[MAX_CORES], idle[MAX_CORES];
    unsigned long irqs, ctxt;
    unsigned long mem_total, mem_free, slab_used, slab_total;
    unsigned long uptime_s;
    int nprocs;
    struct proc procs[MAX_PROCS];
};

enum {
    SORT_CPU,
    SORT_MEM,
    SORT_PID
};

static struct sample samples[2];
static struct sample *cur = &samples[0], *prev = &samples[1];
static int sampled;

static int core_pct[MAX_CORES];
static unsigned char history[MAX_CORES][HISTORY];
static unsigned long irq_rate, ctxt_rate;

static int sort_key = SORT_CPU;
static int sel, sel_pid = -1, top, list_rows;

static int pending_sig, pending_pid;
static char pending_name[32];
static char message[TERM_COLS + 1];
static unsigned long message_until;

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

static unsigned long delta(unsigned long now, unsigned long then)
{
    return now > then ? now - then : 0;
}

static void read_stat(struct sample *s)
{
    char buf[READ_BUF];
    s->ncores = 0;
    s->ctxt = 0;
    if (read_file("/proc/stat", buf, sizeof(buf)) < 0) {
        return;
    }
    for (const char *line = buf; *line; line = next_line(line)) {
        const char *p = line;
        if (strncmp(line, "ctxt ", 5) == 0) {
            p += 5;
            s->ctxt = number(&p);
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
        pr->pct10 = 0;
        field_str(buf, "Name", pr->name, sizeof(pr->name));
        field_str(buf, "State", pr->state, sizeof(pr->state));
    }
    closedir(dir);
}

static void derive_rates(void)
{
    unsigned long dt = delta(cur->at_ms, prev->at_ms);
    if (dt == 0) {
        dt = 1;
    }
    irq_rate = delta(cur->irqs, prev->irqs) * 1000 / dt;
    ctxt_rate = delta(cur->ctxt, prev->ctxt) * 1000 / dt;

    for (int i = 0; i < cur->ncores; i++) {
        unsigned long busy = i < prev->ncores ? delta(cur->busy[i], prev->busy[i]) : 0;
        unsigned long idle = i < prev->ncores ? delta(cur->idle[i], prev->idle[i]) : 0;
        int pct = busy + idle ? (int)(busy * 100 / (busy + idle)) : 0;
        core_pct[i] = pct;
        memmove(history[i], history[i] + 1, HISTORY - 1);
        history[i][HISTORY - 1] = (unsigned char)pct;
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

    // The first sample has nothing to compare against and is never shown.
    if (sampled) {
        derive_rates();
        resort();
    }
    sampled = 1;
}

static void fill_row(int row, unsigned attr)
{
    for (int c = 0; c < TERM_COLS; c++) {
        term_put(row, c, ' ', attr);
    }
}

static unsigned load_attr(unsigned long pct)
{
    return TERM_FG(pct >= 80 ? TERM_RED : pct >= 50 ? TERM_YELLOW : TERM_GREEN);
}

static void draw_bar(int row, unsigned long used, unsigned long total)
{
    unsigned long pct = total ? used * 100 / total : 0;
    int filled = total ? (int)(used * BAR_W / total) : 0;
    term_put(row, 6, '[', TERM_NORMAL);
    for (int i = 0; i < BAR_W; i++) {
        term_put(row, 7 + i, i < filled ? '|' : ' ', load_attr(pct));
    }
    term_put(row, 7 + BAR_W, ']', TERM_NORMAL);
}

static void draw_title(void)
{
    char line[TERM_COLS + 1];
    unsigned long up = cur->uptime_s;
    snprintf(line, sizeof(line),
             " ptop   up %lu:%02lu:%02lu   %d processes   %lu irq/s   %lu switches/s", up / 3600,
             up / 60 % 60, up % 60, cur->nprocs, irq_rate, ctxt_rate);
    fill_row(0, ATTR_TITLE);
    term_print(0, 0, line, ATTR_TITLE);
}

static void draw_cpus(void)
{
    static const char *const levels[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    char text[16];
    for (int i = 0; i < cur->ncores; i++) {
        int row = 1 + i;
        snprintf(text, sizeof(text), " cpu%d", cur->core_id[i]);
        term_print(row, 0, text, TERM_BOLD);
        draw_bar(row, (unsigned long)core_pct[i], 100);
        snprintf(text, sizeof(text), "%3d%%", core_pct[i]);
        term_print(row, VALUE_COL, text, TERM_NORMAL);
        for (int h = 0; h < HISTORY; h++) {
            term_print(row, GRAPH_COL + h, levels[(history[i][h] * 8 + 99) / 100], ATTR_GRAPH);
        }
    }
}

static void draw_mem(int row)
{
    char text[TERM_COLS + 1];
    unsigned long used = delta(cur->mem_total, cur->mem_free);
    term_print(row, 0, " mem", TERM_BOLD);
    draw_bar(row, used, cur->mem_total);
    snprintf(text, sizeof(text), "%3lu%%  %lu / %lu MB   slab %lu / %lu kB",
             cur->mem_total ? used * 100 / cur->mem_total : 0, used / 1024, cur->mem_total / 1024,
             cur->slab_used, cur->slab_total);
    term_print(row, VALUE_COL, text, TERM_NORMAL);
}

#define ROW_FORMAT " %5s %5s  %-8s %5s %8s %9s  %s"

static void draw_head(int row)
{
    static const struct {
        int col, width;
        const char *label;
    } sorted[] = {
        [SORT_CPU] = {23, 5, "CPU%"},
        [SORT_MEM] = {29, 8, "MEM"},
        [SORT_PID] = {1, 5, "PID"},
    };
    char text[TERM_COLS + 1];
    fill_row(row, ATTR_HEAD);
    snprintf(text, sizeof(text), ROW_FORMAT, "PID", "PPID", "STATE", "CPU%", "MEM", "TIME", "NAME");
    term_print(row, 0, text, ATTR_HEAD);
    snprintf(text, sizeof(text), "%*s", sorted[sort_key].width, sorted[sort_key].label);
    term_print(row, sorted[sort_key].col, text, ATTR_SORTED);
}

static void draw_proc(int row, const struct proc *p, unsigned attr)
{
    char pid[8], ppid[8], cpu[8], mem[16], time[16], text[TERM_COLS + 1];
    snprintf(pid, sizeof(pid), "%d", p->pid);
    snprintf(ppid, sizeof(ppid), "%d", p->ppid);
    snprintf(cpu, sizeof(cpu), "%lu.%lu", p->pct10 / 10, p->pct10 % 10);
    if (p->mem_kb < 10240) {
        snprintf(mem, sizeof(mem), "%lu K", p->mem_kb);
    } else {
        snprintf(mem, sizeof(mem), "%lu.%lu M", p->mem_kb / 1024, p->mem_kb % 1024 * 10 / 1024);
    }
    snprintf(time, sizeof(time), "%lu:%02lu.%02lu", p->cpu_ms / 60000, p->cpu_ms / 1000 % 60,
             p->cpu_ms % 1000 / 10);
    snprintf(text, sizeof(text), ROW_FORMAT, pid, ppid, p->state, cpu, mem, time, p->name);
    if (attr != TERM_NORMAL) {
        fill_row(row, attr);
    }
    term_print(row, 0, text, attr);
}

static void draw_list(int first, int rows, int highlight)
{
    int n = cur->nprocs;
    list_rows = rows;
    if (sel < top) {
        top = sel;
    }
    if (sel >= top + rows) {
        top = sel - rows + 1;
    }
    if (top > n - rows) {
        top = n - rows;
    }
    if (top < 0) {
        top = 0;
    }
    for (int i = 0; i < rows && top + i < n; i++) {
        int selected = highlight && top + i == sel;
        draw_proc(first + i, &cur->procs[top + i], selected ? ATTR_SELECTED : TERM_NORMAL);
    }
}

static void draw_status(void)
{
    static const char *const help[][2] = {
        {"q", " quit  "},    {"c m p", " sort  "}, {"↑↓ PgUp PgDn", " select  "},
        {"k", " SIGTERM  "}, {"K", " SIGKILL"},
    };
    int row = TERM_ROWS - 1;
    if (pending_sig) {
        char text[TERM_COLS + 1];
        snprintf(text, sizeof(text), " send %s to %d (%s)? y/n",
                 pending_sig == SIGKILL ? "SIGKILL" : "SIGTERM", pending_pid, pending_name);
        term_print(row, 0, text, ATTR_PROMPT);
        return;
    }
    if (message[0] && term_ms() < message_until) {
        term_print(row, 0, message, ATTR_PROMPT);
        return;
    }
    int col = 1;
    for (size_t i = 0; i < sizeof(help) / sizeof(help[0]); i++) {
        col += term_print(row, col, help[i][0], TERM_BOLD);
        col += term_print(row, col, help[i][1], TERM_NORMAL);
    }
}

static void draw(int once)
{
    term_clear(TERM_NORMAL);
    draw_title();
    draw_cpus();
    int row = 1 + cur->ncores;
    draw_mem(row++);
    draw_head(row++);
    if (once) {
        draw_list(row, TERM_ROWS - row, 0);
    } else {
        draw_list(row, TERM_ROWS - 1 - row, 1);
        draw_status();
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
    strncpy(pending_name, cur->procs[sel].name, sizeof(pending_name) - 1);
    pending_name[sizeof(pending_name) - 1] = '\0';
}

static void answer_kill(int key)
{
    const char *name = pending_sig == SIGKILL ? "SIGKILL" : "SIGTERM";
    if (key != 'y' && key != 'Y') {
        message[0] = '\0';
    } else if (kill(pending_pid, pending_sig) == 0) {
        snprintf(message, sizeof(message), " sent %s to %d (%s)", name, pending_pid, pending_name);
    } else {
        snprintf(message, sizeof(message), " kill %d: %s", pending_pid, strerror(errno));
    }
    message_until = term_ms() + MESSAGE_MS;
    pending_sig = 0;
}

// Returns 1 when the key asks to quit.
static int handle_key(int key)
{
    if (pending_sig) {
        answer_kill(key);
        return 0;
    }
    switch (key) {
        case 'q':
        case 'Q':
        case TERM_KEY_ESC:
        case TERM_KEY_EOF:
            return 1;
        case 'c':
            sort_key = SORT_CPU;
            resort();
            break;
        case 'm':
            sort_key = SORT_MEM;
            resort();
            break;
        case 'p':
            sort_key = SORT_PID;
            resort();
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
    unsigned long next = term_ms() + REFRESH_MS;
    for (;;) {
        draw(0);
        term_present();
        long left = (long)(next - term_ms());
        int key = left > 0 ? term_key((int)left) : TERM_KEY_NONE;
        if (key != TERM_KEY_NONE && handle_key(key)) {
            break;
        }
        if ((long)(next - term_ms()) <= 0) {
            take_sample();
            next = term_ms() + REFRESH_MS;
        }
    }
    term_close();
    return 0;
}
