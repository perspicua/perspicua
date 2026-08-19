/*
 * ptop.c - Minimal btop-style system monitor for Perspicua.
 *
 * Monochrome, box-drawn panels: per-core CPU activity, memory, and a process
 * table sorted by memory. Refreshes once a second and runs until you press 'q'.
 *
 * There is no per-process/per-core CPU-time accounting in the kernel, so the CPU
 * panel shows context-switch RATE per core (bars auto-scaled to the busiest
 * core) rather than a true utilization percentage.
 */

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define REFRESH_MS   1000
#define POLL_MS      100
#define INNER_W      76 /* columns between the │ borders */
#define BAR_W        22
#define MAX_CORES    16
#define MAX_PROCS    128
#define PROC_ROWS    14 /* process rows shown before truncating */
#define READ_BUF     1024
#define PATH_BUF     64

#define ESC              "\033["
#define ANSI_CLEAR       ESC "2J" ESC "H"
#define ANSI_HIDE_CURSOR ESC "?25l"
#define ANSI_SHOW_CURSOR ESC "?25h"

/* ── tiny output helpers ─────────────────────────────────────────── */
static void out(const char *s)
{
    sys_write(1, s, strlen(s));
}

static void box_hchars(int n)
{
    for (int i = 0; i < n; i++)
        sys_write(1, "─", 3); /* ─ */
}

static void box_top(const char *label)
{
    sys_write(1, "┌", 3); /* ┌ */
    box_hchars(1);
    sys_write(1, " ", 1);
    out(label);
    sys_write(1, " ", 1);
    int dashes = INNER_W - ((int)strlen(label) + 3);
    if (dashes < 0)
        dashes = 0;
    box_hchars(dashes);
    sys_write(1, "┐", 3); /* ┐ */
    sys_write(1, "\n", 1);
}

static void box_bottom(void)
{
    sys_write(1, "└", 3); /* └ */
    box_hchars(INNER_W);
    sys_write(1, "┘", 3); /* ┘ */
    sys_write(1, "\n", 1);
}

/* Draw one ASCII content line inside the box, padded to the inner width. */
static void box_line(const char *s)
{
    int len = (int)strlen(s);
    if (len > INNER_W)
        len = INNER_W;
    sys_write(1, "│", 3); /* │ */
    sys_write(1, s, len);
    for (int i = len; i < INNER_W; i++)
        sys_write(1, " ", 1);
    sys_write(1, "│", 3);
    sys_write(1, "\n", 1);
}

static void make_bar(char *out_buf, int width, unsigned long used, unsigned long total)
{
    int filled = total ? (int)((unsigned long long)used * width / total) : 0;
    if (filled > width)
        filled = width;
    int p = 0;
    out_buf[p++] = '[';
    for (int i = 0; i < width; i++)
        out_buf[p++] = (i < filled) ? '|' : ' ';
    out_buf[p++] = ']';
    out_buf[p] = '\0';
}

/* ── /proc parsing ───────────────────────────────────────────────── */
static int read_proc_file(const char *path, char *buf, size_t bufsz)
{
    int fd = sys_open(path, VFS_O_RDONLY);
    if (fd < 0)
        return -1;
    int total = 0, n;
    while ((size_t)total < bufsz - 1) {
        n = sys_read(fd, buf + total, bufsz - 1 - total);
        if (n <= 0)
            break;
        total += n;
    }
    buf[total] = '\0';
    sys_close(fd);
    return total;
}

static unsigned long to_ul(const char *s)
{
    unsigned long v = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned long)(*s++ - '0');
    return v;
}

/* Copy the value of a "Key:\tvalue\n" line into out. Returns 1 on success. */
static int status_field(const char *buf, const char *key, char *out_buf, size_t outsz)
{
    const char *p = buf;
    size_t klen = strlen(key);
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            p += klen + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            size_t i = 0;
            while (*p && *p != '\n' && i < outsz - 1)
                out_buf[i++] = *p++;
            out_buf[i] = '\0';
            return 1;
        }
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }
    return 0;
}

