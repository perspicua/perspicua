/*
 * env.c - The environment: getenv, setenv, unsetenv and putenv over environ.
 */

#include <stddef.h>

#include "errno.h"
#include "stdlib.h"
#include "string.h"

#include "libc/internal.h"

char **environ = NULL;
static int environ_is_malloced = 0;

// The entries setenv made: the only ones libc may free when they are replaced or removed.
static char **owned;
static size_t owned_count, owned_cap;

static int own(char *entry)
{
    if (owned_count == owned_cap) {
        size_t cap = owned_cap ? owned_cap * 2 : 16;
        char **bigger = realloc(owned, cap * sizeof(char *));
        if (!bigger) {
            return -1;
        }
        owned = bigger;
        owned_cap = cap;
    }
    owned[owned_count++] = entry;
    return 0;
}

// Frees entry if setenv made it; a string from execve or putenv belongs to someone else.
static void release(char *entry)
{
    for (size_t i = 0; i < owned_count; i++) {
        if (owned[i] == entry) {
            owned[i] = owned[--owned_count];
            free(entry);
            return;
        }
    }
}

// The index of the entry whose name is the first name_len bytes of key, or -1.
static int find(const char *key, size_t name_len)
{
    for (int i = 0; environ && environ[i]; i++) {
        if (strncmp(environ[i], key, name_len) == 0 && environ[i][name_len] == '=') {
            return i;
        }
    }
    return -1;
}

static int valid_name(const char *name)
{
    if (!name || name[0] == '\0' || strchr(name, '=')) {
        errno = EINVAL;
        return 0;
    }
    return 1;
}

// Puts entry in place of the one at index i, or after the last one when i is -1.
static int place(int i, char *entry)
{
    if (i >= 0) {
        char *old = environ[i];
        environ[i] = entry;
        release(old);
        return 0;
    }

    int count = 0;
    while (environ && environ[count]) {
        count++;
    }
    char **bigger;
    if (environ_is_malloced) {
        bigger = realloc(environ, (size_t)(count + 2) * sizeof(char *));
    } else {
        bigger = malloc((size_t)(count + 2) * sizeof(char *));
        if (bigger && count > 0) {
            memcpy(bigger, environ, (size_t)count * sizeof(char *));
        }
    }
    if (!bigger) {
        errno = ENOMEM;
        return -1;
    }
    environ_is_malloced = 1;
    bigger[count] = entry;
    bigger[count + 1] = NULL;
    environ = bigger;
    return 0;
}

char *getenv(const char *name)
{
    if (!name) {
        return NULL;
    }
    size_t len = strlen(name);
    int i = find(name, len);
    return i >= 0 ? &environ[i][len + 1] : NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (!valid_name(name)) {
        return -1;
    }
    size_t name_len = strlen(name);
    int i = find(name, name_len);
    if (i >= 0 && !overwrite) {
        return 0;
    }

    size_t val_len = value ? strlen(value) : 0;
    char *entry = malloc(name_len + val_len + 2);
    if (!entry || own(entry) < 0) {
        free(entry);
        errno = ENOMEM;
        return -1;
    }
    memcpy(entry, name, name_len);
    entry[name_len] = '=';
    memcpy(entry + name_len + 1, value ? value : "", val_len + 1);

    if (place(i, entry) < 0) {
        release(entry);
        return -1;
    }
    return 0;
}

// Removes every entry for name: duplicates can arrive through execve's envp.
int unsetenv(const char *name)
{
    if (!valid_name(name)) {
        return -1;
    }
    size_t len = strlen(name);
    int i;
    while ((i = find(name, len)) >= 0) {
        char *old = environ[i];
        for (int j = i; environ[j]; j++) {
            environ[j] = environ[j + 1];
        }
        release(old);
    }
    return 0;
}

int putenv(char *string)
{
    char *equals = string ? strchr(string, '=') : NULL;
    if (!equals || equals == string) {
        errno = EINVAL;
        return -1;
    }
    // The whole name, however long, so a longer one never stands in for a shorter.
    return place(find(string, (size_t)(equals - string)), string);
}

void __libc_init(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    environ = envp;
}
