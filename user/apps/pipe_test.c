#include "syscall.h"

int main(void)
{
    int pipefd[2];
    int res = sys_pipe(pipefd);
    if (res < 0)
    {
        char msg[] = "Pipe creation failed!\n";
        sys_write(1, msg, sizeof(msg) - 1);
        sys_exit(1);
    }

    int pid = sys_fork();
    if (pid < 0)
    {
        char msg[] = "Fork failed!\n";
        sys_write(1, msg, sizeof(msg) - 1);
        sys_exit(1);
    }

    if (pid == 0)
    {
        /* Child: Read from pipe */
        sys_close(pipefd[1]); /* Close write end */

        char buf[32];
        int n = sys_read(pipefd[0], buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            char msg[] = "Child received: ";
            sys_write(1, msg, sizeof(msg) - 1);
            sys_write(1, buf, n);
            sys_write(1, "\n", 1);
        }
        else
        {
            char msg[] = "Child read failed or EOF!\n";
            sys_write(1, msg, sizeof(msg) - 1);
        }

        sys_close(pipefd[0]);
        sys_exit(0);
    }
    else
    {
        /* Parent: Write to pipe */
        sys_close(pipefd[0]); /* Close read end */

        char msg[] = "Hello from Parent!";
        sys_write(pipefd[1], msg, sizeof(msg) - 1);

        sys_close(pipefd[1]);

        /* Wait for child to finish */
        int status;
        sys_waitpid(pid, &status);

        char done[] = "Parent done.\n";
        sys_write(1, done, sizeof(done) - 1);

        sys_exit(0);
    }

    return 0;
}