/* ── CPU: per-core context-switch counters from /proc/schedstat ───── */
static int read_core_ctx(unsigned long *ctx, int max)
{
    char buf[READ_BUF];
    if (read_proc_file("/proc/schedstat", buf, sizeof(buf)) < 0)
        return 0;

    int ncores = 0;
    const char *p = buf;
    /* Skip the header line ("cpu  context_switches  idle_entries"). */
    while (*p && *p != '\n')
        p++;
    if (*p == '\n')
        p++;

    while (*p && ncores < max) {
        /* line: "<core>   <ctx>   <idle>" */
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p < '0' || *p > '9')
            break;
        while (*p >= '0' && *p <= '9') /* core index */
            p++;
        while (*p == ' ' || *p == '\t')
            p++;
        ctx[ncores++] = to_ul(p);
        while (*p && *p != '\n') /* to end of line */
            p++;
        if (*p == '\n')
            p++;
    }
    return ncores;
}

static void draw_cpu(const unsigned long *rate, int ncores)
{
    unsigned long maxr = 1;
    for (int i = 0; i < ncores; i++)
        if (rate[i] > maxr)
            maxr = rate[i];

    box_top("cpu (ctx-switch rate per core)");
    char line[160], bar[BAR_W + 4];
    for (int i = 0; i < ncores; i++) {
        make_bar(bar, BAR_W, rate[i], maxr);
        snprintf(line, sizeof(line), " Core%-2d %s %7lu ctx/s", i, bar, rate[i]);
        box_line(line);
    }
    box_bottom();
}

/* ── memory ──────────────────────────────────────────────────────── */
static void draw_mem(void)
{
    char buf[READ_BUF], val[32], line[160], bar[BAR_W + 4];
    unsigned long total = 0, freem = 0, su = 0, st = 0;

    if (read_proc_file("/proc/meminfo", buf, sizeof(buf)) > 0) {
        if (status_field(buf, "MemTotal", val, sizeof(val)))
            total = to_ul(val);
        if (status_field(buf, "MemFree", val, sizeof(val)))
            freem = to_ul(val);
        if (status_field(buf, "SlabUsed", val, sizeof(val)))
            su = to_ul(val);
        if (status_field(buf, "SlabTotal", val, sizeof(val)))
            st = to_ul(val);
    }
    unsigned long used = (total >= freem) ? total - freem : 0;
    unsigned long pct10 = total ? used * 1000 / total : 0; /* tenths of a percent */

    box_top("mem");
    make_bar(bar, BAR_W, used, total);
    snprintf(line, sizeof(line), " Used %s %8lu / %lu kB  %lu.%lu%%", bar, used, total, pct10 / 10,
             pct10 % 10);
    box_line(line);
    make_bar(bar, BAR_W, su, st);
    snprintf(line, sizeof(line), " Slab %s %8lu / %lu kB", bar, su, st);
    box_line(line);
    box_bottom();
}

/* ── process table ───────────────────────────────────────────────── */
struct proc_info {
    int pid, ppid;
    char name[48];
    char state[12];
    unsigned long vmsize_kb;
};

static int collect_procs(struct proc_info *out_arr, int max)
{
    int fd = sys_open("/proc", VFS_O_RDONLY);
    if (fd < 0)
        return 0;

    struct vfs_dirent dent;
    char path[PATH_BUF], sbuf[READ_BUF], field[64];
    int count = 0;

    while (count < max && sys_getdents(fd, &dent, sizeof(dent)) > 0) {
        const char *nm = dent.name;
        if (nm[0] < '0' || nm[0] > '9')
            continue;
        int pid = 0;
        const char *p = nm;
        while (*p >= '0' && *p <= '9')
            pid = pid * 10 + (*p++ - '0');
        if (*p != '\0')
            continue;

        struct proc_info *info = &out_arr[count];
        info->pid = pid;
        info->ppid = 0;
        info->vmsize_kb = 0;
        strcpy(info->name, "?");
        strcpy(info->state, "?");

        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        if (read_proc_file(path, sbuf, sizeof(sbuf)) > 0) {
            if (status_field(sbuf, "Name", field, sizeof(field)))
                strncpy(info->name, field, sizeof(info->name) - 1);
            if (status_field(sbuf, "State", field, sizeof(field)))
                strncpy(info->state, field, sizeof(info->state) - 1);
            if (status_field(sbuf, "PPid", field, sizeof(field)))
                info->ppid = (int)to_ul(field);
            if (status_field(sbuf, "VmSize", field, sizeof(field)))
                info->vmsize_kb = to_ul(field);
        }
        count++;
    }
    sys_close(fd);
    return count;
}

