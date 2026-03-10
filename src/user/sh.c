#include "lib/syscall.h"
#include "lib/string.h"

static void print(const char* s)
{
    sys_write(1, s, strlen(s));
}

int main(void)
{
    char welcome[] = "Perspicua TEsting Shell v0.1\n";
    print(welcome);

    char cmd_buf[128];
    int cmd_len = 0;

    print("$ ");

    while (1)
    {
        char c;
        if (sys_read(0, &c, 1) > 0)
        {
            if (c == '\n')
            {
                cmd_buf[cmd_len] = '\0';
                print("\n");

                if (cmd_len > 0)
                {
                    if (strcmp(cmd_buf, "help") == 0)
                    {
                        print("Available commands: help, cat, hello\n");
                        print("Type the name of an ELF file to exec it (e.g. /cat.elf)\n");
                    }
                    else
                    {
                        char path[128];
                        if (cmd_buf[0] == '/')
                        {
                            strcpy(path, cmd_buf);
                        }
                        else
                        {
                            strcpy(path, "/");
                            strcat(path, cmd_buf);
                            strcat(path, ".elf");
                        }

                        if (sys_exec(path) < 0)
                        {
                            print("Error: command not found: ");
                            print(path);
                            print("\n");
                        }
                    }
                }

                cmd_len = 0;
                print("$ ");
            }
            else if (c == '\b' || c == 127)
            {
                if (cmd_len > 0)
                {
                    cmd_len--;
                    print("\b \b");
                }
            }
            else
            {
                if (cmd_len < 127)
                {
                    cmd_buf[cmd_len++] = c;
                    sys_write(1, &c, 1);
                }
            }
        }
    }

    return 0;
}
