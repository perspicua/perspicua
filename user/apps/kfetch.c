/*
 * kfetch.c - A neofetch-style summary beside the logo; piped, plain "Label: value" lines.
 */

#include <stdarg.h>
#include <stddef.h>

#include "dirent.h"
#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/stat.h"
#include "term.h"
#include "unistd.h"

#define TEXT_MAX  2048
#define MAX_LINES 20
#define VALUE_MAX 96
#define SCREEN_W  80
#define LOGO_W    35
#define GAP       3
#define LABEL_W   10

#define RESET "\033[0m"
#define LABEL "\033[1;95m"

static const char *const logo[] = {
    "        .:-=++++++=-:.",
    "     .:+*#*+==---=+***+-.",
    "   .-*#+-.    .+=.::.:+#*-.",
    "  :*%+.        -::*##=..+%*:",
    " :##-  . :-------:::=##+ -##:",
    ".*#- ... =%##****##*-.+*: -#*.",
    ":#*. ... =##=    .+##-   ..*#:",
    "-#*..... =##= ....*##: ... +#-",
    ":#*. ... =##*****##*- ... .*#:",
    " +%=  .. =##+:-:::.  .... =%+",
    " .*%=. . =##=      ....  =%*.",
    "  .=#*-. -##= .....   .-*#=.",
    "    :+##=+##= .   ..-=*##+::",
    "      .-+*##= .++****+*#####+.",
    "          .:: .==-:.. =######*-",
    "                    ...+#######+.",
    "                        :*#######-.",
    "                         .=*******=",
};

#define LOGO_LINES ((int)(sizeof(logo) / sizeof(logo[0])))

struct line {
    const char *label;
    char value[VALUE_MAX];
};

static struct line lines[MAX_LINES];
static int nlines;

static void add(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void add(const char *label, const char *fmt, ...)
{
    if (nlines == MAX_LINES) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lines[nlines].value, VALUE_MAX, fmt, ap);
    va_end(ap);
    lines[nlines++].label = label;
}

static int read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        buf[0] = '\0';
        return -1;
    }
    int total = 0, n;
    while ((size_t)total < size - 1 && (n = read(fd, buf + total, size - 1 - (size_t)total)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    close(fd);
    return total;
}

// The value of a "key : value" or "key: value" line, trimmed; 0 if there is none.
static int field(const char *text, const char *key, char *out, size_t size)
{
    size_t len = strlen(key);
    for (const char *line = text; line && *line;) {
        if (strncmp(line, key, len) == 0) {
            const char *p = line + len;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == ':') {
                p++;
                while (*p == ' ' || *p == '\t') {
                    p++;
                }
                size_t n = 0;
                while (p[n] && p[n] != '\n' && n < size - 1) {
                    n++;
                }
                memcpy(out, p, n);
                out[n] = '\0';
                return 1;
            }
        }
        line = strchr(line, '\n');
        line = line ? line + 1 : NULL;
    }
    return 0;
}

// The number on a "key value" line, as /proc/clock and /proc/fb write them; -1 if missing.
static long number(const char *text, const char *key)
{
    size_t len = strlen(key);
    for (const char *line = text; line && *line;) {
        if (strncmp(line, key, len) == 0 && line[len] == ' ') {
            return atol(line + len + 1);
        }
        line = strchr(line, '\n');
        line = line ? line + 1 : NULL;
    }
    return -1;
}

