/*
 * ptop.c - Process monitor utility for the Perspicua kernel.
 *
 * Reads /proc to display running processes and system memory info,
 * refreshing every second. Exits after a fixed number of iterations.
 */

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "types.h"
#include "wait.h"
#include "stdlib.h"

/* ── tunables ────────────────────────────────────────────────────── */
#define REFRESH_INTERVAL_MS 1000
#define MAX_ITERATIONS      10 /* exit after 10 refreshes (~10s) */
#define MAX_PROCS           300
#define READ_BUF_SIZE       512
#define PATH_BUF_SIZE       64

/* ── ANSI helpers ────────────────────────────────────────────────── */
#define ESC "\033["

/* Cursor / screen */
#define ANSI_CLEAR       ESC "2J" ESC "H" /* clear + home      */
#define ANSI_HOME        ESC "H"
#define ANSI_HIDE_CURSOR ESC "?25l"
#define ANSI_SHOW_CURSOR ESC "?25h"
#define ANSI_MOVE(r, c)  ESC #r ";" #c "H" /* literal only      */

/* Colours (256-colour palette) */
#define C_RESET ESC "0m"
#define C_BOLD  ESC "1m"

/* Greens */
#define C_GREEN_BRIGHT ESC "38;5;82m"
#define C_GREEN_DIM    ESC "38;5;28m"
#define C_GREEN_MID    ESC "38;5;40m"

/* Header bar */
#define C_HEADER_BG    ESC "48;5;22m" ESC "38;5;46m" ESC "1m"
#define C_HEADER_RESET ESC "0m"

/* Column headers */
#define C_COL_HDR ESC "38;5;34m" ESC "1m"

static char **global_envp;

/* Alternating row colours */
#define C_ROW_ODD  ESC "38;5;82m"
#define C_ROW_EVEN ESC "38;5;40m"

/* Accent / labels */
#define C_LABEL      ESC "38;5;34m" ESC "1m"
#define C_VALUE      ESC "38;5;118m"
#define C_BAR_FILLED ESC "38;5;46m"
#define C_BAR_EMPTY  ESC "38;5;238m"
#define C_FOOTER     ESC "38;5;240m"
#define C_WARN       ESC "38;5;214m" ESC "1m"
#define C_ZOMBIE     ESC "38;5;196m"

/* ── tiny string helpers (no libc dependency beyond what you listed) */
static size_t ptop_strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void ptop_write(const char *s)
{
    sys_write(1, s, ptop_strlen(s));
}

/* Write exactly `width` chars of `s`, padding with spaces on the right. */
static void ptop_write_padded(const char *s, int width)
{
    char buf[128];
    int slen = (int)ptop_strlen(s);
    int i;
    for (i = 0; i < width && i < 127; i++)
        buf[i] = (i < slen) ? s[i] : ' ';
    buf[i] = '\0';
    sys_write(1, buf, i);
}

/* Right-align a string in a field of `width`. */
static void ptop_write_right(const char *s, int width)
{
    char buf[64];
    int slen = (int)ptop_strlen(s);
    int pad = width - slen;
    int i = 0, j;
    for (j = 0; j < pad && i < 63; j++)
        buf[i++] = ' ';
    for (j = 0; j < slen && i < 63; j++)
        buf[i++] = s[j];
    buf[i] = '\0';
    sys_write(1, buf, i);
}

/* ── memory bar renderer ─────────────────────────────────────────── */
static void draw_bar(unsigned long used, unsigned long total, int bar_width)
{
    if (total == 0) {
        ptop_write("[" C_BAR_EMPTY "----------" C_RESET "]");
        return;
    }

    int filled = (int)((unsigned long long)used * bar_width / total);
    if (filled > bar_width)
        filled = bar_width;

    char bar[128];
    int pos = 0;
    bar[pos++] = '[';

    /* ANSI codes inline into a small temp write sequence */
    sys_write(1, bar, pos);
    pos = 0;

    ptop_write(C_BAR_FILLED);
    int i;
    for (i = 0; i < filled; i++)
        sys_write(1, "|", 1);
    ptop_write(C_BAR_EMPTY);
    for (; i < bar_width; i++)
        sys_write(1, ".", 1);
    ptop_write(C_RESET);
    sys_write(1, "]", 1);
}

