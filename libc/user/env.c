#include "stdlib.h"
#include "string.h"

char **environ = NULL;
static int environ_is_malloced = 0;

char *getenv(const char *name)
{
    if (!environ || !name)
        return NULL;
    size_t len = strlen(name);
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, len) == 0 && environ[i][len] == '=') {
            return &environ[i][len + 1];
        }
    }
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (!name || name[0] == '\0' || strchr(name, '='))
        return -1;

    char *existing = getenv(name);
    if (existing && !overwrite)
        return 0;

    size_t name_len = strlen(name);
    size_t val_len = value ? strlen(value) : 0;
    char *new_entry = malloc(name_len + val_len + 2);
    if (!new_entry)
        return -1;

    strcpy(new_entry, name);
    new_entry[name_len] = '=';
    if (value) {
        strcpy(new_entry + name_len + 1, value);
    } else {
        new_entry[name_len + 1] = '\0';
    }

    if (existing) {
        // Find the index and replace
        for (int i = 0; environ[i]; i++) {
            if (strncmp(environ[i], name, name_len) == 0 && environ[i][name_len] == '=') {
                // We don't free the old entry because we don't know if it was malloced
                // Standard libc behavior is to leak it unless it was put there by putenv/setenv
                // For simplicity, we'll just overwrite.
                environ[i] = new_entry;
                return 0;
            }
        }
    }

    // Add new entry
    int count = 0;
    if (environ) {
        while (environ[count])
            count++;
    }

    char **new_environ;
    if (environ_is_malloced) {
        new_environ = realloc(environ, (count + 2) * sizeof(char *));
    } else {
        new_environ = malloc((count + 2) * sizeof(char *));
        if (new_environ) {
            for (int i = 0; i < count; i++)
                new_environ[i] = environ[i];
            environ_is_malloced = 1;
        }
    }

    if (!new_environ) {
        free(new_entry);
        return -1;
    }

    new_environ[count] = new_entry;
    new_environ[count + 1] = NULL;
    environ = new_environ;

    return 0;
}

int unsetenv(const char *name)
{
    if (!environ || !name || name[0] == '\0' || strchr(name, '='))
        return -1;

    size_t len = strlen(name);
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, len) == 0 && environ[i][len] == '=') {
            // Found it, shift everything else down
            for (int j = i; environ[j]; j++) {
                environ[j] = environ[j + 1];
            }
            return 0;
        }
    }
    return 0;
}

int putenv(char *string)
{
    if (!string || !strchr(string, '='))
        return -1;

    // Split string to name for getenv
    char *equals = strchr(string, '=');
    size_t name_len = equals - string;
    char name[128]; // Arbitrary limit for temporary check
    if (name_len >= sizeof(name))
        name_len = sizeof(name) - 1;
    strncpy(name, string, name_len);
    name[name_len] = '\0';

    char *existing = getenv(name);
    if (existing) {
        for (int i = 0; environ[i]; i++) {
            if (strncmp(environ[i], name, name_len) == 0 && environ[i][name_len] == '=') {
                environ[i] = string;
                return 0;
            }
        }
    }

    // Add new entry
    int count = 0;
    if (environ) {
        while (environ[count])
            count++;
    }

    char **new_environ;
    if (environ_is_malloced) {
        new_environ = realloc(environ, (count + 2) * sizeof(char *));
    } else {
        new_environ = malloc((count + 2) * sizeof(char *));
        if (new_environ) {
            for (int i = 0; i < count; i++)
                new_environ[i] = environ[i];
            environ_is_malloced = 1;
        }
    }

    if (!new_environ)
        return -1;

    new_environ[count] = string;
    new_environ[count + 1] = NULL;
    environ = new_environ;

    return 0;
}

void _libc_init(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    environ = envp;
}
