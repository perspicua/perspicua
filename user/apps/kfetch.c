/*
 * kfetch.c - System information display for Perspicua.
 */

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "types.h"

static int read_proc_file(const char *path, char *buf, size_t bufsz)
{
    int fd = sys_open(path, VFS_O_RDONLY);
    if (fd < 0)
        return -1;
    int n = sys_read(fd, buf, bufsz - 1);
    if (n > 0)
        buf[n] = '\0';
    else
        buf[0] = '\0';
    sys_close(fd);
    return n;
}

static unsigned long atoul(const char *s)
{
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static char *find_field(char *buf, const char *key)
{
    char *p = buf;
    size_t klen = strlen(key);
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            p += klen + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            return p;
        }
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }
    return NULL;
}

static void strip_newline(char *s)
{
    char *p = strchr(s, '\n');
    if (p)
        *p = '\0';
}

#define BAR_WIDTH  20
#define LOGO_LINES 20
#define LOGO_W     42
#define LABEL_W    8
#define MAX_INFO   20

static const char *logo[LOGO_LINES] = {
    "                                        ", "                                        ",
    "             .:-=++++++=-:.             ", "          .:+*#*+==---=+***+-.          ",
    "        .-*#+-.    .+=.::.:+#*-.        ", "       :*%+.        -::*##=..+%*:       ",
    "      :##-  . :-------:::=##+ -##:      ", "     .*#- ... =%##****##*-.+*: -#*.     ",
    "     :#*. ... =##=    .+##-   ..*#:     ", "     -#*..... =##= ....*##: ... +#-     ",
    "     :#*. ... =##*****##*- ... .*#:     ", "      +%=  .. =##+:-:::.  .... =%+      ",
    "      .*%=. . =##=      ....  =%*.      ", "       .=#*-. -##= .....   .-*#=.       ",
    "         :+##=+##= .   ..-=*##+::       ", "           .-+*##= .++****+*#####+.     ",
    "               .:: .==-:.. =######*-    ", "                         ...+#######+.  ",
    "                             :*#######-.", "                              .=*******=",
};

static char info_lines[MAX_INFO][128];
static int info_count = 0;

static void add_info(const char *label, const char *value)
{
    if (info_count >= MAX_INFO)
        return;
    snprintf(info_lines[info_count++], 128, "%-*s  %s", LABEL_W, label, value);
}

static void add_sep(void)
{
    if (info_count >= MAX_INFO)
        return;
    memset(info_lines[info_count], 0, 128);
    for (int i = 0; i < 35; i++) {
        strncat(info_lines[info_count], "─", 127);
    }
    info_count++;
}

int main(void)
{
    char buf[512];
    char tmp[128];

    /* 1. Gather Data */
    char ver[64] = "unknown";
    if (read_proc_file("/proc/version", buf, sizeof(buf)) > 0) {
        strip_newline(buf);
        strncpy(ver, buf, sizeof(ver) - 1);
    }

    char uptime_str[64] = "unknown";
    if (read_proc_file("/proc/uptime", buf, sizeof(buf)) > 0) {
        strip_newline(buf);
        snprintf(uptime_str, sizeof(uptime_str), "%s seconds", buf);
    }

    unsigned long mem_total = 0, mem_free = 0, slab_used = 0, slab_total = 0;
    if (read_proc_file("/proc/meminfo", buf, sizeof(buf)) > 0) {
        char *p;
        if ((p = find_field(buf, "MemTotal")))
            mem_total = atoul(p);
        if ((p = find_field(buf, "MemFree")))
            mem_free = atoul(p);
        if ((p = find_field(buf, "SlabTotal")))
            slab_total = atoul(p);
        if ((p = find_field(buf, "SlabUsed")))
            slab_used = atoul(p);
    }
    unsigned long mem_used = (mem_total >= mem_free) ? mem_total - mem_free : 0;

    int procs = 0;
    int fd = sys_open("/proc", VFS_O_RDONLY);
    if (fd >= 0) {
        struct vfs_dirent dent;
        while (sys_getdents(fd, &dent, sizeof(dent)) > 0) {
            if (dent.name[0] >= '1' && dent.name[0] <= '9')
                procs++;
        }
        sys_close(fd);
    }

    char cwd[256];
    sys_getcwd(cwd, sizeof(cwd));

    /* 2. Format Info Lines */
    add_info("OS", "Perspicua");
    add_info("KERNEL", ver);
    add_info("ARCH", "AArch64");
    add_info("UPTIME", uptime_str);
    add_sep();

    add_info("USER", "root");
    add_info("SHELL", "sh");
    snprintf(tmp, sizeof(tmp), "%d", procs);
    add_info("PROCS", tmp);
    snprintf(tmp, sizeof(tmp), "%d", sys_getpid());
    add_info("PID", tmp);
    add_info("CWD", cwd);
    add_sep();

    /* Memory Bars */
    const char *labels[] = {"MEM", "SLAB"};
    unsigned long used[] = {mem_used, slab_used};
    unsigned long total[] = {mem_total, slab_total};

    for (int j = 0; j < 2; j++) {
        if (info_count >= MAX_INFO)
            break;
        int pos = snprintf(info_lines[info_count], 128, "%-*s  [", LABEL_W, labels[j]);

        int filled =
            (total[j] == 0) ? 0 : (int)((unsigned long long)used[j] * BAR_WIDTH / total[j]);
        if (filled > BAR_WIDTH)
            filled = BAR_WIDTH;

        for (int i = 0; i < filled; i++)
            info_lines[info_count][pos++] = '#';
        for (int i = filled; i < BAR_WIDTH; i++)
            info_lines[info_count][pos++] = '.';

        int pct = (total[j] == 0) ? 0 : (int)((unsigned long long)used[j] * 100 / total[j]);
        snprintf(info_lines[info_count] + pos, 128 - pos, "] %3d%%  %lu / %lu kB", pct, used[j],
                 total[j]);
        info_count++;
    }

    /* 3. Output Logo + Info */
    int rows = (info_count > LOGO_LINES) ? info_count : LOGO_LINES;
    printf("\n");
    for (int i = 0; i < rows; i++) {
        if (i < LOGO_LINES)
            printf("%s", logo[i]);
        else
            printf("%-*s", LOGO_W, "");

        if (i < info_count)
            printf("  %s", info_lines[i]);
        printf("\n");
    }
    printf("\n");

    return 0;
}
