#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "uapi/stat.h"

static void print_mode(uint32_t mode)
{
    char type = '-';
    if (S_ISDIR(mode))
        type = 'd';
    else if (S_ISCHR(mode))
        type = 'c';
    else if (S_ISBLK(mode))
        type = 'b';
    else if (S_ISFIFO(mode))
        type = 'p';
    else if (S_ISLNK(mode))
        type = 'l';

    printf("%c", type);
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

static void print_entry(const char* name, struct stat* st, int long_format)
{
    if (long_format)
    {
        print_mode(st->st_mode);
        printf(" %2u %4u %4u %8u %s\n",
               (unsigned int)st->st_nlink,
               (unsigned int)st->st_uid,
               (unsigned int)st->st_gid,
               (unsigned int)st->st_size,
               name);
    }
    else
    {
        printf("%s\n", name);
    }
}

static int list_path_with_options(const char* path, int long_format, int show_all)
{
    struct stat st;
    if (sys_stat(path, &st) < 0)
    {
        printf("ls: cannot stat '%s'\n", path);
        return 1;
    }

    if (!S_ISDIR(st.st_mode))
    {
        print_entry(path, &st, long_format);
        return 0;
    }

    int fd = sys_open(path, VFS_O_RDONLY);
    if (fd < 0)
    {
        printf("ls: cannot open directory '%s'\n", path);
        return 1;
    }

    struct vfs_dirent dirents[16];
    int res;
    char full_path[512];
    int path_len = strlen(path);

    while ((res = sys_getdents(fd, dirents, sizeof(dirents))) > 0)
    {
        for (int i = 0; i < res; i++)
        {
            if (dirents[i].name[0] == '.' && !show_all)
            {
                continue;
            }

            if (long_format)
            {
                struct stat entry_st;
                // Skip constructing path if it's . or /
                if (strcmp(path, ".") == 0)
                {
                    strncpy(full_path, dirents[i].name, sizeof(full_path));
                }
                else
                {
                    strncpy(full_path, path, sizeof(full_path));
                    if (path[path_len - 1] != '/')
                    {
                        strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
                    }
                    strncat(full_path, dirents[i].name, sizeof(full_path) - strlen(full_path) - 1);
                }

                if (sys_stat(full_path, &entry_st) < 0)
                {
                    // If stat fails, just print name
                    printf("?--------- ? ? ? ? %s\n", dirents[i].name);
                }
                else
                {
                    print_entry(dirents[i].name, &entry_st, long_format);
                }
            }
            else
            {
                print_entry(dirents[i].name, NULL, long_format);
            }
        }
    }

    sys_close(fd);
    return 0;
}

int main(int argc, char** argv)
{
    int long_format = 0;
    int show_all = 0;
    int start_idx = 1;

    // Very simple flag parsing
    for (int i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-')
        {
            for (int j = 1; argv[i][j] != '\0'; j++)
            {
                if (argv[i][j] == 'l')
                {
                    long_format = 1;
                }
                else if (argv[i][j] == 'a')
                {
                    show_all = 1;
                }
            }
            start_idx++;
        }
        else
        {
            break;
        }
    }

    if (start_idx == argc)
    {
        list_path_with_options(".", long_format, show_all);
    }
    else
    {
        for (int i = start_idx; i < argc; i++)
        {
            if (argc > start_idx + 1)
            {
                printf("%s:\n", argv[i]);
            }
            list_path_with_options(argv[i], long_format, show_all);
            if (i < argc - 1)
            {
                printf("\n");
            }
        }
    }

    return 0;
}
