
#include "syscall.h"
#include "string.h"

static void print_string(const char* s)
{
    sys_write(1, s, strlen(s));
}

int main(void)
{
    char welcome[] = "Perspicua Testing Shell v0.1\n";
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
                /* Process the command when newline is received */
                cmd_buffer[cmd_length] = '\0';
                print_string("\n");

                if (cmd_length > 0)
                {
                    if (strcmp(cmd_buffer, "help") == 0)
                    {
                        print_string("Available commands: help, cat, hello\n");
                        print_string("Type the name of an ELF file to exec it (e.g. /cat.elf)\n");
                    }
                    else
                    {
                        char path[128];
                        if (cmd_buffer[0] == '/')
                        {
                            strcpy(path, cmd_buffer);
                        }
                        else
                        {
                            strcpy(path, "/");
                            strcat(path, cmd_buffer);
                            strcat(path, ".elf");
                        }

                        int pid = sys_fork();
                        if (pid < 0)
                        {
                            print_string("Error: fork failed\n");
                        }
                        else if (pid == 0)
                        {
                            /* Child process: execute the requested program */
                            if (sys_exec(path) < 0)
                            {
                                print_string("Error: command not found: ");
                                print_string(path);
                                print_string("\n");
                                sys_exit(1);
                            }
                        }
                        else
                        {
                            /* Parent process: wait for the program to exit */
                            int status = 0;
                            sys_waitpid(pid, &status);
                        }
                    }
                }

                cmd_length = 0;
                print_string("$ ");
            }
            else if (c == '\b' || c == 127)
            {
                /* Handle backspace by erasing the last character */
                if (cmd_length > 0)
                {
                    cmd_length--;
                    print_string("\b \b");
                }
            }
            else
            {
                /* Buffer the character and echo it back to the user */
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