// Copies what follows marker in text, up to the first character in stop.
static int after(const char *text, const char *marker, const char *stop, char *out, size_t size)
{
    const char *p = strstr(text, marker);
    if (!p) {
        return 0;
    }
    p += strlen(marker);
    size_t n = 0;
    while (p[n] && !strchr(stop, p[n]) && n < size - 1) {
        n++;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return n > 0;
}

static int stdout_is_terminal(void)
{
    struct stat st;
    return fstat(1, &st) == 0 && S_ISCHR(st.st_mode);
}

static void add_os_host_kernel(char *text)
{
    char ver[48], model[VALUE_MAX];
    read_file("/proc/version", text, TEXT_MAX);
    int have_ver = after(text, "version ", " \n", ver, sizeof(ver));
    if (have_ver) {
        // The release is the tag the build descends from: "0.1.2" of "0.1.2-41-g653bf8a".
        add("OS", "Perspicua %.*s aarch64", (int)strcspn(ver, "-"), ver);
    }

    read_file("/proc/cpuinfo", text, TEXT_MAX);
    if (field(text, "Model", model, sizeof(model))) {
        add("Host", "%s", model);
    }

    if (have_ver) {
        add("Kernel", "%s", ver);
    }
}

static void add_uptime(char *text)
{
    if (read_file("/proc/uptime", text, TEXT_MAX) <= 0) {
        return;
    }
    long s = atol(text);
    long days = s / 86400, hours = s / 3600 % 24, mins = s / 60 % 60;
    if (s < 60) {
        add("Uptime", "%ld sec%s", s, s == 1 ? "" : "s");
        return;
    }
    char out[VALUE_MAX] = "";
    if (days) {
        snprintf(out, sizeof(out), "%ld day%s, ", days, days == 1 ? "" : "s");
    }
    if (days || hours) {
        snprintf(out + strlen(out), sizeof(out) - strlen(out), "%ld hour%s, ", hours,
                 hours == 1 ? "" : "s");
    }
    snprintf(out + strlen(out), sizeof(out) - strlen(out), "%ld min%s", mins, mins == 1 ? "" : "s");
    add("Uptime", "%s", out);
}

static int count_entries(const char *dir, const char *suffix, int numeric)
{
    DIR *d = opendir(dir);
    if (!d) {
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (numeric) {
            // pid 0 is the kernel itself, not a process anyone started.
            n += e->d_name[0] >= '1' && e->d_name[0] <= '9';
        } else if (len > strlen(suffix) && strcmp(e->d_name + len - strlen(suffix), suffix) == 0) {
            n++;
        }
    }
    closedir(d);
    return n;
}

static void add_shell(void)
{
    const char *shell = getenv("SHELL");
    if (!shell) {
        return;
    }
    const char *name = strrchr(shell, '/');
    name = name ? name + 1 : shell;
    size_t len = strlen(name);
    if (len > 4 && strcmp(name + len - 4, ".elf") == 0) {
        len -= 4;
    }
    add("Shell", "%.*s", (int)len, name);
}

static void add_cpu(char *text, const char *clock)
{
    char model[VALUE_MAX] = "";
    read_file("/proc/cpuinfo", text, TEXT_MAX);
    if (!field(text, "model name", model, sizeof(model))) {
        return;
    }
    // "ARM Cortex-A72 (BCM2711)" reads best as just the core.
    char *core = strncmp(model, "ARM ", 4) == 0 ? model + 4 : model;
    char *paren = strstr(core, " (");
    if (paren) {
        *paren = '\0';
    }

    read_file("/proc/stat", text, TEXT_MAX);
    int cores = 0;
    for (const char *p = text; (p = strstr(p, "\ncpu")) != NULL; p++) {
        cores++;
    }

    long mhz = number(clock, "arm_mhz");
    char speed[24] = "";
    if (mhz >= 1000) {
        snprintf(speed, sizeof(speed), " @ %ld.%02ld GHz", mhz / 1000, mhz % 1000 / 10);
    } else if (mhz > 0) {
        snprintf(speed, sizeof(speed), " @ %ld MHz", mhz);
    }
    add("CPU", "%s (%d)%s", core, cores, speed);
}

static void add_memory(char *text)
{
    char total[32], free_kb[32];
    read_file("/proc/meminfo", text, TEXT_MAX);
    if (!field(text, "MemTotal", total, sizeof(total))
        || !field(text, "MemFree", free_kb, sizeof(free_kb))) {
        return;
    }
    long t = atol(total), f = atol(free_kb);
    long used = t > f ? t - f : 0;
    add("Memory", "%ld MiB / %ld MiB (%ld%%)", used / 1024, t / 1024, t ? used * 100 / t : 0);
}

static void gather(int terminal)
{
    static char text[TEXT_MAX], clock[256], fb[128];
    read_file("/proc/clock", clock, sizeof(clock));
    read_file("/proc/fb", fb, sizeof(fb));

    add_os_host_kernel(text);
    add_uptime(text);

    int programs = count_entries("/bin", ".elf", 0);
    if (programs >= 0) {
        add("Programs", "%d (/bin)", programs);
    }
    add_shell();
    int rows, cols;
    if (terminal && term_query_size(&rows, &cols) == 0) {
        add("Terminal", "console, %dx%d", cols, rows);
    } else if (terminal) {
        add("Terminal", "console");
    }
    long w = number(fb, "width"), h = number(fb, "height");
    if (w > 0 && h > 0) {
        add("Display", "%ldx%ld", w, h);
    }
    add_cpu(text, clock);
    long mc = number(clock, "temp_mc");
    if (mc > 0) {
        add("Temp", "%ld.%ld °C", mc / 1000, mc % 1000 / 100);
    }
    add_memory(text);
    int procs = count_entries("/proc", NULL, 1);
    if (procs >= 0) {
        add("Processes", "%d", procs);
    }
}

static void print_plain(const char *user)
{
    printf("%s@perspicua\n", user);
    for (size_t i = 0; i < strlen(user) + strlen("@perspicua"); i++) {
        fputc('-', stdout);
    }
    printf("\n");
    for (int i = 0; i < nlines; i++) {
        printf("%s: %s\n", lines[i].label, lines[i].value);
    }
}

// The logo in its purple, heaviest strokes bold, the lightest in the gray of its background.
static void print_logo_row(const char *row)
{
    const char *colour = NULL;
    int col = 0;
    for (const char *p = row; *p; p++, col++) {
        const char *want = strchr("#%", *p)    ? "\033[0;1;95m"
                           : strchr("*+=", *p) ? "\033[0;95m"
                           : *p != ' '         ? "\033[0;90m"
                                               : colour;
        if (want != colour) {
            printf("%s", want);
            colour = want;
        }
        fputc(*p, stdout);
    }
    printf(RESET "%*s", LOGO_W - col, "");
}

static void print_info_row(int i, const char *user)
{
    int width = SCREEN_W - LOGO_W - GAP;
    int header = 2, nrows = nlines + header + 3;
    if (i < 0 || i >= nrows) {
        return;
    }
    if (i == 0) {
        printf(LABEL "%s" RESET "@" LABEL "perspicua" RESET, user);
    } else if (i == 1) {
        for (size_t k = 0; k < strlen(user) + strlen("@perspicua"); k++) {
            printf("─");
        }
    } else if (i < nlines + header) {
        const struct line *l = &lines[i - header];
        printf(LABEL "%-*s" RESET " %.*s", LABEL_W, l->label, width - LABEL_W - 1, l->value);
    } else if (i > nlines + header) {
        // Normal colours, then bright: the terminal's palette at a glance.
        const char *base = i == nlines + header + 1 ? "4" : "10";
        for (int c = 0; c < 8; c++) {
            printf("\033[%s%dm   ", base, c);
        }
        printf(RESET);
    }
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        fprintf(stderr, "usage: kfetch\n");
        return 2;
    }

    int terminal = stdout_is_terminal();
    const char *user = getenv("USER");
    if (!user) {
        user = "root";
    }
    gather(terminal);

    if (!terminal) {
        print_plain(user);
        return 0;
    }

    int info_rows = nlines + 2 + 3;
    int top = info_rows < LOGO_LINES ? (LOGO_LINES - info_rows) / 2 : 0;
    int rows = top + info_rows > LOGO_LINES ? top + info_rows : LOGO_LINES;

    printf("\n");
    for (int r = 0; r < rows; r++) {
        if (r < LOGO_LINES) {
            print_logo_row(logo[r]);
        } else {
            printf("%*s", LOGO_W, "");
        }
        printf("%*s", GAP, "");
        print_info_row(r - top, user);
        printf("\n");
    }
    printf("\n");
    return 0;
}
