#include "syscall.h"
#include "string.h"

#define MAX_ARGS    32
#define CMD_MAX_LEN 512
#define MAX_CMDS    16

typedef struct
{
    char* argv[MAX_ARGS];
    int argc;
    char* infile;
    char* outfile;
    int append;
    int background;
} Command;

static void print_string(const char* s)
{
    sys_write(1, s, strlen(s));
}

static void print_char(char c)
{
    sys_write(1, &c, 1);
}

/* * Replaces special shell operators with spaced-out versions
 * so our tokenizer can easily split them without breaking quotes.
 */
static void expand_operators(const char* line, char* expanded)
{
    int i = 0, j = 0;
    int in_quotes = 0;

    while (line[i] != '\0')
    {
        if (line[i] == '"')
        {
            in_quotes = !in_quotes;
        }

        if (!in_quotes && (line[i] == '<' || line[i] == '>' || line[i] == '|' || line[i] == ';'))
        {
            if (line[i] == '>' && line[i + 1] == '>')
            {
                expanded[j++] = ' ';
                expanded[j++] = '>';
                expanded[j++] = '>';
                expanded[j++] = ' ';
                i++;
            }
            else
            {
                expanded[j++] = ' ';
                expanded[j++] = line[i];
                expanded[j++] = ' ';
            }
        }
        else
        {
            expanded[j++] = line[i];
        }
        i++;
    }
    expanded[j] = '\0';
}

