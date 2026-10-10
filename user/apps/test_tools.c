/*
 * test_tools.c - Runs the command-line tools on known input and checks their output.
 *
 * Each tool is found through execvp, fed stdin through a pipe, and its stdout
 * compared byte for byte. find and du get a small tree under /tt_tmp, and edit
 * is typed at through stdin and judged by the file it saves under /tt_edit;
 * both are removed at the end.
 */

#include <stddef.h>

#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/stat.h"
#include "sys/wait.h"
#include "unistd.h"

#define OUT_MAX 4096
#define TREE    "/tt_tmp"
#define EDIT    "/tt_edit"

// Keys as the console sends them.
#define UP    "\033[A"
#define DOWN  "\033[B"
#define RIGHT "\033[C"
#define LEFT  "\033[D"
#define HOME  "\033[H"
#define END   "\033[F"
#define PGDN  "\033[6~"
#define DEL   "\033[3~"
#define BS    "\177"
#define ENTER "\r"
#define ESC   "\033"
#define SAVE  "\023"
#define QUIT  "\021"
#define FIND  "\006"
#define GOTO  "\007"

static int passed, failed;

static void check(const char *name, int ok)
{
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("  FAIL %s\n", name);
    }
}

// Runs argv with input on stdin; returns the exit status and fills out with stdout.
static int run(const char *input, char *out, char *const argv[])
{
    int in[2], outp[2];
    if (pipe(in) != 0 || pipe(outp) != 0) {
        return -1;
    }
    int pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(outp[1], 1);
        close(in[0]);
        close(in[1]);
        close(outp[0]);
        close(outp[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in[0]);
    close(outp[1]);
    if (input) {
        write(in[1], input, strlen(input));
    }
    close(in[1]);

    // Output past OUT_MAX is drained and dropped, so a program that draws a lot never blocks.
    int n = 0;
    char spill[256];
    for (;;) {
        int room = OUT_MAX - 1 - n;
        int r =
            room > 0 ? read(outp[0], out + n, (size_t)room) : read(outp[0], spill, sizeof(spill));
        if (r <= 0) {
            break;
        }
        if (room > 0) {
            n += r;
        }
    }
    out[n] = '\0';
    close(outp[0]);

    int status = -1;
    waitpid(pid, &status, 0);
    return status;
}

static int outputs(const char *input, const char *expect, char *const argv[])
{
    char out[OUT_MAX];
    int status = run(input, out, argv);
    if (status != 0 || strcmp(out, expect) != 0) {
        printf("  %s: status %d, got \"%s\"\n", argv[0], status, out);
        return 0;
    }
    return 1;
}

static int line_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Sorts the lines of out in place, for tools whose order follows the directory.
static void sort_lines(char *out)
{
    char *lines[32];
    int n = 0;
    for (char *p = strtok(out, "\n"); p && n < 32; p = strtok(NULL, "\n")) {
        lines[n++] = p;
    }
    qsort(lines, (size_t)n, sizeof(char *), line_cmp);
    char sorted[OUT_MAX] = "";
    for (int i = 0; i < n; i++) {
        strcat(sorted, lines[i]);
        strcat(sorted, "\n");
    }
    strcpy(out, sorted);
}

static void write_file(const char *path, int bytes)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    char buf[256];
    memset(buf, 'x', sizeof(buf));
    while (fd >= 0 && bytes > 0) {
        int chunk = bytes < (int)sizeof(buf) ? bytes : (int)sizeof(buf);
        write(fd, buf, (size_t)chunk);
        bytes -= chunk;
    }
    close(fd);
}

static const char *fifteen = "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n";

