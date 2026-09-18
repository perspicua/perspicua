/*
 * edit.c - Reading a line: history, key handling and tab completion.
 */

#include "sh.h"

#include <stdbool.h>
#include <stddef.h>

#include "dirent.h"
#include "signal.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"
#include "wait.h"

#define MAX_HISTORY 32

#define KEY_UP        1001
#define KEY_DOWN      1002
#define KEY_TAB       '\t'
#define KEY_BACKSPACE 127

static char *history[MAX_HISTORY];
static int history_count = 0;
static int history_index = -1;

/*
 * What the last tab press put on screen, so a second press can erase it before
 * drawing something else. Grouped because the three move together: displaying
 * matches sets all of them and clearing them resets all of them.
 */
static struct {
    char buffer[CMD_MAX_LEN];
    int displayed;
    int lines;
} last_completion;

void add_to_history(const char *line)
{
    if (history_count > 0 && strcmp(history[history_count - 1], line) == 0) {
        return;
    }

    if (history_count < MAX_HISTORY) {
        history[history_count++] = sh_strdup(line);
    } else {
        free(history[0]);
        for (int i = 1; i < MAX_HISTORY; i++) {
            history[i - 1] = history[i];
        }
        history[MAX_HISTORY - 1] = sh_strdup(line);
    }
}

void clear_completion_matches(void)
{
    if (last_completion.lines > 0) {
        printf("\r\033[%dA\033[0J", last_completion.lines + 1);
        last_completion.lines = 0;
        last_completion.displayed = 0;
    }
}

void redraw_line(const char *cmd)
{
    printf("\r\033[2K");
    print_prompt();
    printf("%s", cmd);
}

int read_key(void)
{
    char c;
    if (read(0, &c, 1) <= 0) {
        return -1;
    }

    if (c == 27) {
        char seq[2];
        if (read(0, &seq[0], 1) <= 0) {
            return 27;
        }
        if (read(0, &seq[1], 1) <= 0) {
            return 27;
        }

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A':
                    return KEY_UP;
                case 'B':
                    return KEY_DOWN;
            }
        }
        return 27;
    }
    return (unsigned char)c;
}

// returns start of last word
static int find_token_start(const char *buf, int cursor_pos)
{
    if (cursor_pos == 0) {
        return 0;
    }

    int pos = cursor_pos - 1;

    while (pos >= 0) {
        if (buf[pos] == ' ' || buf[pos] == '\t') {
            return pos + 1;
        }
        pos--;
    }

    return 0;
}

// simple bubble sort for small arrays
static void sort_matches(char **matches, int count)
{
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (strcmp(matches[j], matches[j + 1]) > 0) {
                char *temp = matches[j];
                matches[j] = matches[j + 1];
                matches[j + 1] = temp;
            }
        }
    }
}

static int find_common_prefix_len(char **matches, int count)
{
    if (count == 0 || !matches[0]) {
        return 0;
    }

    int common_len = strlen(matches[0]);
    for (int i = 1; i < count; i++) {
        int j = 0;
        while (j < common_len && matches[i][j] && matches[0][j] == matches[i][j]) {
            j++;
        }
        common_len = j;
    }
    return common_len;
}

// true if cmd; false if file/arg
static int is_cmd_position(const char *buf, int token_start)
{
    if (token_start == 0) {
        return 1;
    }

    int pos = token_start - 1;
    while (pos > 0 && buf[pos] == ' ') {
        pos--;
    }

    if (pos <= 0) {
        return 1;
    }

    if (buf[pos] == '|' || buf[pos] == ';' || buf[pos] == '&') {
        return 1;
    } else {
        return 0;
    }
}