static void parse_command(char* str, Command* cmd)
{
    cmd->argc = 0;
    cmd->infile = NULL;
    cmd->outfile = NULL;
    cmd->append = 0;
    cmd->background = 0;

    char* tokens[64];
    int token_count = 0;
    char* p = str;

    while (*p)
    {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (!*p)
            break;

        if (*p == '"')
        {
            p++;  // Skip opening quote
            tokens[token_count++] = p;
            while (*p && *p != '"')
                p++;
            if (*p)
                *p++ = '\0';  // Replace closing quote
        }
        else
        {
            tokens[token_count++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
        }
    }

    for (int i = 0; i < token_count; i++)
    {
        if (strcmp(tokens[i], "<") == 0 && i + 1 < token_count)
        {
            cmd->infile = tokens[++i];
        }
        else if (strcmp(tokens[i], ">") == 0 && i + 1 < token_count)
        {
            cmd->outfile = tokens[++i];
        }
        else if (strcmp(tokens[i], ">>") == 0 && i + 1 < token_count)
        {
            cmd->append = 1;
            cmd->outfile = tokens[++i];
        }
        else if (strcmp(tokens[i], "&") == 0)
        {
            cmd->background = 1;
        }
        else
        {
            if (cmd->argc < MAX_ARGS - 1)
            {
                cmd->argv[cmd->argc++] = tokens[i];
            }
        }
    }
    cmd->argv[cmd->argc] = NULL;
}

static int is_parent_builtin(const char* name)
{
    return (strcmp(name, "cd") == 0 || strcmp(name, "exit") == 0);
}

static void run_parent_builtin(Command* cmd)
{
    if (strcmp(cmd->argv[0], "exit") == 0)
    {
        sys_exit(0);
    }
    else if (strcmp(cmd->argv[0], "cd") == 0)
    {
        const char* target = (cmd->argc > 1) ? cmd->argv[1] : "/";
        if (sys_chdir(target) < 0)
        {
            print_string("sh: cd: no such directory: ");
            print_string(target);
            print_string("\n");
        }
    }
}

static int is_output_builtin(const char* name)
{
    return (strcmp(name, "clear") == 0 || strcmp(name, "echo") == 0 || strcmp(name, "pwd") == 0
            || strcmp(name, "help") == 0);
}

static void run_output_builtin(Command* cmd)
{
    if (strcmp(cmd->argv[0], "clear") == 0)
    {
        print_string("\033[2J\033[H");
    }
    else if (strcmp(cmd->argv[0], "echo") == 0)
    {
        for (int i = 1; i < cmd->argc; i++)
        {
            print_string(cmd->argv[i]);
            if (i < cmd->argc - 1)
                print_string(" ");
        }
        print_string("\n");
    }
    else if (strcmp(cmd->argv[0], "pwd") == 0)
    {
        char cwd[256];
        if (sys_getcwd(cwd, sizeof(cwd)) == 0)
        {
            print_string(cwd);
            print_string("\n");
        }
    }
    else if (strcmp(cmd->argv[0], "help") == 0)
    {
        print_string("\033[1;33mPerspicua Beefy Shell\033[0m\n");
        print_string("Built-ins: help, echo, clear, pwd, cd, exit\n");
        print_string(
            "Features: Multiple Pipes (|), Redirections (>, >>, <), Quotes (\" \"), Background (&), Sequences (;)\n");
    }
}

static void run_exec(Command* cmd)
{
    char path[256];
    if (cmd->argv[0][0] == '/')
    {
        strcpy(path, cmd->argv[0]);
    }
    else
    {
        strcpy(path, "/");
        strcat(path, cmd->argv[0]);
        strcat(path, ".elf");
    }

    if (sys_exec(path) < 0)
    {
        if (cmd->argv[0][0] != '/')
        {
            strcpy(path, "/");
            strcat(path, cmd->argv[0]);
            if (sys_exec(path) < 0)
            {
                print_string("sh: command not found: ");
                print_string(cmd->argv[0]);
                print_string("\n");
                sys_exit(1);
            }
        }
        else
        {
            print_string("sh: command not found: ");
            print_string(cmd->argv[0]);
            print_string("\n");
            sys_exit(1);
        }
    }
}

static int apply_redirections(Command* cmd)
{
    if (cmd->infile)
    {
        int fd = sys_open(cmd->infile, VFS_O_RDONLY);
        if (fd < 0)
        {
            print_string("sh: cannot open input file\n");
            return -1;
        }
        sys_dup2(fd, 0);
        sys_close(fd);
    }
    if (cmd->outfile)
    {
        int flags = VFS_O_WRONLY | VFS_O_CREAT | (cmd->append ? VFS_O_APPEND : VFS_O_TRUNC);
        int fd = sys_open(cmd->outfile, flags);
        if (fd < 0)
        {
            print_string("sh: cannot open output file\n");
            return -1;
        }
        sys_dup2(fd, 1);
        sys_close(fd);
    }
    return 0;
}

static void execute_pipeline(char* pipe_string)
{
    char* commands_str[MAX_CMDS];
    int num_cmds = 0;

    char* p = pipe_string;
    commands_str[num_cmds++] = p;
    while (*p)
    {
        if (*p == '|')
        {
            *p = '\0';
            commands_str[num_cmds++] = p + 1;
        }
        p++;
    }

    if (num_cmds == 1)
    {
        Command cmd;
        parse_command(commands_str[0], &cmd);
        if (cmd.argc == 0)
            return;

        if (is_parent_builtin(cmd.argv[0]))
        {
            run_parent_builtin(&cmd);
            return;
        }

        int pid = sys_fork();
        if (pid == 0)
        {
            if (apply_redirections(&cmd) < 0)
                sys_exit(1);

            if (is_output_builtin(cmd.argv[0]))
            {
                run_output_builtin(&cmd);
                sys_exit(0);
            }
            run_exec(&cmd);
            sys_exit(1);
        }
        else
        {
            if (!cmd.background)
            {
                sys_waitpid(pid, NULL);
            }
        }
        return;
    }

    /* Handle multiple piped commands */
    int prev_pipe = -1;
    int pipefd[2];
    int pids[MAX_CMDS];
    int bg_flag = 0;

    for (int i = 0; i < num_cmds; i++)
    {
        Command cmd;
        parse_command(commands_str[i], &cmd);
        if (cmd.argc == 0)
            continue;
        if (cmd.background)
            bg_flag = 1;

        if (i < num_cmds - 1)
        {
            if (sys_pipe(pipefd) < 0)
            {
                print_string("sh: pipe failed\n");
                return;
            }
        }

        int pid = sys_fork();
        if (pid == 0)
        {
            if (prev_pipe != -1)
            {
                sys_dup2(prev_pipe, 0);
                sys_close(prev_pipe);
            }
            if (i < num_cmds - 1)
            {
                sys_dup2(pipefd[1], 1);
                sys_close(pipefd[0]);
                sys_close(pipefd[1]);
            }

            if (apply_redirections(&cmd) < 0)
                sys_exit(1);

            if (is_output_builtin(cmd.argv[0]))
            {
                run_output_builtin(&cmd);
                sys_exit(0);
            }
            run_exec(&cmd);
            sys_exit(1);
        }
        else
        {
            pids[i] = pid;
            if (prev_pipe != -1)
                sys_close(prev_pipe);
            if (i < num_cmds - 1)
            {
                sys_close(pipefd[1]);
                prev_pipe = pipefd[0];
            }
        }
    }

    if (!bg_flag)
    {
        for (int i = 0; i < num_cmds; i++)
        {
            sys_waitpid(pids[i], NULL);
        }
    }
}

static void execute_line(char* line)
{
    char expanded[CMD_MAX_LEN * 2];
    expand_operators(line, expanded);

    /* Split by semi-colons for sequential execution */
    char* seq_commands[16];
    int num_seq = 0;

    char* p = expanded;
    seq_commands[num_seq++] = p;

    int in_quotes = 0;
    while (*p)
    {
        if (*p == '"')
            in_quotes = !in_quotes;
        if (*p == ';' && !in_quotes)
        {
            *p = '\0';
            seq_commands[num_seq++] = p + 1;
        }
        p++;
    }

    for (int i = 0; i < num_seq; i++)
    {
        execute_pipeline(seq_commands[i]);
    }
}

static void print_prompt(void)
{
    char cwd[256];
    if (sys_getcwd(cwd, sizeof(cwd)) == 0)
    {
        print_string("\033[1;32mperspicua\033[0m:\033[1;34m");
        print_string(cwd);
        print_string("\033[0m$ ");
    }
    else
    {
        print_string("perspicua:$ ");
    }
}

int main(void)
{
    print_string("\033[2J\033[H"); /* Clear screen */
    print_string("\033[1;36m==============================\033[0m\n");
    print_string("      \033[1;33mPerspicua OS Shell\033[0m      \n");
    print_string("\033[1;36m==============================\033[0m\n");
    print_string("Type \033[1mhelp\033[0m to see available commands.\n\n");

    char cmd_buffer[CMD_MAX_LEN];
    int cmd_length = 0;

    print_prompt();

    while (1)
    {
        char c;
        if (sys_read(0, &c, 1) > 0)
        {
            if (c == '\n' || c == '\r')
            {
                cmd_buffer[cmd_length] = '\0';
                print_string("\n");

                if (cmd_length > 0)
                {
                    execute_line(cmd_buffer);
                }

                cmd_length = 0;
                print_prompt();
            }
            else if (c == '\b' || c == 127)
            {
                if (cmd_length > 0)
                {
                    cmd_length--;
                    print_string("\b \b");
                }
            }
            else if (c >= 32 && c <= 126)
            {
                if (cmd_length < CMD_MAX_LEN - 1)
                {
                    cmd_buffer[cmd_length++] = c;
                    print_char(c);
                }
            }
        }
    }

    return 0;
}