static void test_head_tail(void)
{
    check("head -n 3", outputs(fifteen, "1\n2\n3\n", (char *[]){"head", "-n", "3", NULL}));
    check("head defaults to 10",
          outputs(fifteen, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n", (char *[]){"head", NULL}));
    check("head -n0", outputs(fifteen, "", (char *[]){"head", "-n0", NULL}));
    check("tail -n 2", outputs(fifteen, "14\n15\n", (char *[]){"tail", "-n", "2", NULL}));
    check("tail keeps a missing final newline missing",
          outputs("a\nb", "b", (char *[]){"tail", "-n", "1", NULL}));
    check("tail of a short input is all of it",
          outputs("a\nb\n", "a\nb\n", (char *[]){"tail", NULL}));
    check("tail -n 0", outputs(fifteen, "", (char *[]){"tail", "-n", "0", NULL}));
}

static void test_sort_uniq(void)
{
    check("sort", outputs("b\na\nc\n", "a\nb\nc\n", (char *[]){"sort", NULL}));
    check("sort -r", outputs("b\na\nc\n", "c\nb\na\n", (char *[]){"sort", "-r", NULL}));
    check("sort -n",
          outputs("10\n9\n-1\n100\n", "-1\n9\n10\n100\n", (char *[]){"sort", "-n", NULL}));
    check("sort ends the last line", outputs("b\na", "a\nb\n", (char *[]){"sort", NULL}));
    check("uniq", outputs("a\na\nb\na\n", "a\nb\na\n", (char *[]){"uniq", NULL}));
    check("uniq -c", outputs("a\na\nb\na\n", "      2 a\n      1 b\n      1 a\n",
                             (char *[]){"uniq", "-c", NULL}));
}

static void test_seq_sleep_clear(void)
{
    check("seq last", outputs(NULL, "1\n2\n3\n", (char *[]){"seq", "3", NULL}));
    check("seq first last", outputs(NULL, "2\n3\n4\n", (char *[]){"seq", "2", "4", NULL}));
    check("seq counts down", outputs(NULL, "5\n3\n1\n", (char *[]){"seq", "5", "-2", "1", NULL}));
    check("seq empty range", outputs(NULL, "", (char *[]){"seq", "3", "1", NULL}));

    char out[OUT_MAX];
    check("seq refuses step 0", run(NULL, out, (char *[]){"seq", "1", "0", "3", NULL}) != 0);
    check("sleep 0", run(NULL, out, (char *[]){"sleep", "0", NULL}) == 0);
    check("sleep 0.05", run(NULL, out, (char *[]){"sleep", "0.05", NULL}) == 0);
    check("sleep refuses words", run(NULL, out, (char *[]){"sleep", "soon", NULL}) != 0);
    check("clear", outputs(NULL, "\033[2J\033[H", (char *[]){"clear", NULL}));
}

static void test_free_uptime(void)
{
    char out[OUT_MAX];
    check("free", run(NULL, out, (char *[]){"free", NULL}) == 0 && strstr(out, "Mem:") != NULL
                      && strstr(out, "MB") != NULL);
    check("uptime", run(NULL, out, (char *[]){"uptime", NULL}) == 0 && strncmp(out, "up ", 3) == 0
                        && strstr(out, "process") != NULL);
}

static void test_ptop(void)
{
    char out[OUT_MAX];
    int status = run(NULL, out, (char *[]){"ptop", "--once", NULL});
    check("ptop --once", status == 0 && strstr(out, " cpu0 [") != NULL
                             && strstr(out, "  PID  PPID") != NULL && strstr(out, "ptop") != NULL
                             && strchr(out, '\033') == NULL);
    check("ptop refuses unknown flags", run(NULL, out, (char *[]){"ptop", "-x", NULL}) != 0);
}

// Returns the file's length with its text in out, or -1 if it cannot be read.
static int read_text(const char *path, char *out, int size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    int n = 0, r;
    while (n < size - 1 && (r = read(fd, out + n, (size_t)(size - 1 - n))) > 0) {
        n += r;
    }
    out[n] = '\0';
    close(fd);
    return n;
}

static void write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        write(fd, text, strlen(text));
        close(fd);
    }
}

// Types keys into edit on a file holding before (none if NULL) and checks what it leaves behind.
static int edits(const char *before, const char *keys, const char *after)
{
    unlink(EDIT "/f.txt");
    if (before) {
        write_text(EDIT "/f.txt", before);
    }
    char out[OUT_MAX], got[OUT_MAX];
    int status = run(keys, out, (char *[]){"edit", EDIT "/f.txt", NULL});
    int n = read_text(EDIT "/f.txt", got, sizeof(got));
    struct stat st;
    int tmp_left = stat(EDIT "/f.txt.tmp", &st) == 0;
    if (status != 0 || n < 0 || strcmp(got, after) != 0 || tmp_left) {
        printf("  edit: status %d, got \"%s\"%s\n", status, n < 0 ? "(no file)" : got,
               tmp_left ? ", f.txt.tmp left behind" : "");
        return 0;
    }
    return 1;
}

// n copies of line, each ending in a newline, with prefix put in front of the one at index at.
static void lines_of(char *out, int n, const char *line, int at, const char *prefix)
{
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i == at) {
            strcat(out, prefix);
        }
        strcat(out, line);
        strcat(out, "\n");
    }
}