static char **find_cmd_matches(const char *prefix, int *count_out)
{
    char *path_env = getenv("PATH");
    if (!path_env) {
        path_env = "/bin:/";
    }

    size_t prefix_len = strlen(prefix);

    char path_cpy[256];
    strncpy(path_cpy, path_env, sizeof(path_cpy) - 1);
    path_cpy[sizeof(path_cpy) - 1] = 0;
    char *path_part = strtok(path_cpy, ":");

    // 2^vibe
    int capacity = 32;
    int matches_count = 0;
    char **matches = malloc(capacity * sizeof(char *));

    while (path_part) {
        DIR *dir = opendir(path_part);
        if (!dir) {
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            char *entry_name = entry->d_name;

            // skips "." and ".."
            if (entry_name[0] == '.') {
                continue;
            }

            // if potential match not cmd skip
            int name_len = strlen(entry_name);
            if (name_len < 4 || strcmp(entry_name + name_len - 4, ".elf") != 0) {
                continue;
            }

            int cmd_len = name_len - 4;
            char *cmd_name = malloc(cmd_len + 1);
            strncpy(cmd_name, entry_name, cmd_len);
            cmd_name[cmd_len] = 0;

            // if prefix of cmd matches prefix, add to match list (if not duplicate)
            if (prefix_len == 0 || !strncmp(cmd_name, prefix, prefix_len)) {
                int dup = 0;
                for (int i = 0; i < matches_count; i++) {
                    if (strcmp(cmd_name, matches[i]) == 0) {
                        dup = 1;
                    }
                }

                if (matches_count == capacity) {
                    int new_capacity = 2 * capacity;
                    char **tmp = realloc(matches, new_capacity * sizeof(char *));
                    if (!tmp) {
                        free(matches);
                        *count_out = 0;
                        return NULL;
                    }
                    capacity = new_capacity;
                    matches = tmp;
                }

                if (!dup && matches_count < capacity) {
                    matches[matches_count++] = cmd_name;
                } else {
                    free(cmd_name);
                }
            }
        }
        closedir(dir);
        path_part = strtok(NULL, ":");
    }

    if (matches_count == 0) {
        free(matches);
        *count_out = 0;
        return NULL;
    }

    sort_matches(matches, matches_count);

    void *temp = realloc(matches, (matches_count + 1) * sizeof(char *));
    if (!temp) {
        free(matches);
        *count_out = 0;
        return NULL;
    }

    matches = temp;
    matches[matches_count] = NULL;
    *count_out = matches_count;

    return matches;
}

static void split_path(const char *partial, char **dir_out, char **prefix_out)
{
    char *last_slash = strrchr(partial, '/');
    if (!last_slash) {
        *dir_out = NULL;
        *prefix_out = sh_strdup(partial);
    } else if (last_slash == partial) {
        *dir_out = sh_strdup("/");
        *prefix_out = sh_strdup(partial + 1);
    } else {
        int dir_len = last_slash - partial;
        *dir_out = sh_strndup(partial, dir_len);
        *prefix_out = sh_strdup(last_slash + 1);
    }
}

