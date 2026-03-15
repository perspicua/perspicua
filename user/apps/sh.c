#include "syscall.h"
#include "string.h"

static void print_string(const char* s)
{
    sys_write(1, s, strlen(s));
}

static char* trim(char* str)
{
    char* end;
    while (*str == ' ')
        str++;
    if (*str == 0)
        return str;
    end = str + strlen(str) - 1;
    while (end > str && *end == ' ')
        end--;
    end[1] = '\0';
    return str;
}

static void run_command(char* cmd)
{
    cmd = trim(cmd);
    if (strlen(cmd) == 0)
        return;

    if (strcmp(cmd, "help") == 0)
    {
        print_string("Available commands: help, cat, hello, sh, ls\n");
        print_string("Type the name of an ELF file to exec it (e.g. /cat.elf)\n");
        print_string("Pipe support: command1 | command2\n");
        return;
    }

    char path[128];
    if (cmd[0] == '/')
    {
        strcpy(path, cmd);
    }
    else
    {
        strcpy(path, "/");
        strcat(path, cmd);
        strcat(path, ".elf");
    }

    if (sys_exec(path) < 0)
    {
        print_string("Error: command not found: ");
        print_string(path);
        print_string("\n");
        sys_exit(1);
    }
}

static void execute_line(char* line)
{
    char* pipe_ptr = strchr(line, '|');
    if (pipe_ptr)
    {
        *pipe_ptr   = '\0';
        char* left  = trim(line);
        char* right = trim(pipe_ptr + 1);

        int pipefd[2];
        if (sys_pipe(pipefd) < 0)
        {
            print_string("Error: pipe failed\n");
            return;
        }

        int pid1 = sys_fork();
        if (pid1 == 0)
        {
            /* Child 1: Write to pipe */
            sys_dup2(pipefd[1], 1); /* Redirect stdout to write end */
            sys_close(pipefd[0]);
            sys_close(pipefd[1]);
            run_command(left);
            sys_exit(0);
        }

        int pid2 = sys_fork();
        if (pid2 == 0)
        {
            /* Child 2: Read from pipe */
            sys_dup2(pipefd[0], 0); /* Redirect stdin to read end */
            sys_close(pipefd[0]);
            sys_close(pipefd[1]);
            run_command(right);
            sys_exit(0);
        }

        /* Parent */
        sys_close(pipefd[0]);
        sys_close(pipefd[1]);
        sys_waitpid(pid1, (void*)0);
        sys_waitpid(pid2, (void*)0);
    }
    else
    {
        int pid = sys_fork();
        if (pid < 0)
        {
            print_string("Error: fork failed\n");
        }
        else if (pid == 0)
        {
            run_command(line);
            sys_exit(0);
        }
        else
        {
            int status = 0;
            sys_waitpid(pid, &status);
        }
    }
}

int main(void)
{
    char welcome[] = "Perspicua Shell v0.2 (Pipe support enabled)\n";
    print_string(welcome);

    char cmd_buffer[128];
    int cmd_length = 0;

    print_string("$ ");

    while (1)
    {
        char c;
        if (sys_read(0, &c, 1) > 0)
        {
            if (c == '\n')
            {
                cmd_buffer[cmd_length] = '\0';
                print_string("\n");

                if (cmd_length > 0)
                {
                    execute_line(cmd_buffer);
                }

                cmd_length = 0;
                print_string("$ ");
            }
            else if (c == '\b' || c == 127)
            {
                if (cmd_length > 0)
                {
                    cmd_length--;
                    print_string("\b \b");
                }
            }
            else
            {
                if (cmd_length < 127)
                {
                    cmd_buffer[cmd_length++] = c;
                    sys_write(1, &c, 1);
                }
            }
        }
    }

    return 0;
}
