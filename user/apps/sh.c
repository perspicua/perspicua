#include "syscall.h"
#include "string.h"
#include "signals.h"
#include "wait.h"
#include "stdlib.h"
#include "stdio.h"

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

static void redraw_line(const char *cmd)
{
    printf("\r\033[2K"); // Try ANSI clear line, if not supported it might just print junk but we'll see
    // If \033[2K fails, we can fallback to:
    // printf("\r                                                                                \r");
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

            cmd_length = 0;
            history_index = -1;
            print_prompt();
        } else if (key == KEY_BACKSPACE || key == '\b') {
            if (cmd_length > 0) {
                cmd_length--;
                printf("\b \b");
            }
        } else if (key == KEY_UP) {
            if (history_count > 0 && history_index < history_count - 1) {
                history_index++;
                strcpy(cmd_buffer, history[history_count - 1 - history_index]);
                cmd_length = strlen(cmd_buffer);
                redraw_line(cmd_buffer);
            }
        } else if (key == KEY_DOWN) {
            if (history_index > 0) {
                history_index--;
                strcpy(cmd_buffer, history[history_count - 1 - history_index]);
                cmd_length = strlen(cmd_buffer);
                redraw_line(cmd_buffer);
            } else if (history_index == 0) {
                history_index = -1;
                cmd_buffer[0] = '\0';
                cmd_length = 0;
                redraw_line(cmd_buffer);
            }
        } else if (key >= 32 && key <= 126) {
            if (cmd_length < CMD_MAX_LEN - 1) {
                cmd_buffer[cmd_length++] = (char)key;
                printf("%c", (char)key);
            }
        }
    }

    free(cmd_buffer);
    return 0;
}
