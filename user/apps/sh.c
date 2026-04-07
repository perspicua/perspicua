#include "syscall.h"
#include "string.h"
#include "signals.h"
#include "types.h"
#include "uapi/stat.h"
#include "wait.h"
#include "stdlib.h"
#include "stdio.h"
#include "dirent.h"

#define MAX_ARGS    32
#define CMD_MAX_LEN 512
#define MAX_CMDS    16
#define MAX_HISTORY 32

#define KEY_UP        1001
#define KEY_DOWN      1002
#define KEY_TAB       '\t'
#define KEY_BACKSPACE 127

static char *history[MAX_HISTORY];
static int history_count = 0;
static int history_index = -1;

static char last_completion_buffer[CMD_MAX_LEN] = {0};
static int last_completion_displayed = 0;
static int last_completion_lines = 0;

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *infile;
    char *outfile;
    int append;
    int background;
} Command;

static char *sh_strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *new = malloc(len);
    if (new)
        strcpy(new, s);
    return new;
}

static char *sh_strndup(const char *s, size_t len)
{
    char *new = malloc(len + 1);
    if (new) {
        strncpy(new, s, len);
        new[len] = 0;
    }
    return new;
}

static void add_to_history(const char *line)
{
    if (history_count > 0 && strcmp(history[history_count - 1], line) == 0)
        return;

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

static void print_prompt(void);

static void clear_completion_matches(void)
{
    if (last_completion_lines > 0) {
        printf("\r\033[%dA\033[0J", last_completion_lines + 1);
        last_completion_lines = 0;
        last_completion_displayed = 0;
    }
}

static void redraw_line(const char *cmd)
{
    printf(
        "\r\033[2K"); // Try ANSI clear line, if not supported it might just print junk but we'll see
    // If \033[2K fails, we can fallback to:
    // printf("\r \r");
    print_prompt();
    printf("%s", cmd);
}

static int read_key(void)
{
    char c;
    if (sys_read(0, &c, 1) <= 0)
        return -1;

    // printf("DEBUG: c=%d\n", (int)c);

    if (c == 27) {
        char seq[2];
        if (sys_read(0, &seq[0], 1) <= 0)
            return 27;
        if (sys_read(0, &seq[1], 1) <= 0)
            return 27;

        // printf("DEBUG: seq=[%d, %d]\n", (int)seq[0], (int)seq[1]);

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

static void handle_sigchld(int sig)
{
    (void)sig;
    /* Reap any finished background children */
    while (sys_waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* * Replaces special shell operators with spaced-out versions
 * so our tokenizer can easily split them without breaking quotes.
 */
static void expand_operators(const char *line, char *expanded)
{
    int i = 0, j = 0;
    int in_quotes = 0;

    while (line[i] != '\0') {
        if (line[i] == '"') {
            in_quotes = !in_quotes;
        }

        if (!in_quotes && (line[i] == '<' || line[i] == '>' || line[i] == '|' || line[i] == ';')) {
            if (line[i] == '>' && line[i + 1] == '>') {
                expanded[j++] = ' ';
                expanded[j++] = '>';
                expanded[j++] = '>';
                expanded[j++] = ' ';
                i++;
            } else {
                expanded[j++] = ' ';
                expanded[j++] = line[i];
                expanded[j++] = ' ';
            }
        } else {
            expanded[j++] = line[i];
        }
        i++;
    }
    expanded[j] = '\0';
}

static void parse_command(char *str, Command *cmd)
{
    cmd->argc = 0;
    cmd->infile = NULL;
    cmd->outfile = NULL;
    cmd->append = 0;
    cmd->background = 0;

    char *tokens[64];
    int token_count = 0;
    char *p = str;

    while (*p) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (!*p)
            break;

        if (*p == '"') {
            p++; // Skip opening quote
            tokens[token_count++] = p;
            while (*p && *p != '"')
                p++;
            if (*p)
                *p++ = '\0'; // Replace closing quote
        } else {
            tokens[token_count++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
        }
    }

    for (int i = 0; i < token_count; i++) {
        if (strcmp(tokens[i], "<") == 0 && i + 1 < token_count) {
            cmd->infile = tokens[++i];
        } else if (strcmp(tokens[i], ">") == 0 && i + 1 < token_count) {
            cmd->outfile = tokens[++i];
        } else if (strcmp(tokens[i], ">>") == 0 && i + 1 < token_count) {
            cmd->append = 1;
            cmd->outfile = tokens[++i];
        } else if (strcmp(tokens[i], "&") == 0) {
            cmd->background = 1;
        } else {
            if (cmd->argc < MAX_ARGS - 1) {
                cmd->argv[cmd->argc++] = tokens[i];
            }
        }
    }
    cmd->argv[cmd->argc] = NULL;
}

static int is_parent_builtin(const char *name)
{
    return (strcmp(name, "cd") == 0 || strcmp(name, "exit") == 0 || strcmp(name, "export") == 0
            || strcmp(name, "unset") == 0);
}

static void run_parent_builtin(Command *cmd)
{
    if (strcmp(cmd->argv[0], "exit") == 0) {
        sys_exit(0);
    } else if (strcmp(cmd->argv[0], "cd") == 0) {
        const char *target = (cmd->argc > 1) ? cmd->argv[1] : "/";
        if (sys_chdir(target) < 0) {
            printf("sh: cd: no such directory: %s\n", target);
        }
    } else if (strcmp(cmd->argv[0], "export") == 0) {
        if (cmd->argc > 1) {
            char *arg = cmd->argv[1];
            char *equals = strchr(arg, '=');
            if (equals) {
                *equals = '\0';
                setenv(arg, equals + 1, 1);
                *equals = '='; // Restore if needed, though arg is local to parse_command's tokens
            } else {
                setenv(arg, "", 1);
            }
        }
    } else if (strcmp(cmd->argv[0], "unset") == 0) {
        if (cmd->argc > 1) {
            unsetenv(cmd->argv[1]);
        }
    }
}

static int is_output_builtin(const char *name)
{
    return (strcmp(name, "clear") == 0 || strcmp(name, "echo") == 0 || strcmp(name, "pwd") == 0
            || strcmp(name, "help") == 0 || strcmp(name, "env") == 0);
}

static void run_output_builtin(Command *cmd)
{
    if (strcmp(cmd->argv[0], "clear") == 0) {
        /* \033[H  - Move cursor to home (1,1)
         * \033[2J - Clear entire screen
         * \033[3J - Clear scrollback buffer
         */
        printf("\033[H\033[2J\033[3J");
    } else if (strcmp(cmd->argv[0], "echo") == 0) {
        for (int i = 1; i < cmd->argc; i++) {
            printf("%s", cmd->argv[i]);
            if (i < cmd->argc - 1)
                printf(" ");
        }
        printf("\n");
    } else if (strcmp(cmd->argv[0], "pwd") == 0) {
        char cwd[256];
        if (sys_getcwd(cwd, sizeof(cwd)) == 0) {
            printf("%s\n", cwd);
        }
    } else if (strcmp(cmd->argv[0], "help") == 0) {
        printf("Perspicua Shell\n");
        printf("Built-ins: help, echo, clear, pwd, cd, exit, export, unset, env\n");
        printf("Features: |, >, >>, <, \" \", &, ;\n");
    } else if (strcmp(cmd->argv[0], "env") == 0) {
        if (environ) {
            for (int i = 0; environ[i]; i++) {
                printf("%s\n", environ[i]);
            }
        }
    }
}

static void run_exec(Command *cmd)
{
    char path[256];
    char *name = cmd->argv[0];

    // If command contains a slash, try to exec it directly
    if (strchr(name, '/')) {
        sys_exec(name, cmd->argv, environ);
        printf("sh: %s : no such file or directory\n", name);
        sys_exit(1);
    }

    char *path_env = getenv("PATH");
    if (!path_env) {
        path_env = "/bin:/";
    }

    char path_copy[256];
    strncpy(path_copy, path_env, sizeof(path_copy));
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *dir = strtok(path_copy, ":");
    while (dir) {
        // Try dir/name.elf
        strcpy(path, dir);
        int len = strlen(path);
        if (len > 0 && path[len - 1] != '/') {
            strcat(path, "/");
        }
        strcat(path, name);
        strcat(path, ".elf");
        sys_exec(path, cmd->argv, environ);

        // Try dir/name
        strcpy(path, dir);
        len = strlen(path);
        if (len > 0 && path[len - 1] != '/') {
            strcat(path, "/");
        }
        strcat(path, name);
        sys_exec(path, cmd->argv, environ);

        dir = strtok(NULL, ":");
    }

    printf("sh: command not found: %s\n", name);
    sys_exit(1);
}

static int apply_redirections(Command *cmd)
{
    if (cmd->infile) {
        int fd = sys_open(cmd->infile, VFS_O_RDONLY);
        if (fd < 0) {
            printf("sh: cannot open input file\n");
            return -1;
        }
        sys_dup2(fd, 0);
        sys_close(fd);
    }
    if (cmd->outfile) {
        int flags = VFS_O_WRONLY | VFS_O_CREAT | (cmd->append ? VFS_O_APPEND : VFS_O_TRUNC);
        int fd = sys_open(cmd->outfile, flags);
        if (fd < 0) {
            printf("sh: cannot open output file\n");
            return -1;
        }
        sys_dup2(fd, 1);
        sys_close(fd);
    }
    return 0;
}

static void execute_pipeline(char *pipe_string)
{
    char *commands_str[MAX_CMDS];
    int num_cmds = 0;

    char *p = pipe_string;
    commands_str[num_cmds++] = p;
    while (*p) {
        if (*p == '|') {
            *p = '\0';
            commands_str[num_cmds++] = p + 1;
        }
        p++;
    }

    if (num_cmds == 1) {
        Command cmd;
        parse_command(commands_str[0], &cmd);
        if (cmd.argc == 0)
            return;

        if (is_parent_builtin(cmd.argv[0])) {
            run_parent_builtin(&cmd);
            return;
        }

        int pid = sys_fork();
        if (pid == 0) {
            if (apply_redirections(&cmd) < 0)
                sys_exit(1);

            if (is_output_builtin(cmd.argv[0])) {
                run_output_builtin(&cmd);
                sys_exit(0);
            }
            run_exec(&cmd);
            sys_exit(1);
        } else {
            if (!cmd.background) {
                sys_waitpid(pid, NULL, 0);
            }
        }
        return;
    }

    /* Handle multiple piped commands */
    int prev_pipe = -1;
    int pipefd[2];
    int pids[MAX_CMDS];
    int bg_flag = 0;

    for (int i = 0; i < num_cmds; i++) {
        Command cmd;
        parse_command(commands_str[i], &cmd);
        if (cmd.argc == 0)
            continue;
        if (cmd.background)
            bg_flag = 1;

        if (i < num_cmds - 1) {
            if (sys_pipe(pipefd) < 0) {
                printf("sh: pipe failed\n");
                return;
            }
        }

        int pid = sys_fork();
        if (pid == 0) {
            if (prev_pipe != -1) {
                sys_dup2(prev_pipe, 0);
                sys_close(prev_pipe);
            }
            if (i < num_cmds - 1) {
                sys_dup2(pipefd[1], 1);
                sys_close(pipefd[0]);
                sys_close(pipefd[1]);
            }

            if (apply_redirections(&cmd) < 0)
                sys_exit(1);

            if (is_output_builtin(cmd.argv[0])) {
                run_output_builtin(&cmd);
                sys_exit(0);
            }
            run_exec(&cmd);
            sys_exit(1);
        } else {
            pids[i] = pid;
            if (prev_pipe != -1)
                sys_close(prev_pipe);
            if (i < num_cmds - 1) {
                sys_close(pipefd[1]);
                prev_pipe = pipefd[0];
            }
        }
    }

    if (!bg_flag) {
        for (int i = 0; i < num_cmds; i++) {
            sys_waitpid(pids[i], NULL, 0);
        }
    }
}

static void execute_line(char *line)
{
    char *expanded = malloc(CMD_MAX_LEN * 2);
    if (!expanded) {
        printf("sh: memory allocation failed\n");
        return;
    }
    expand_operators(line, expanded);

    /* Split by semi-colons for sequential execution */
    char *seq_commands[16];
    int num_seq = 0;

    char *p = expanded;
    seq_commands[num_seq++] = p;

    int in_quotes = 0;
    while (*p) {
        if (*p == '"')
            in_quotes = !in_quotes;
        if (*p == ';' && !in_quotes) {
            *p = '\0';
            if (num_seq < 16) {
                seq_commands[num_seq++] = p + 1;
            }
        }
        p++;
    }

    for (int i = 0; i < num_seq; i++) {
        execute_pipeline(seq_commands[i]);
    }

    free(expanded);
}

// returns start of last word
static int find_token_start(const char *buf, int cursor_pos)
{
    if (cursor_pos == 0)
        return 0;

    int pos = cursor_pos - 1;

    while (pos >= 0) {
        if (buf[pos] == ' ' || buf[pos] == '\t')
            return pos + 1;
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

// Find longest common prefix among all matches
static int find_common_prefix_len(char **matches, int count)
{
    if (count == 0 || !matches[0])
        return 0;

    int common_len = strlen(matches[0]);
    for (int i = 1; i < count; i++) {
        int j = 0;
        while (j < common_len && matches[i][j] && matches[0][j] == matches[i][j])
            j++;
        common_len = j;
    }
    return common_len;
}

// true if cmd; false if file/arg
static int is_cmd_position(const char *buf, int token_start)
{
    if (token_start == 0)
        return 1;

    int pos = token_start - 1;
    while (pos > 0 && buf[pos] == ' ') {
        pos--;
    }

    if (pos <= 0)
        return 1;

    if (buf[pos] == '|' || buf[pos] == ';' || buf[pos] == '&')
        return 1;
    else
        return 0;
}

static char **find_cmd_matches(const char *prefix, int *count_out)
{
    char *path_env = getenv("PATH");
    if (!path_env)
        path_env = "/bin:/";

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
        if (!dir)
            continue;

        struct vfs_dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            char *entry_name = entry->name;

            // skips "." and ".."
            if (entry_name[0] == '.')
                continue;

            // if potential match not cmd skip
            int name_len = strlen(entry_name);
            if (name_len < 4 || strcmp(entry_name + name_len - 4, ".elf") != 0)
                continue;

            int cmd_len = name_len - 4;
            char *cmd_name = malloc(cmd_len + 1);
            strncpy(cmd_name, entry_name, cmd_len);
            cmd_name[cmd_len] = 0;

            // if prefix of cmd matches prefix, add to match list (if not duplicate)
            if (prefix_len == 0 || !strncmp(cmd_name, prefix, prefix_len)) {
                int dup = 0;
                for (int i = 0; i < matches_count; i++) {
                    if (strcmp(cmd_name, matches[i]) == 0)
                        dup = 1;
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

                if (!dup && matches_count < capacity)
                    matches[matches_count++] = cmd_name;
                else
                    free(cmd_name);
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

    // Sort matches alphabetically
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

    struct vfs_dirent *entry = NULL;
    while ((entry = readdir(d))) {
        char *entry_name = entry->name;

        // skip special entries
        if (!strcmp(entry_name, ".") || !strcmp(entry_name, ".."))
            continue;

        // handle hidden files
        if (entry_name[0] == '.' && !prefix_is_hidden)
            continue;

        if (prefix_len > 0 && strncmp(entry_name, prefix, prefix_len))
            continue;

        size_t full_path_len = strlen(search_dir) + 1 + strlen(entry_name) + 1;
        char *full_path = malloc(full_path_len);
        if (!full_path)
            continue;

        // Add path separator if search_dir doesn't end with one
        size_t dir_len_tmp = strlen(search_dir);
        if (dir_len_tmp > 0 && search_dir[dir_len_tmp - 1] == '/')
            snprintf(full_path, full_path_len, "%s%s", search_dir, entry_name);
        else
            snprintf(full_path, full_path_len, "%s/%s", search_dir, entry_name);

        struct stat st;
        int is_dir = 0;
        if (!sys_stat(full_path, &st)) {
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
            if (!match_str)
                continue;
            if (dir_ends_slash) {
                if (is_dir)
                    snprintf(match_str, match_len, "%s%s/", dir, entry_name);
                else
                    snprintf(match_str, match_len, "%s%s", dir, entry_name);
            } else {
                if (is_dir)
                    snprintf(match_str, match_len, "%s/%s/", dir, entry_name);
                else
                    snprintf(match_str, match_len, "%s/%s", dir, entry_name);
            }
        } else {
            if (is_dir) {
                match_str = malloc(strlen(entry_name) + 2);
                if (!match_str)
                    continue;
                snprintf(match_str, strlen(entry_name) + 2, "%s/", entry_name);
            } else {
                match_str = sh_strdup(entry_name);
                if (!match_str)
                    continue;
            }
        }

        if (matches_count == capacity) {
            int new_capacity = 2 * capacity;
            char **tmp = realloc(matches, new_capacity * sizeof(char *));
            if (!tmp) {
                for (int i = 0; i < matches_count; i++)
                    free(matches[i]);
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

    // Sort matches alphabetically
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

static int do_completions(char *cmd_buffer, int *cmd_len, int cursor_pos)
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
        if (matches)
            free(matches);
        free(prefix);
        return cursor_pos;
    }

    int new_cursor = cursor_pos;

    if (match_count == 1) {
        // Single match - complete it fully
        char *completion = matches[0];
        int completion_len = strlen(completion);

        // Check if completion ends with '/' (directory)
        int is_directory = (completion_len > 0 && completion[completion_len - 1] == '/');

        // Don't add space after directories (allows continuing to type)
        int add_space = is_directory ? 0 : 1;
        int insert_len = completion_len - prefix_len + add_space;
        int tail_len = *cmd_len - cursor_pos;

        // Ensure we have space for the insertion + null terminator
        if (*cmd_len + insert_len >= CMD_MAX_LEN - 1)
            goto cleanup;

        memmove(cmd_buffer + cursor_pos + insert_len, cmd_buffer + cursor_pos, tail_len);

        memcpy(cmd_buffer + token_start, completion, completion_len);

        if (add_space)
            cmd_buffer[token_start + completion_len] = ' ';

        *cmd_len += insert_len;
        cmd_buffer[*cmd_len] = '\0';

        new_cursor = token_start + completion_len + add_space;

        // Reset since we modified the buffer
        last_completion_displayed = 0;
        last_completion_lines = 0;
    } else {
        // Multiple matches - try to complete common prefix first
        int common_len = find_common_prefix_len(matches, match_count);

        if (common_len > prefix_len) {
            // There's a common prefix we can complete to
            int insert_len = common_len - prefix_len;
            int tail_len = *cmd_len - cursor_pos;

            if (*cmd_len + insert_len < CMD_MAX_LEN - 1) {
                memmove(cmd_buffer + cursor_pos + insert_len, cmd_buffer + cursor_pos, tail_len);
                memcpy(cmd_buffer + token_start, matches[0], common_len);

                *cmd_len += insert_len;
                cmd_buffer[*cmd_len] = '\0';

                new_cursor = token_start + common_len;

                // Reset completion display state since buffer changed
                last_completion_displayed = 0;
                last_completion_lines = 0;
            }
        } else {
            // No additional common prefix - show all matches
            int already_displayed =
                (last_completion_displayed && strcmp(cmd_buffer, last_completion_buffer) == 0);

            if (!already_displayed) {
                printf("\n");
                for (int i = 0; i < match_count; i++) {
                    printf("%s\n", matches[i]);
                }
                fflush(stdout);

                // Remember we displayed matches for this buffer
                strncpy(last_completion_buffer, cmd_buffer, CMD_MAX_LEN - 1);
                last_completion_buffer[CMD_MAX_LEN - 1] = '\0';
                last_completion_displayed = 1;
                last_completion_lines = match_count;

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

// static void completion(char *cmd_buffer, size_t cmd_len) {}

static void print_prompt(void)
{
    char cwd[256];
    if (sys_getcwd(cwd, sizeof(cwd)) == 0) {
        printf("perspicua:%s$ ", cwd);
    } else {
        printf("perspicua:$ ");
    }
}

int main(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;
    sys_signal(SIGNAL_INT, SIGNAL_IGN);
    sys_signal(SIGNAL_CHLD, handle_sigchld);

    printf("Perspicua Shell\n");
    printf("Type help to see available commands.\n\n");

    char *cmd_buffer = malloc(CMD_MAX_LEN);
    if (!cmd_buffer) {
        printf("sh: memory allocation failed\n");
        sys_exit(1);
    }
    int cmd_length = 0;

    print_prompt();

    while (1) {
        int key = read_key();
        if (key < 0)
            continue;

        if (key == '\n' || key == '\r') {
            cmd_buffer[cmd_length] = '\0';
            printf("\n");

            if (cmd_length > 0) {
                add_to_history(cmd_buffer);
                execute_line(cmd_buffer);
            }

            // Clear any displayed completion matches before new prompt
            clear_completion_matches();

            cmd_length = 0;
            cmd_buffer[0] = '\0'; // Actually clear the buffer
            history_index = -1;
            // Reset all completion state for new command
            last_completion_displayed = 0;
            last_completion_lines = 0;
            last_completion_buffer[0] = '\0';
            print_prompt();
        } else if (key == KEY_BACKSPACE || key == '\b') {
            if (cmd_length > 0) {
                if (last_completion_displayed) {
                    clear_completion_matches();
                    cmd_length--;
                    cmd_buffer[cmd_length] = '\0'; // Actually remove the character
                    redraw_line(cmd_buffer);
                } else {
                    cmd_length--;
                    cmd_buffer[cmd_length] = '\0'; // Actually remove the character
                    printf("\b \b");
                }
                last_completion_displayed = 0;
            }
        } else if (key == KEY_UP) {
            clear_completion_matches();
            if (history_count > 0 && history_index < history_count - 1) {
                history_index++;
                strcpy(cmd_buffer, history[history_count - 1 - history_index]);
                cmd_length = strlen(cmd_buffer);
                redraw_line(cmd_buffer);
                last_completion_displayed = 0;
            }
        } else if (key == KEY_DOWN) {
            clear_completion_matches();
            if (history_index > 0) {
                history_index--;
                strcpy(cmd_buffer, history[history_count - 1 - history_index]);
                cmd_length = strlen(cmd_buffer);
                redraw_line(cmd_buffer);
                last_completion_displayed = 0;
            } else if (history_index == 0) {
                history_index = -1;
                cmd_buffer[0] = '\0';
                cmd_length = 0;
                redraw_line(cmd_buffer);
                last_completion_displayed = 0;
            }
        } else if (key == KEY_TAB) {
            cmd_buffer[cmd_length] = '\0'; // Ensure null-terminated before completion
            do_completions(cmd_buffer, &cmd_length, cmd_length);
            // Redraw to show the updated buffer (important for single-match auto-complete)
            redraw_line(cmd_buffer);
        } else if (key >= 32 && key <= 126) {
            if (cmd_length < CMD_MAX_LEN - 1) {
                if (last_completion_displayed) {
                    clear_completion_matches();
                    cmd_buffer[cmd_length++] = (char)key;
                    cmd_buffer[cmd_length] = '\0'; // Keep null-terminated
                    redraw_line(cmd_buffer);
                } else {
                    cmd_buffer[cmd_length++] = (char)key;
                    cmd_buffer[cmd_length] = '\0'; // Keep null-terminated
                    printf("%c", (char)key);
                }
                last_completion_displayed = 0;
            }
        }
    }

    free(cmd_buffer);
    return 0;
}
