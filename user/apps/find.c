/*
 * find - walk directory trees and print the paths that match.
 *
 * find [path...] [-name glob] [-type f|d]; the glob understands * and ?.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PATH_LEN 256

static const char *name_glob;
static int want_type; // 0 for any, 'f' or 'd'
static int status;

static int glob_match(const char *pat, const char *s)
{
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            for (const char *t = s;; t++) {
                if (glob_match(pat + 1, t)) {
                    return 1;
                }
                if (!*t) {
                    return 0;
                }
            }
        }
        if (!*s || (*pat != '?' && *pat != *s)) {
            return 0;
        }
    }
    return *s == '\0';
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

// path is a PATH_LEN buffer holding len bytes; children are appended in place.
static void visit(char *path, size_t len)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "find: %s: cannot stat\n", path);
        status = 1;
        return;
    }

    int is_dir = S_ISDIR(st.st_mode);
    int type_ok = !want_type || (want_type == 'd') == is_dir;
    if (type_ok && (!name_glob || glob_match(name_glob, base_name(path)))) {
        printf("%s\n", path);
    }
    if (!is_dir) {
        return;
    }

    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "find: %s: cannot open\n", path);
        status = 1;
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        size_t nlen = strlen(e->d_name);
        size_t sep = path[len - 1] != '/';
        if (len + sep + nlen >= PATH_LEN) {
            fprintf(stderr, "find: path too long under %s\n", path);
            status = 1;
            continue;
        }
        path[len] = '/';
        memcpy(path + len + sep, e->d_name, nlen + 1);
        visit(path, len + sep + nlen);
        path[len] = '\0';
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    int first = 1;
    int i = 1;
    while (i < argc && argv[i][0] != '-') {
        i++;
    }
    int paths_end = i;

    for (; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "find: %s needs an argument\n", argv[i]);
            return 2;
        }
        if (strcmp(argv[i], "-name") == 0) {
            name_glob = argv[i + 1];
        } else if (strcmp(argv[i], "-type") == 0 && (argv[i + 1][0] == 'f' || argv[i + 1][0] == 'd')
                   && argv[i + 1][1] == '\0') {
            want_type = argv[i + 1][0];
        } else {
            fprintf(stderr, "usage: find [path...] [-name glob] [-type f|d]\n");
            return 2;
        }
    }

    char path[PATH_LEN];
    if (first == paths_end) {
        strcpy(path, ".");
        visit(path, 1);
    }
    for (int k = first; k < paths_end; k++) {
        size_t len = strlen(argv[k]);
        if (len == 0 || len >= PATH_LEN) {
            continue;
        }
        memcpy(path, argv[k], len + 1);
        visit(path, len);
    }
    return status;
}
