/*
 * du - space used under each directory, in kB (file sizes, rounded up).
 *
 * -s prints one total per argument instead of a line per directory.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PATH_LEN 256

static int summary;
static int status;

static unsigned long kb(unsigned long bytes)
{
    return (bytes + 1023) / 1024;
}

// Returns the bytes under path, printing each directory on the way out.
static unsigned long walk(char *path, size_t len, int top)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "du: %s: cannot stat\n", path);
        status = 1;
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (top) {
            printf("%lu\t%s\n", kb((unsigned long)st.st_size), path);
        }
        return (unsigned long)st.st_size;
    }

    unsigned long total = 0;
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "du: %s: cannot open\n", path);
        status = 1;
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        size_t nlen = strlen(e->d_name);
        size_t sep = path[len - 1] != '/';
        if (len + sep + nlen >= PATH_LEN) {
            fprintf(stderr, "du: path too long under %s\n", path);
            status = 1;
            continue;
        }
        path[len] = '/';
        memcpy(path + len + sep, e->d_name, nlen + 1);
        total += walk(path, len + sep + nlen, 0);
        path[len] = '\0';
    }
    closedir(d);

    if (!summary || top) {
        printf("%lu\t%s\n", kb(total), path);
    }
    return total;
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "-s") == 0) {
        summary = 1;
        i++;
    }

    char path[PATH_LEN];
    if (i == argc) {
        strcpy(path, ".");
        walk(path, 1, 1);
    }
    for (; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (len == 0 || len >= PATH_LEN) {
            continue;
        }
        memcpy(path, argv[i], len + 1);
        walk(path, len, 1);
    }
    return status;
}