/* ── /proc parsing helpers ───────────────────────────────────────── */

/*
 * Read a small file from /proc entirely into buf (null-terminated).
 * Returns number of bytes read, or -1 on error.
 */
static int read_proc_file(const char *path, char *buf, size_t bufsz)
{
    int fd = sys_open(path, VFS_O_RDONLY);
    if (fd < 0)
        return -1;

    int total = 0;
    int n;
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

/*
 * Extract the value of a "Key:   value\n" line from a /proc/pid/status buffer.
 * Writes result into `out` (null-terminated). Returns 1 on success.
 */
static int parse_status_field(const char *buf, const char *key, char *out, size_t outsz)
{
    const char *p = buf;
    size_t klen = ptop_strlen(key);

    while (*p) {
        /* find the key at the start of a line */
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            p += klen + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            size_t i = 0;
            while (*p && *p != '\n' && i < outsz - 1)
                out[i++] = *p++;
            out[i] = '\0';
            return 1;
        }
        /* skip to next line */
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }
    return 0;
}

/*
 * Simple decimal string to unsigned long.
 */
static unsigned long ptop_atoul(const char *s)
{
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

/* ── process record ──────────────────────────────────────────────── */
struct proc_info {
    int pid;
    char name[64];
    char state[16];
    unsigned long vmsize_kb;
    int ppid;
};

/* ── terminal width (fixed; no ioctl available) ──────────────────── */
#define TERM_COLS 80

/* ── main draw loop ──────────────────────────────────────────────── */
static void draw_header(int iteration, int total_procs)
{
    char tmp[128];

    /* ── title bar ── */
    ptop_write(C_HEADER_BG);
    ptop_write("  ptop  —  Perspicua Process Monitor  ");
    snprintf(tmp, sizeof(tmp), "iter %d/%d  procs: %d", iteration, MAX_ITERATIONS, total_procs);
    ptop_write_right(tmp, TERM_COLS - 38 - 2);
    ptop_write("  ");
    ptop_write(C_HEADER_RESET "\n");
}

static void draw_meminfo(void)
{
    char buf[READ_BUF_SIZE];
    char val[32];

    if (read_proc_file("/proc/meminfo", buf, sizeof(buf)) < 0) {
        ptop_write(C_WARN "  [meminfo unavailable]\n" C_RESET);
        return;
    }

    unsigned long mem_total = 0, mem_free = 0;
    unsigned long slab_used = 0, slab_total = 0;

    if (parse_status_field(buf, "MemTotal", val, sizeof(val)))
        mem_total = ptop_atoul(val);
    if (parse_status_field(buf, "MemFree", val, sizeof(val)))
        mem_free = ptop_atoul(val);
    if (parse_status_field(buf, "SlabUsed", val, sizeof(val)))
        slab_used = ptop_atoul(val);
    if (parse_status_field(buf, "SlabTotal", val, sizeof(val)))
        slab_total = ptop_atoul(val);

    unsigned long mem_used = mem_total - mem_free;

    /* ── Mem bar ── */
    ptop_write(C_LABEL "  Mem  " C_RESET);
    draw_bar(mem_used, mem_total, 30);
    snprintf(val, sizeof(val), " %lu", mem_used);
    ptop_write(C_VALUE);
    ptop_write(val);
    ptop_write(C_LABEL " / ");
    snprintf(val, sizeof(val), "%lu kB\n", mem_total);
    ptop_write(C_VALUE);
    ptop_write(val);
    ptop_write(C_RESET);

    /* ── Slab bar ── */
    ptop_write(C_LABEL "  Slab " C_RESET);
    draw_bar(slab_used, slab_total, 30);
    snprintf(val, sizeof(val), " %lu", slab_used);
    ptop_write(C_VALUE);
    ptop_write(val);
    ptop_write(C_LABEL " / ");
    snprintf(val, sizeof(val), "%lu kB\n", slab_total);
    ptop_write(C_VALUE);
    ptop_write(val);
    ptop_write(C_RESET);

    ptop_write("\n");
}

static void draw_proc_table(struct proc_info *procs, int count)
{
    char tmp[64];

    /* ── column headers ── */
    ptop_write(C_COL_HDR);
    ptop_write_padded("  PID", 6);
    ptop_write_padded("  PPID", 7);
    ptop_write_padded("  STATE", 10);
    ptop_write_padded("  VMSIZE", 10);
    ptop_write_padded("  NAME", TERM_COLS - 6 - 7 - 10 - 10);
    ptop_write(C_RESET "\n");

    /* thin separator line */
    ptop_write(C_GREEN_DIM);
    for (int i = 0; i < TERM_COLS; i++)
        sys_write(1, "─", 3); /* UTF-8 box char */
    ptop_write(C_RESET "\n");

    for (int i = 0; i < count; i++) {
        struct proc_info *p = &procs[i];

        /* alternating row colour; zombies always red */
        int is_zombie = (strcmp(p->state, "zombie") == 0);
        if (is_zombie)
            ptop_write(C_ZOMBIE);
        else if (i % 2 == 0)
            ptop_write(C_ROW_EVEN);
        else
            ptop_write(C_ROW_ODD);

        /* PID */
        snprintf(tmp, sizeof(tmp), "%d", p->pid);
        ptop_write_right(tmp, 5);
        sys_write(1, "  ", 2);

        /* PPID */
        snprintf(tmp, sizeof(tmp), "%d", p->ppid);
        ptop_write_right(tmp, 5);
        sys_write(1, "  ", 2);

        /* STATE (padded) */
        ptop_write_padded(p->state, 10);

        /* VMSIZE */
        snprintf(tmp, sizeof(tmp), "%lu kB", p->vmsize_kb);
        ptop_write_right(tmp, 9);
        sys_write(1, "  ", 2);

        /* NAME */
        ptop_write_padded(p->name, TERM_COLS - 6 - 7 - 10 - 11);

        ptop_write(C_RESET "\n");
    }
}

static void draw_footer(void)
{
    ptop_write("\n");
    ptop_write(C_GREEN_DIM);
    for (int i = 0; i < TERM_COLS; i++)
        sys_write(1, "─", 3);
    ptop_write(C_RESET "\n");
    ptop_write(C_FOOTER "  ptop  |  refreshes every 1s  |  exits automatically after ");
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%d", MAX_ITERATIONS);
    ptop_write(tmp);
    ptop_write(" iterations" C_RESET "\n");
}

/* ── enumerate /proc ─────────────────────────────────────────────── */
static int collect_procs(struct proc_info *out, int max)
{
    struct vfs_dirent dent;
    char status_path[PATH_BUF_SIZE];
    char status_buf[READ_BUF_SIZE];
    char field[64];
    int count = 0;

    int fd = sys_open("/proc", VFS_O_RDONLY);
    if (fd < 0)
        return 0;

    int scanned = 0;
    const int MAX_SCAN = MAX_PROCS * 2; /* Safety scan limit */

    while (count < max && scanned < MAX_SCAN) {
        int n = sys_getdents(fd, &dent, sizeof(dent));
        if (n <= 0)
            break;

        scanned++;

        /* skip "." ".." and non-numeric names (version, meminfo, etc.) */
        const char *nm = dent.name;
        if (nm[0] == '.' || nm[0] < '0' || nm[0] > '9')
            continue;

        int pid = 0;
        const char *p = nm;
        while (*p >= '0' && *p <= '9')
            pid = pid * 10 + (*p++ - '0');
        if (*p != '\0')
            continue;

        struct proc_info *info = &out[count];
        info->pid = pid;
        info->ppid = 0;
        info->vmsize_kb = 0;
        info->name[0] = '\0';
        info->state[0] = '\0';

        snprintf(status_path, sizeof(status_path), "/proc/%d/status", pid);
        if (read_proc_file(status_path, status_buf, sizeof(status_buf)) > 0) {
            if (parse_status_field(status_buf, "Name", field, sizeof(field)))
                strncpy(info->name, field, sizeof(info->name) - 1);
            if (parse_status_field(status_buf, "State", field, sizeof(field)))
                strncpy(info->state, field, sizeof(info->state) - 1);
            if (parse_status_field(status_buf, "PPid", field, sizeof(field)))
                info->ppid = (int)ptop_atoul(field);
            if (parse_status_field(status_buf, "VmSize", field, sizeof(field)))
                info->vmsize_kb = ptop_atoul(field);
        }

        /* fallback name from cmdline if status gave nothing */
        if (info->name[0] == '\0') {
            snprintf(status_path, sizeof(status_path), "/proc/%d/cmdline", pid);
            if (read_proc_file(status_path, status_buf, sizeof(status_buf)) > 0)
                strncpy(info->name, status_buf, sizeof(info->name) - 1);
        }

        if (info->name[0] == '\0')
            strncpy(info->name, "?", sizeof(info->name) - 1);
        if (info->state[0] == '\0')
            strncpy(info->state, "unknown", sizeof(info->state) - 1);

        count++;
    }

    sys_close(fd);
    return count;
}

/* ── demo launcher ───────────────────────────────────────────────── */
/*
 * Fork demo as a background child. The child calls into demo_main()
 * which is defined in demo.c — just rename demo.c's main() to
 * demo_main() and declare it here, or link both translation units
 * together and use the approach below.
 *
 * If you prefer keeping them as separate binaries, replace the
 * demo_main() call with sys_exec("/bin/demo") instead.
 */
/* ── demo launcher ───────────────────────────────────────────────── */
#define DEMO_PATH "/bin/stress.elf" /* adjust to wherever demo.elf is mounted */

static void launch_demo(void)
{
    int pid = sys_fork();
    if (pid < 0) {
        ptop_write(C_WARN "ptop: failed to fork demo\n" C_RESET);
        return;
    }
    if (pid == 0) {
        /* child — replace image with demo binary */
        char *argv[] = {DEMO_PATH, NULL};
        sys_exec(DEMO_PATH, argv, global_envp);
        /* if exec returns, the binary wasn't found */
        ptop_write(C_WARN "ptop: exec(" DEMO_PATH ") failed\n" C_RESET);
        sys_exit(1);
    }
    /* parent — demo runs in background, we don't wait for it here, but we'll reap it */
}

/* ── entry point ─────────────────────────────────────────────────── */
int main(int argc, char **argv, char **envp)
{
    global_envp = envp;
    (void)argc;
    (void)argv;
    struct proc_info *procs = malloc(sizeof(struct proc_info) * MAX_PROCS);
    if (!procs) {
        printf("ptop: failed to allocate memory for process info\n");
        sys_exit(1);
    }

    /* launch demo workload before first draw so processes are visible
       immediately on iteration 1 */
    launch_demo();

    ptop_write(ANSI_HIDE_CURSOR);

    for (int iter = 1; iter <= MAX_ITERATIONS; iter++) {
        int count = collect_procs(procs, MAX_PROCS);

        ptop_write(ANSI_CLEAR);

        draw_header(iter, count);
        ptop_write("\n");
        draw_meminfo();
        draw_proc_table(procs, count);
        draw_footer();

        /* Reap any children that have exited (non-blocking) */
        int reaped;
        int reap_count = 0;
        /* Limit reaping to avoid infinite loop if sys_waitpid misbehaves */
        while (reap_count < MAX_PROCS && (reaped = sys_waitpid(-1, NULL, WNOHANG)) > 0) {
            reap_count++;
        }

        if (iter < MAX_ITERATIONS)
            sys_sleep(REFRESH_INTERVAL_MS);
    }

    /* Final cleanup: reap the demo process if it finished */
    int final_reap_count = 0;
    while (final_reap_count < MAX_PROCS && sys_waitpid(-1, NULL, WNOHANG) > 0) {
        final_reap_count++;
    }

    ptop_write(ANSI_SHOW_CURSOR);
    ptop_write(C_RESET "\n");

    free(procs);
    sys_exit(0);
    return 0;
}
