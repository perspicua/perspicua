/*
 * test_sh.c - Runs lines through sh -c and checks their output and exit status.
 */

#include "stdio.h"
#include "string.h"
#include "sys/wait.h"
#include "unistd.h"

#include "utest.h"

#define OUT_MAX 1024
#define SCRATCH "/tsh_file"

#define EXPECT(line, want, status) utest_check(runs(line, want, status), line, __LINE__)

// Runs sh -c line with stdout and stderr on one pipe; returns its exit status.
static int sh_c(const char *line, char *out)
{
    int outp[2];
    if (pipe(outp) != 0) {
        return -1;
    }
    int pid = fork();
    if (pid == 0) {
        dup2(outp[1], 1);
        dup2(outp[1], 2);
        close(outp[0]);
        close(outp[1]);
        execvp("sh", (char *[]){"sh", "-c", (char *)line, NULL});
        _exit(127);
    }
    close(outp[1]);

    int n = 0, r;
    while (n < OUT_MAX - 1 && (r = read(outp[0], out + n, (size_t)(OUT_MAX - 1 - n))) > 0) {
        n += r;
    }
    out[n] = '\0';
    close(outp[0]);

    int status = -1;
    waitpid(pid, &status, 0);
    return status;
}

static int runs(const char *line, const char *want, int want_status)
{
    char out[OUT_MAX];
    int status = sh_c(line, out);
    if (status != want_status || strcmp(out, want) != 0) {
        printf("  sh -c '%s': status %d, got \"%s\"\n", line, status, out);
        return 0;
    }
    return 1;
}

int main(void)
{
    EXPECT("echo hello", "hello\n", 0);
    EXPECT("echo 'a  b' \"c  d\"", "a  b c  d\n", 0);
    EXPECT("echo a # b", "a\n", 0);
    EXPECT("export X=42; echo $X ${X}", "42 42\n", 0);

    EXPECT("false; echo $?", "1\n", 0);
    EXPECT("true && echo yes || echo no", "yes\n", 0);
    EXPECT("false && echo yes || echo no", "no\n", 0);
    EXPECT("exit 3", "", 3);
    EXPECT("false; exit", "", 1);
    EXPECT("nosuchcmd; echo $?", "sh: command not found: nosuchcmd\n127\n", 0);

    EXPECT("seq 1 3 | head -n 1", "1\n", 0);
    EXPECT("echo one > " SCRATCH "; echo two >> " SCRATCH "; cat < " SCRATCH, "one\ntwo\n", 0);
    EXPECT("cd /bin; pwd", "/bin\n", 0);
    unlink(SCRATCH);

    return utest_summary("test_sh");
}