static char **find_file_matches(const char *dir, const char *prefix, int *count_out)
{
    const char *search_dir = (dir == NULL || !strcmp(dir, "")) ? "." : dir;
    int include_dir_prefix = (dir != NULL && strcmp(dir, "") != 0);

    size_t prefix_len = strlen(prefix);
    int prefix_is_hidden = (prefix_len > 0 && prefix[0] == '.');

    int capacity = 32;
    int matches_count = 0;
    char **matches = malloc(capacity * sizeof(char *));
    if (!matches) {
        *count_out = 0;
        return NULL;
    }

    DIR *d = opendir(search_dir);
    if (!d) {
        free(matches);
        *count_out = 0;
        return NULL;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(d))) {
        char *entry_name = entry->d_name;

        // skip special entries
        if (!strcmp(entry_name, ".") || !strcmp(entry_name, "..")) {
            continue;
        }

        // handle hidden files
        if (entry_name[0] == '.' && !prefix_is_hidden) {
            continue;
        }

        if (prefix_len > 0 && strncmp(entry_name, prefix, prefix_len)) {
            continue;
        }

        size_t full_path_len = strlen(search_dir) + 1 + strlen(entry_name) + 1;
        char *full_path = malloc(full_path_len);
        if (!full_path) {
            continue;
        }

        // Add path separator if search_dir doesn't end with one
        size_t dir_len_tmp = strlen(search_dir);
        if (dir_len_tmp > 0 && search_dir[dir_len_tmp - 1] == '/') {
            snprintf(full_path, full_path_len, "%s%s", search_dir, entry_name);
        } else {
            snprintf(full_path, full_path_len, "%s/%s", search_dir, entry_name);
        }

        struct stat st;
        int is_dir = 0;
        if (!stat(full_path, &st)) {
            is_dir = S_ISDIR(st.st_mode);
        }
        free(full_path);

        // Build match string with directory prefix if needed
        char *match_str;
        size_t match_len;
        if (include_dir_prefix) {
            // Include the directory prefix (e.g., "../foo" -> "../foobar/")
            // Check if dir already ends with '/' to avoid double slashes
            size_t dir_len = strlen(dir);
            int dir_ends_slash = (dir_len > 0 && dir[dir_len - 1] == '/');

            match_len =
                dir_len + (dir_ends_slash ? 0 : 1) + strlen(entry_name) + (is_dir ? 1 : 0) + 1;
            match_str = malloc(match_len);
            if (!match_str) {
                continue;
            }
            if (dir_ends_slash) {
                if (is_dir) {
                    snprintf(match_str, match_len, "%s%s/", dir, entry_name);
                } else {
                    snprintf(match_str, match_len, "%s%s", dir, entry_name);
                }
            } else {
                if (is_dir) {
                    snprintf(match_str, match_len, "%s/%s/", dir, entry_name);
                } else {
                    snprintf(match_str, match_len, "%s/%s", dir, entry_name);
                }
            }
        } else {
            if (is_dir) {
                match_str = malloc(strlen(entry_name) + 2);
                if (!match_str) {
                    continue;
                }
                snprintf(match_str, strlen(entry_name) + 2, "%s/", entry_name);
            } else {
                match_str = sh_strdup(entry_name);
                if (!match_str) {
                    continue;
                }
            }
        }

        if (matches_count == capacity) {
            int new_capacity = 2 * capacity;
            char **tmp = realloc(matches, new_capacity * sizeof(char *));
            if (!tmp) {
                for (int i = 0; i < matches_count; i++) {
                    free(matches[i]);
                }
                free(matches);
                free(match_str);
                *count_out = 0;
                closedir(d);
                return NULL;
            }
            capacity = new_capacity;
            matches = tmp;
        }

        matches[matches_count++] = match_str;
    }

    closedir(d);

    if (matches_count == 0) {
        free(matches);
        *count_out = 0;
        return NULL;
    }

    sort_matches(matches, matches_count);

    void *temp = realloc(matches, (matches_count + 1) * sizeof(char *));
    if (!temp) {
        free(matches);
        *count_out = 0;
        return NULL;
    }

    matches = temp;
    matches[matches_count] = NULL;
    *count_out = matches_count;

    return matches;
}

int do_completions(char *cmd_buffer, int *cmd_len, int cursor_pos)
{
    int token_start = find_token_start(cmd_buffer, cursor_pos);

    int prefix_len = cursor_pos - token_start;
    char *prefix = sh_strndup(cmd_buffer + token_start, prefix_len);

    int is_cmd = is_cmd_position(cmd_buffer, token_start);
    int match_count = 0;
    char **matches;

    if (is_cmd) {
        matches = find_cmd_matches(prefix, &match_count);
    } else {
        char *dir = NULL;
        char *file_prefix = NULL;
        split_path(prefix, &dir, &file_prefix);
        matches = find_file_matches(dir, file_prefix, &match_count);

        free(dir);
        free(file_prefix);
    }

    if (match_count == 0) {
        if (matches) {
            free(matches);
        }
        free(prefix);
        return cursor_pos;
    }

    int new_cursor = cursor_pos;

    if (match_count == 1) {
        // Single match - complete it fully
        char *completion = matches[0];
        int completion_len = strlen(completion);

        int is_directory = (completion_len > 0 && completion[completion_len - 1] == '/');

        // Don't add space after directories (allows continuing to type)
        int add_space = is_directory ? 0 : 1;
        int insert_len = completion_len - prefix_len + add_space;
        int tail_len = *cmd_len - cursor_pos;

        // Ensure we have space for the insertion + null terminator
        if (*cmd_len + insert_len >= CMD_MAX_LEN - 1) {
            goto cleanup;
        }

        memmove(cmd_buffer + cursor_pos + insert_len, cmd_buffer + cursor_pos, tail_len);

        memcpy(cmd_buffer + token_start, completion, completion_len);

        if (add_space) {
            cmd_buffer[token_start + completion_len] = ' ';
        }

        *cmd_len += insert_len;
        cmd_buffer[*cmd_len] = '\0';

        new_cursor = token_start + completion_len + add_space;

        // Reset since we modified the buffer
        last_completion.displayed = 0;
        last_completion.lines = 0;
    } else {
        // Multiple matches - try to complete common prefix first
        int common_len = find_common_prefix_len(matches, match_count);

        if (common_len > prefix_len) {
            int insert_len = common_len - prefix_len;
            int tail_len = *cmd_len - cursor_pos;

            if (*cmd_len + insert_len < CMD_MAX_LEN - 1) {
                memmove(cmd_buffer + cursor_pos + insert_len, cmd_buffer + cursor_pos, tail_len);
                memcpy(cmd_buffer + token_start, matches[0], common_len);

                *cmd_len += insert_len;
                cmd_buffer[*cmd_len] = '\0';

                new_cursor = token_start + common_len;

                // Reset completion display state since buffer changed
                last_completion.displayed = 0;
                last_completion.lines = 0;
            }
        } else {
            // No additional common prefix - show all matches
            int already_displayed =
                (last_completion.displayed && strcmp(cmd_buffer, last_completion.buffer) == 0);

            if (!already_displayed) {
                printf("\n");
                for (int i = 0; i < match_count; i++) {
                    printf("%s\n", matches[i]);
                }
                fflush(stdout);

                // Remember we displayed matches for this buffer
                strncpy(last_completion.buffer, cmd_buffer, CMD_MAX_LEN - 1);
                last_completion.buffer[CMD_MAX_LEN - 1] = '\0';
                last_completion.displayed = 1;
                last_completion.lines = match_count;

                // Print prompt and buffer after showing matches
                print_prompt();
                printf("%s", cmd_buffer);
                fflush(stdout);
            }
        }
    }

cleanup:
    for (int i = 0; i < match_count; i++) {
        free(matches[i]);
    }
    free(matches);
    free(prefix);

    return new_cursor;
}

