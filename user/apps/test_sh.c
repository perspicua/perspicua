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

// Runs line and passes when the ps it ends up running lists no zombie.
static int no_zombie(const char *line)
{
    char out[OUT_MAX];
    int status = sh_c(line, out);
    if (status != 0 || !strstr(out, "PID") || strstr(out, "zombie")) {
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

    // A shell reading a pipe or a file runs it as a script and exits at its end.
    EXPECT("echo 'echo inner-ran' | sh", "inner-ran\n", 0);
    EXPECT("echo 'echo l1' > " SCRATCH "; echo 'echo l2; false' >> " SCRATCH "; sh < " SCRATCH
           "; echo $?",
           "l1\nl2\n1\n", 0);

    // & ends a command and runs it in the background, attached to its word or not.
    EXPECT("false & echo $?", "0\n", 0);
    EXPECT("sleep 1& echo b", "b\n", 0);
    EXPECT("echo bg > " SCRATCH " & sleep 1; cat " SCRATCH, "bg\n", 0);
    EXPECT("true && echo list > " SCRATCH " & sleep 1; cat " SCRATCH, "list\n", 0);

    // A digit before a redirection names the descriptor; redirections apply left to right.
    EXPECT("echo x 2> " SCRATCH "; cat " SCRATCH, "x\n", 0);
    EXPECT("nosuchcmd 2> " SCRATCH "; cat " SCRATCH, "sh: command not found: nosuchcmd\n", 0);
    EXPECT("nosuchcmd > " SCRATCH " 2>&1; wc -l < " SCRATCH, "       1\n", 0);
    EXPECT("nosuchcmd 2>&1 > " SCRATCH "; wc -l < " SCRATCH,
           "sh: command not found: nosuchcmd\n       0\n", 0);
    EXPECT("echo a >", "sh: syntax error: > needs a file name\n", 2);

    // The shell's own complaints go to stderr, never down the pipe.
    EXPECT("cat < /tsh_missing | wc -l > " SCRATCH "; cat " SCRATCH,
           "sh: /tsh_missing: No such file or directory\n       0\n", 0);

    // 127 for a command that is not there, 126 for one that is and will not run.
    EXPECT("echo hi > " SCRATCH "; " SCRATCH "; echo $?",
           "sh: " SCRATCH ": Exec format error\n126\n", 0);
    EXPECT("/tsh_none; echo $?", "sh: /tsh_none: No such file or directory\n127\n", 0);

    // true, false and : run in a pipeline, as everything else does.
    EXPECT("echo hi | true; echo $?", "0\n", 0);
    EXPECT("echo hi | false; echo $?", "1\n", 0);
    EXPECT("echo hi | :; echo $?", "0\n", 0);

    // A limit is reported and the command not run, never cut short.
    {
        static char line[256], want[64];
        strcpy(line, "echo");
        for (int i = 1; i <= 35; i++) {
            snprintf(line + strlen(line), sizeof(line) - strlen(line), " %d", i);
        }
        EXPECT(line, "sh: more than 31 arguments\n", 2);

        line[0] = '\0';
        for (int i = 0; i < 17; i++) {
            strcat(line, "true; ");
        }
        EXPECT(line, "sh: more than 16 commands separated by ; and &\n", 2);

        line[0] = '\0';
        for (int i = 0; i < 17; i++) {
            strcat(line, i ? " && true" : "true");
        }
        EXPECT(line, "sh: more than 16 commands joined by && and ||\n", 2);

        static char big[768];
        strcpy(big, "export A=");
        memset(big + 9, 'x', 400);
        strcpy(big + 409, "; echo $A$A$A$A$A$A$A$A$A$A$A");
        EXPECT(big, "sh: line longer than 4095 bytes after expansion\n", 2);

        strcpy(big, "echo ");
        memset(big + 5, 'y', 600);
        strcpy(big + 605, " > " SCRATCH "; sh < " SCRATCH "; echo $?");
        snprintf(want, sizeof(want), "sh: line longer than %d bytes\n2\n", 511);
        EXPECT(big, want, 0);
    }

    // A background job that ends while a foreground one runs is reaped then, not at the next prompt.
    EXPECT("echo 'sleep 1' > " SCRATCH "; echo ps >> " SCRATCH, "", 0);
    utest_check(no_zombie("sleep 0 &; sh < " SCRATCH),
                "background job reaped during a foreground one", __LINE__);
    unlink(SCRATCH);

    return utest_summary("test_sh");
}