/* Sort by memory descending so the heaviest tasks sit at the top, like btop. */
static void sort_by_mem(struct proc_info *a, int n)
{
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (a[j].vmsize_kb < a[j + 1].vmsize_kb) {
                struct proc_info t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
            }
        }
    }
}

static void draw_procs(struct proc_info *procs, int count)
{
    char label[64], line[160];
    snprintf(label, sizeof(label), "proc (%d tasks, by mem)", count);
    box_top(label);
    box_line("   PID  PPID  STATE     MEM        NAME");

    int shown = count < PROC_ROWS ? count : PROC_ROWS;
    for (int i = 0; i < shown; i++) {
        struct proc_info *p = &procs[i];
        char mem[24];
        snprintf(mem, sizeof(mem), "%lu kB", p->vmsize_kb);
        snprintf(line, sizeof(line), " %5d %5d  %-9s %-9s  %s", p->pid, p->ppid, p->state, mem,
                 p->name);
        box_line(line);
    }
    if (count > shown) {
        snprintf(line, sizeof(line), " ... %d more", count - shown);
        box_line(line);
    }
    box_bottom();
}

/* ── input: non-blocking check for a quit key ────────────────────── */
static int stdin_flags_saved;

static void set_nonblock(void)
{
    stdin_flags_saved = sys_fcntl(0, VFS_F_GETFL, 0);
    if (stdin_flags_saved >= 0)
        sys_fcntl(0, VFS_F_SETFL, stdin_flags_saved | VFS_O_NONBLOCK);
}

static void restore_blocking(void)
{
    if (stdin_flags_saved >= 0)
        sys_fcntl(0, VFS_F_SETFL, stdin_flags_saved);
}

/* Sleep up to REFRESH_MS, polling for 'q'/ESC. Returns 1 if quit requested. */
static int wait_or_quit(void)
{
    for (int elapsed = 0; elapsed < REFRESH_MS; elapsed += POLL_MS) {
        char c;
        while (sys_read(0, &c, 1) == 1) {
            if (c == 'q' || c == 'Q' || c == 27)
                return 1;
        }
        sys_sleep(POLL_MS);
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    struct proc_info *procs = malloc(sizeof(struct proc_info) * MAX_PROCS);
    if (!procs) {
        printf("ptop: out of memory\n");
        return 1;
    }

    unsigned long prev_ctx[MAX_CORES] = {0};
    unsigned long rate[MAX_CORES] = {0};
    int ncores = read_core_ctx(prev_ctx, MAX_CORES); /* prime the counters */

    set_nonblock();
    out(ANSI_HIDE_CURSOR);

    for (;;) {
        unsigned long cur_ctx[MAX_CORES] = {0};
        int n = read_core_ctx(cur_ctx, MAX_CORES);
        if (n > ncores)
            ncores = n;
        for (int i = 0; i < ncores; i++) {
            rate[i] = (cur_ctx[i] >= prev_ctx[i]) ? cur_ctx[i] - prev_ctx[i] : 0;
            prev_ctx[i] = cur_ctx[i];
        }

        int count = collect_procs(procs, MAX_PROCS);
        sort_by_mem(procs, count);

        out(ANSI_CLEAR);
        draw_cpu(rate, ncores);
        draw_mem();
        draw_procs(procs, count);
        out(" q quit   refresh 1s   sort: mem\n");

        if (wait_or_quit())
            break;
    }

    out(ANSI_SHOW_CURSOR);
    restore_blocking();
    free(procs);
    return 0;
}
