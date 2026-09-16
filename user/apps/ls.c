#include <stddef.h>
#include <stdint.h>

#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "dirent.h"
#include "uapi/stat.h"

static void print_mode(uint32_t mode)
{
    const char *type_str = "[REG]";

    if (S_ISDIR(mode)) {
        type_str = "[DIR]";
    } else if (S_ISCHR(mode)) {
        type_str = "[CHR]";
    } else if (S_ISBLK(mode)) {
        type_str = "[BLK]";
    } else if (S_ISFIFO(mode)) {
        type_str = "[PIP]";
    } else if (S_ISLNK(mode)) {
        type_str = "[LNK]";
    } else if (mode & S_IXUSR) {
        type_str = "[EXE]";
    }

    printf("%-5s  ", type_str);
    printf("%c", (mode & S_IRUSR) ? 'r' : '-');
    printf("%c", (mode & S_IWUSR) ? 'w' : '-');
    printf("%c", (mode & S_IXUSR) ? 'x' : '-');
    printf("%c", (mode & S_IRGRP) ? 'r' : '-');
    printf("%c", (mode & S_IWGRP) ? 'w' : '-');
    printf("%c", (mode & S_IXGRP) ? 'x' : '-');
    printf("%c", (mode & S_IROTH) ? 'r' : '-');
    printf("%c", (mode & S_IWOTH) ? 'w' : '-');
    printf("%c", (mode & S_IXOTH) ? 'x' : '-');
}

/*
 * format_time - "Mmm dd hh:mm", the column ls has always shown.
 *
 * Done here rather than with localtime/strftime because the libc has neither
 * yet. Everything is UTC: nothing records a zone, on disk or anywhere else.
 */
static void format_time(uint32_t secs, char *out, size_t n)
{
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    if (secs == 0) {
        snprintf(out, n, "%12s", "-");
        return;
    }

    long days = (long)(secs / 86400);
    long rem = (long)(secs % 86400);

    // Civil date from a day count, proleptic Gregorian.
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long day = doy - (153 * mp + 2) / 5 + 1;
    long month = mp + (mp < 10 ? 3 : -9);

    snprintf(out, n, "%s %2ld %02ld:%02ld", months[month - 1], day, rem / 3600, (rem % 3600) / 60);
}

static void print_header(void)
{
    printf(" TYPE   PERMISSIONS  LNK  OWNER:GROUP      SIZE  MODIFIED      NAME\n");
    for (int i = 0; i < 76; i++) {
        write(1, "─", 3);
    }
    printf("\n");
}

static void print_entry(const char *name, struct stat *st, int long_format)
{
    if (long_format) {
        print_mode(st->st_mode);

        char og[32];
        snprintf(og, sizeof(og), "%u:%u", (uint32_t)st->st_uid, (uint32_t)st->st_gid);

        char when[16];
        format_time((uint32_t)st->st_mtime, when, sizeof(when));

        printf("  %3u  %-12s  %8u  %-12s  %s\n", (unsigned int)st->st_nlink, og,
               (unsigned int)st->st_size, when, name);
    } else {
        printf("%s\n", name);
    }
}

#define LS_TERM_W      80
#define LS_MAX_ENTRIES 512

// tcgetpgrp succeeds only on a terminal, distinguishing it from a pipe or file.
static int stdout_is_tty(void)
{
    return tcgetpgrp(1) >= 0;
}

// Pack names into columns (down-then-across), like ls at a terminal.
static void print_columnar(char **names, int count)
{
    int maxlen = 0;
    for (int i = 0; i < count; i++) {
        int l = (int)strlen(names[i]);
        if (l > maxlen) {
            maxlen = l;
        }
    }
    int colw = maxlen + 2;
    int cols = LS_TERM_W / colw;
    if (cols < 1) {
        cols = 1;
    }
    int rows = (count + cols - 1) / cols;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = c * rows + r;
            if (idx >= count) {
                continue;
            }
            printf("%s", names[idx]);
            if (c < cols - 1 && (c + 1) * rows + r < count) {
                int pad = colw - (int)strlen(names[idx]);
                for (int p = 0; p < pad; p++) {
                    printf(" ");
                }
            }
        }
        printf("\n");
    }
}

static int list_path_with_options(const char *path, int long_format, int show_all)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        printf("ls: cannot stat '%s'\n", path);
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (long_format) {
            print_header();
        }
        print_entry(path, &st, long_format);
        return 0;
    }

    DIR *dirp = opendir(path);
    if (!dirp) {
        printf("ls: cannot open directory '%s'\n", path);
        return 1;
    }

    if (long_format) {
        print_header();
    }

    /* Plain listing collects names so it can lay them out in columns (at a
     * terminal) or one per line (when piped, keeping `ls | wc -l` correct). */
    char **names = NULL;
    int count = 0;
    if (!long_format) {
        names = malloc(sizeof(char *) * LS_MAX_ENTRIES);
    }

    struct dirent *ent;
    char full_path[512];
    int path_len = strlen(path);

    while ((ent = readdir(dirp)) != NULL) {
        if (ent->d_name[0] == '.' && !show_all) {
            continue;
        }

        if (long_format) {
            struct stat entry_st;
            if (strcmp(path, ".") == 0) {
                strncpy(full_path, ent->d_name, sizeof(full_path));
            } else {
                strncpy(full_path, path, sizeof(full_path));
                if (path[path_len - 1] != '/') {
                    strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
                }
                strncat(full_path, ent->d_name, sizeof(full_path) - strlen(full_path) - 1);
            }

            if (stat(full_path, &entry_st) < 0) {
                printf("?---------      ?       ?       ?        ?  %s\n", ent->d_name);
            } else {
                print_entry(ent->d_name, &entry_st, long_format);
            }
        } else if (names && count < LS_MAX_ENTRIES) {
            names[count++] = strdup(ent->d_name);
        }
    }

    closedir(dirp);

    if (!long_format && names) {
        if (stdout_is_tty()) {
            print_columnar(names, count);
        } else {
            for (int i = 0; i < count; i++) {
                printf("%s\n", names[i]);
            }
        }
        for (int i = 0; i < count; i++) {
            free(names[i]);
        }
        free(names);
    }

    return 0;
}

int main(int argc, char **argv)
{
    int long_format = 0;
    int show_all = 0;
    int start_idx = 1;

    // Very simple flag parsing
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j] != '\0'; j++) {
                if (argv[i][j] == 'l') {
                    long_format = 1;
                } else if (argv[i][j] == 'a') {
                    show_all = 1;
                }
            }
            start_idx++;
        } else {
            break;
        }
    }

    if (start_idx == argc) {
        list_path_with_options(".", long_format, show_all);
    } else {
        for (int i = start_idx; i < argc; i++) {
            if (argc > start_idx + 1) {
                printf("%s:\n", argv[i]);
            }
            list_path_with_options(argv[i], long_format, show_all);
            if (i < argc - 1) {
                printf("\n");
            }
        }
    }

    return 0;
}