static void test_edit(void)
{
    mkdir(EDIT, 0755);

    check("edit types into a new file",
          edits(NULL, "hello" ENTER "\tworld" SAVE QUIT, "hello\n\tworld\n"));
    check("edit saves an empty buffer as an empty file", edits(NULL, SAVE QUIT, ""));
    check("edit joins lines with Backspace and Delete",
          edits("abc\ndef\nghi\n", DOWN BS "X" END DEL SAVE QUIT, "abcXdefghi\n"));
    check("edit keeps the column across a short line",
          edits("abcdefgh\nab\nabcdefgh\n", END DOWN DOWN "X" SAVE QUIT,
                "abcdefgh\nab\nabcdefghX\n"));
    check("edit steps over a UTF-8 character",
          edits("\xc3\xa9\n", RIGHT "y" LEFT LEFT "x" SAVE QUIT, "x\xc3\xa9y\n"));
    check("edit goes to a line",
          edits("1\n2\n3\n4\n", GOTO "3" ENTER "X" SAVE QUIT, "1\n2\nX3\n4\n"));

    check("edit finds as it types",
          edits("one\ntwo\nthree\n", FIND "thr" ENTER "X" SAVE QUIT, "one\ntwo\nXthree\n"));
    check("edit finds the next match, and Esc goes back",
          edits("ab ab ab\n", FIND "ab" DOWN ENTER "X" FIND "ab" DOWN ESC "Y" SAVE QUIT,
                "ab XYab ab\n"));
    check("edit finds backwards round the end",
          edits("ab ab ab\n", FIND "ab" UP ENTER "Z" SAVE QUIT, "ab ab Zab\n"));

    check("edit asks before throwing changes away",
          edits("keep\n", "zz" QUIT SAVE QUIT, "zzkeep\n"));
    check("edit throws changes away on a second Ctrl-Q", edits("keep\n", "zz" QUIT QUIT, "keep\n"));
    check("edit asks again after more typing",
          edits("keep\n", "z" QUIT "y" QUIT SAVE QUIT, "zykeep\n"));
    check("edit ends without saving when its input does", edits("keep\n", "zz", "keep\n"));

    static char before[OUT_MAX], after[OUT_MAX];
    char wide[101];
    memset(wide, 'a', 100);
    wide[100] = '\0';
    lines_of(before, 1, wide, -1, "");
    snprintf(after, sizeof(after), "A%sZ\n", wide);
    check("edit scrolls along a long line", edits(before, END "Z" HOME "A" SAVE QUIT, after));

    lines_of(before, 30, "x", -1, "");
    lines_of(after, 30, "x", 22, "Y");
    check("edit pages down", edits(before, PGDN "Y" SAVE QUIT, after));

    char out[OUT_MAX], got[OUT_MAX];
    unlink(EDIT "/named.txt");
    int status = run("hi" SAVE EDIT "/named.txt" ENTER QUIT, out, (char *[]){"edit", NULL});
    check("edit asks for a name on the first save",
          status == 0 && read_text(EDIT "/named.txt", got, sizeof(got)) >= 0
              && strcmp(got, "hi\n") == 0);

    unlink(EDIT "/named.txt");
    unlink(EDIT "/f.txt");
    rmdir(EDIT);
}

static void test_find_du(void)
{
    mkdir(TREE, 0755);
    mkdir(TREE "/sub", 0755);
    write_file(TREE "/a.txt", 1500);
    write_file(TREE "/sub/b.c", 100);
    write_file(TREE "/sub/c.txt", 3000);

    char out[OUT_MAX];
    int status = run(NULL, out, (char *[]){"find", TREE, "-name", "*.txt", NULL});
    sort_lines(out);
    check("find -name", status == 0 && strcmp(out, TREE "/a.txt\n" TREE "/sub/c.txt\n") == 0);

    status = run(NULL, out, (char *[]){"find", TREE, "-type", "d", NULL});
    sort_lines(out);
    check("find -type d", status == 0 && strcmp(out, TREE "\n" TREE "/sub\n") == 0);

    status = run(NULL, out, (char *[]){"find", TREE, "-name", "?.c", "-type", "f", NULL});
    check("find -name ? -type f", status == 0 && strcmp(out, TREE "/sub/b.c\n") == 0);

    // 1500 + 100 + 3000 bytes is 4600, which is 5 kB rounded up; sub alone is 4.
    check("du -s", outputs(NULL, "5\t" TREE "\n", (char *[]){"du", "-s", TREE, NULL}));
    check("du", outputs(NULL, "4\t" TREE "/sub\n5\t" TREE "\n", (char *[]){"du", TREE, NULL}));

    unlink(TREE "/sub/c.txt");
    unlink(TREE "/sub/b.c");
    unlink(TREE "/a.txt");
    rmdir(TREE "/sub");
    rmdir(TREE);
}

int main(void)
{
    test_head_tail();
    test_sort_uniq();
    test_seq_sleep_clear();
    test_free_uptime();
    test_ptop();
    test_edit();
    test_find_du();

    if (failed) {
        printf("test_tools: %d of %d tests failed\n", failed, passed + failed);
        return 1;
    }
    printf("test_tools: all %d tests passed\n", passed);
    return 0;
}
