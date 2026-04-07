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

static void print_header(void)
{
    printf(" TYPE   PERMISSIONS  LNK  OWNER:GROUP      SIZE  NAME\n");
    for (int i = 0; i < 62; i++)
        sys_write(1, "─", 3);
    printf("\n");
}

static void print_entry(const char *name, struct stat *st, int long_format)
{
    if (long_format) {
        print_mode(st->st_mode);

        char og[32];
        snprintf(og, sizeof(og), "%u:%u", (uint32_t)st->st_uid, (uint32_t)st->st_gid);

        printf("  %3u  %-12s  %8u  %s\n", (unsigned int)st->st_nlink, og, (unsigned int)st->st_size,
               name);
    } else {
        printf("%s\n", name);
    }
}

static int list_path_with_options(const char *path, int long_format, int show_all)
{
    struct stat st;
    if (sys_stat(path, &st) < 0) {
        printf("ls: cannot stat '%s'\n", path);
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (long_format)
            print_header();
        print_entry(path, &st, long_format);
        return 0;
    }

    DIR *dirp = opendir(path);
    if (!dirp) {
        printf("ls: cannot open directory '%s'\n", path);
        return 1;
    }

    if (long_format)
        print_header();

    struct vfs_dirent *ent;
    char full_path[512];
    int path_len = strlen(path);

    while ((ent = readdir(dirp)) != NULL) {
        if (ent->name[0] == '.' && !show_all) {
            continue;
        }

        if (long_format) {
            struct stat entry_st;
            if (strcmp(path, ".") == 0) {
                strncpy(full_path, ent->name, sizeof(full_path));
            } else {
                strncpy(full_path, path, sizeof(full_path));
                if (path[path_len - 1] != '/') {
                    strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
                }
                strncat(full_path, ent->name, sizeof(full_path) - strlen(full_path) - 1);
            }

            if (sys_stat(full_path, &entry_st) < 0) {
                printf("?---------      ?       ?       ?        ?  %s\n", ent->name);
            } else {
                print_entry(ent->name, &entry_st, long_format);
            }
        } else {
            print_entry(ent->name, NULL, long_format);
        }
    }

    closedir(dirp);
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