void print_prompt(void)
{
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("perspicua:%s$ ", cwd);
    } else {
        printf("perspicua:$ ");
    }
}

/*
 * sh_read_line - Reads one edited line, returning its length.
 *
 * Owns everything between a keystroke and a finished line: history recall,
 * backspace, tab completion and echo. Returns when Enter is pressed, so the
 * caller only sees whole lines.
 */
int sh_read_line(char *buf, size_t size)
{
    int len = 0;
    buf[0] = '\0';

    history_index = -1;
    last_completion.displayed = 0;
    last_completion.lines = 0;
    last_completion.buffer[0] = '\0';

    while (1) {
        int key = read_key();
        if (key < 0) {
            continue;
        }

        if (key == '\n' || key == '\r') {
            buf[len] = '\0';
            printf("\n");
            clear_completion_matches();
            return len;
        } else if (key == KEY_BACKSPACE || key == '\b') {
            if (len > 0) {
                if (last_completion.displayed) {
                    clear_completion_matches();
                    len--;
                    buf[len] = '\0';
                    redraw_line(buf);
                } else {
                    len--;
                    buf[len] = '\0';
                    printf("\b \b");
                }
                last_completion.displayed = 0;
            }
        } else if (key == KEY_UP) {
            clear_completion_matches();
            if (history_count > 0 && history_index < history_count - 1) {
                history_index++;
                strcpy(buf, history[history_count - 1 - history_index]);
                len = strlen(buf);
                redraw_line(buf);
                last_completion.displayed = 0;
            }
        } else if (key == KEY_DOWN) {
            clear_completion_matches();
            if (history_index > 0) {
                history_index--;
                strcpy(buf, history[history_count - 1 - history_index]);
                len = strlen(buf);
                redraw_line(buf);
                last_completion.displayed = 0;
            } else if (history_index == 0) {
                history_index = -1;
                buf[0] = '\0';
                len = 0;
                redraw_line(buf);
                last_completion.displayed = 0;
            }
        } else if (key == KEY_TAB) {
            buf[len] = '\0'; // Ensure null-terminated before completion
            do_completions(buf, &len, len);
            // Redraw to show the updated buffer (important for single-match auto-complete)
            redraw_line(buf);
        } else if (key >= 32 && key <= 126) {
            if ((size_t)len < size - 1) {
                if (last_completion.displayed) {
                    clear_completion_matches();
                    buf[len++] = (char)key;
                    buf[len] = '\0'; // Keep null-terminated
                    redraw_line(buf);
                } else {
                    buf[len++] = (char)key;
                    buf[len] = '\0'; // Keep null-terminated
                    printf("%c", (char)key);
                }
                last_completion.displayed = 0;
            }
        }
    }
}
