#include "syscall.h"
#include "string.h"

#define MAX_ARGS    16
#define CMD_MAX_LEN 256

static void print_string(const char* s)
{
    sys_write(1, s, strlen(s));
}

static void print_char(char c)
{
    sys_write(1, &c, 1);
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

static char current_dir[CMD_MAX_LEN] = "/";

static void set_current_dir(const char* path)
{
    char temp[512];
    if (path[0] == '/') {
        strcpy(temp, path);
    } else {
        strcpy(temp, current_dir);
        if (strcmp(temp, "/") != 0) {
            strcat(temp, "/");
        }
        strcat(temp, path);
    }

    char* parts[64];
    int count = 0;
    char* token = strtok(temp, "/");

    while (token) {
        if (strcmp(token, ".") == 0) {
            // ignore
        } else if (strcmp(token, "..") == 0) {
            if (count > 0) count--;
        } else {
            parts[count++] = token;
        }
        token = strtok(NULL, "/");
    }

    if (count == 0) {
        strcpy(current_dir, "/");
    } else {
        current_dir[0] = '\0';
        for (int i = 0; i < count; i++) {
            strcat(current_dir, "/");
            strcat(current_dir, parts[i]);
        }
    }
}

static void run_command(int argc, char** argv)
{
    if (argc == 0 || !argv[0])
        return;

    if (strcmp(argv[0], "help") == 0)
    {
        print_string("Commands: help, echo, clear, exit, pipes (|), programs in /\n");
        return;
    }

    if (strcmp(argv[0], "clear") == 0)
    {
        print_string("\033[2J\033[H");
        return;
    }

    if (strcmp(argv[0], "echo") == 0)
    {
        for (int i = 1; i < argc; i++)
        {
            print_string(argv[i]);
            if (i < argc - 1)
                print_string(" ");
        }
        print_string("\n");
        return;
    }

    if (strcmp(argv[0], "exit") == 0)
    {
        sys_exit(0);
    }

    if (strcmp(argv[0], "cd") == 0)
    {
        int res = 0;
        if (argc == 1 ){
            sys_chdir("/");
            set_current_dir("/");
            // print_string("Changed directory to /\n");
            return;
        }
        else {
            res = sys_chdir(argv[1]); 
            if (res < 0)
            {
                print_string("sh: cd : no such directory: ");
                print_string(argv[1]);
                print_string("\n");
            }
                else {
                    set_current_dir(argv[1]);
                }
        }
        return;
    }

    char path[128];
    if (argv[0][0] == '/')
    {
        strcpy(path, argv[0]);
    }
    else
    {
        strcpy(path, "/");
        strcat(path, argv[0]);
        strcat(path, ".elf");
    }

    /* Note: Currently, our kernel sys_exec only takes the path and no argv. */
    if (sys_exec(path) < 0)
    {
        /* Try without .elf just in case */
        if (argv[0][0] != '/')
        {
            strcpy(path, "/");
            strcat(path, argv[0]);
            if (sys_exec(path) < 0)
            {
                print_string("sh: command not found: ");
                print_string(argv[0]);
                print_string("\n");
                sys_exit(1);
            }
        }
        else
        {
            print_string("sh: command not found: ");
            print_string(argv[0]);
            print_string("\n");
            sys_exit(1);
        }
    }
}

static void execute_pipeline(char* left_str, char* right_str)
{
    int pipefd[2];
    if (sys_pipe(pipefd) < 0)
    {
        print_string("sh: pipe failed\n");
        return;
    }

    int pid1 = sys_fork();
    if (pid1 == 0)
    {
        sys_dup2(pipefd[1], 1);
        sys_close(pipefd[0]);
        sys_close(pipefd[1]);

        char* argv[MAX_ARGS];
        int argc = 0;
        char* p = left_str;
        while (*p)
        {
            while (*p == ' ')
                *p++ = '\0';
            if (!*p)
                break;
            if (argc < MAX_ARGS - 1)
                argv[argc++] = p;
            while (*p && *p != ' ')
                p++;
        }
        argv[argc] = NULL;
        run_command(argc, argv);
        sys_exit(0);
    }

    int pid2 = sys_fork();
    if (pid2 == 0)
    {
        sys_dup2(pipefd[0], 0);
        sys_close(pipefd[0]);
        sys_close(pipefd[1]);

        char* argv[MAX_ARGS];
        int argc = 0;
        char* p = right_str;
        while (*p)
        {
            while (*p == ' ')
                *p++ = '\0';
            if (!*p)
                break;
            if (argc < MAX_ARGS - 1)
                argv[argc++] = p;
            while (*p && *p != ' ')
                p++;
        }
        argv[argc] = NULL;
        run_command(argc, argv);
        sys_exit(0);
    }

    sys_close(pipefd[0]);
    sys_close(pipefd[1]);
    sys_waitpid(pid1, (void*)0);
    sys_waitpid(pid2, (void*)0);
}

static void execute_line(char* line)
{
    char* pipe_ptr = strchr(line, '|');
    if (pipe_ptr)
    {
        *pipe_ptr = '\0';
        char* left = trim(line);
        char* right = trim(pipe_ptr + 1);
        execute_pipeline(left, right);
    }
    else
    {
        char* argv[MAX_ARGS];
        int argc = 0;
        char* p = line;

        while (*p)
        {
            while (*p == ' ')
                *p++ = '\0';
            if (!*p)
                break;
            if (argc < MAX_ARGS - 1)
                argv[argc++] = p;
            while (*p && *p != ' ')
                p++;
        }
        argv[argc] = NULL;

        if (argc == 0)
            return;

        /* Built-ins that should run in the parent process (like exit, clear, echo, help) */
        if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "clear") == 0 || strcmp(argv[0], "echo") == 0
            || strcmp(argv[0], "help") == 0 || strcmp(argv[0], "cd") == 0)
        {
            run_command(argc, argv);
            return;
        }

        int pid = sys_fork();
        if (pid < 0)
        {
            print_string("sh: fork failed\n");
        }
        else if (pid == 0)
        {
            run_command(argc, argv);
            sys_exit(0);
        }
        else
        {
            int status = 0;
            sys_waitpid(pid, &status);
        }
    }
}

static void print_prompt(void)
{
    print_string("perspicua:");
    print_string(current_dir);
    print_string("$ ");
}

int main(void)
{
    print_string("Perspicua OS Shell\n");
    print_string("Type help to see available commands.\n\n");

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
