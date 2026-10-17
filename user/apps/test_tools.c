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
#include "signal.h"
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

static void test_clock(void)
{
    char out[OUT_MAX];
    int status = run(NULL, out, (char *[]){"cat", "/proc/clock", NULL});
    check("/proc/clock",
          status == 0 && strstr(out, "arm_mhz ") != NULL && strstr(out, "throttled 0x") != NULL);
    status = run(NULL, out, (char *[]){"bench", "syscall", NULL});
    check("bench heads its table with the cpu clock",
          status == 0 && strncmp(out, "  cpu ", 6) == 0 && strstr(out, " MHz") != NULL);
}

static void test_kfetch(void)
{
    char out[OUT_MAX];
    int status = run(NULL, out, (char *[]){"kfetch", NULL});
    check("kfetch prints plain lines through a pipe",
          status == 0 && strncmp(out, "root@perspicua\n", 15) == 0
              && strstr(out, "\nOS: Perspicua ") != NULL && strstr(out, "\nKernel: ") != NULL
              && strstr(out, "\nCPU: ") != NULL && strstr(out, "\nMemory: ") != NULL
              && strchr(out, '\033') == NULL);
    check("/proc/cpuinfo names the board",
          run(NULL, out, (char *[]){"cat", "/proc/cpuinfo", NULL}) == 0
              && strstr(out, "\nModel\t\t: ") != NULL);
    check("/proc/version names the build",
          run(NULL, out, (char *[]){"cat", "/proc/version", NULL}) == 0
              && strncmp(out, "Perspicua version ", 18) == 0 && strstr(out, " built ") != NULL);
}

static void test_mandel(void)
{
    char out[OUT_MAX];
    check("mandel: 1 worker and 4 draw the same screen",
          run(NULL, out, (char *[]){"mandel", "--check", NULL}) == 0
              && strstr(out, "check passed") != NULL);
    check("/proc/fb", run(NULL, out, (char *[]){"cat", "/proc/fb", NULL}) == 0
                          && strstr(out, "width ") != NULL && strstr(out, "pitch ") != NULL);
}

static void test_ptop(void)
{
    char out[OUT_MAX];
    int status = run(NULL, out, (char *[]){"ptop", "--once", NULL});
    check("ptop --once", status == 0 && strstr(out, "\u256d\u2500 cpu ") != NULL
                             && strstr(out, " mem ") != NULL && strstr(out, " proc ") != NULL
                             && strstr(out, "PID") != NULL && strstr(out, "ptop") != NULL
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

    check("edit keeps a file of one newline", edits("\n", SAVE QUIT, "\n"));

    write_text(EDIT "/exists.txt", "keep\n");
    run("hi" SAVE EDIT "/exists.txt" ENTER "n" QUIT QUIT, out, (char *[]){"edit", NULL});
    check("edit's save as leaves a file it was told not to overwrite",
          read_text(EDIT "/exists.txt", got, sizeof(got)) >= 0 && strcmp(got, "keep\n") == 0);
    run("hi" SAVE EDIT "/exists.txt" ENTER "y" QUIT, out, (char *[]){"edit", NULL});
    check("edit's save as overwrites once told to",
          read_text(EDIT "/exists.txt", got, sizeof(got)) >= 0 && strcmp(got, "hi\n") == 0);

    unlink(EDIT "/second.txt");
    run("hi" SAVE "/tt_no_dir/x.txt" ENTER SAVE EDIT "/second.txt" ENTER QUIT, out,
        (char *[]){"edit", NULL});
    check("edit asks for a name again after a save under one failed",
          read_text(EDIT "/second.txt", got, sizeof(got)) >= 0 && strcmp(got, "hi\n") == 0);

    unlink(EDIT "/second.txt");
    unlink(EDIT "/exists.txt");
    unlink(EDIT "/named.txt");
    unlink(EDIT "/f.txt");
    rmdir(EDIT);
}

#define LESS_FILE "/tt_less.txt"
#define MATCH     "\033[0;30;43m"

#define MANY "/tt_many"

// Past the old fixed sizes: ls kept 512 names, tree 256, and uniq compared 1023 bytes of a line.
static void test_limits(void)
{
    char out[OUT_MAX], path[32];
    mkdir(MANY, 0755);
    for (int i = 0; i < 520; i++) {
        snprintf(path, sizeof(path), MANY "/f%03d", i);
        write_text(path, "");
    }
    check("ls lists every name in a big directory",
          outputs(NULL, "     520\n", (char *[]){"sh", "-c", "ls " MANY " | wc -l", NULL}));
    check("tree counts every name in a big directory",
          outputs(NULL, "0 directories, 520 files\n",
                  (char *[]){"sh", "-c", "tree " MANY " | tail -n 1", NULL}));
    for (int i = 0; i < 520; i++) {
        snprintf(path, sizeof(path), MANY "/f%03d", i);
        unlink(path);
    }
    rmdir(MANY);

    static char lines[3200];
    memset(lines, 'A', 1500);
    strcpy(lines + 1500, "x\n");
    memset(lines + 1502, 'A', 1500);
    strcpy(lines + 3002, "y\n");
    int status = run(lines, out, (char *[]){"uniq", NULL});
    check("uniq compares long lines whole", status == 0 && strcmp(out, lines) == 0);
}

static void test_less(void)
{
    char out[OUT_MAX];

    // A line 30 rows down wraps inside "needle"; only a found match scrolls up, lit across the wrap.
    static char text[512];
    text[0] = '\0';
    for (int i = 0; i < 30; i++) {
        strcat(text, "x\n");
    }
    size_t at = strlen(text);
    memset(text + at, 'a', 77);
    strcpy(text + at + 77, "needle\n");
    for (int i = 0; i < 30; i++) {
        strcat(text, "x\n");
    }
    write_text(LESS_FILE, text);
    int status = run("/needle" ENTER "q", out, (char *[]){"less", LESS_FILE, NULL});
    check("less finds a match across a wrapped row, and lights both halves",
          status == 0 && strstr(out, MATCH "nee\033[2;1Hdle") != NULL);

    // After a resize to 40 columns the 60-column line takes two rows, prompt open or not.
    memset(text, 'b', 60);
    strcpy(text + 60, "\n");
    write_text(LESS_FILE, text);
    status = run("/\033[24;40R" ESC "q", out, (char *[]){"less", LESS_FILE, NULL});
    check("less wraps again on a resize during the search prompt",
          status == 0 && strstr(out, "\033[2;1Hbbbbbbbbbbbbbbbbbbbb") != NULL);

    write_text(LESS_FILE, "abc\xc2\x9b"
                          "31mRED\n");
    status = run("q", out, (char *[]){"less", LESS_FILE, NULL});
    check("less shows a C1 control as ?, never sends it",
          status == 0 && strstr(out, "abc?31mRED") != NULL && strstr(out, "\xc2\x9b") == NULL);

    unlink(LESS_FILE);
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

#define FS "/tt_fs"

static int status_of(char *const argv[])
{
    char out[OUT_MAX];
    return run(NULL, out, argv);
}

static int holds(const char *path, const char *text)
{
    char buf[256];
    return read_text(path, buf, sizeof(buf)) >= 0 && strcmp(buf, text) == 0;
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void test_cp_mv(void)
{
    mkdir(FS, 0755);
    write_text(FS "/a", "hello\n");

    // cp truncates its destination, so one that is the source must be refused untouched.
    check("cp a a refused", status_of((char *[]){"cp", FS "/a", FS "/a", NULL}) == 1);
    check("cp a a keeps a", holds(FS "/a", "hello\n"));
    check("cp a A refused on FAT", status_of((char *[]){"cp", FS "/a", FS "/A", NULL}) == 1);
    check("cp a into its own dir refused", status_of((char *[]){"cp", FS "/a", FS, NULL}) == 1);
    check("cp a through .. refused",
          status_of((char *[]){"cp", FS "/a", FS "/../" FS "/a", NULL}) == 1);
    check("a survives every refusal", holds(FS "/a", "hello\n"));
    check("cp a b", status_of((char *[]){"cp", FS "/a", FS "/b", NULL}) == 0);
    check("cp a b copies", holds(FS "/b", "hello\n"));

    check("mv b b", status_of((char *[]){"mv", FS "/b", FS "/b", NULL}) == 0);
    check("mv b b keeps b", holds(FS "/b", "hello\n"));

    unlink(FS "/b");
}

static void test_failure_status(void)
{
    char out[OUT_MAX];

    check("cat of a missing file fails",
          run(NULL, out, (char *[]){"cat", FS "/missing", NULL}) == 1 && out[0] == '\0');
    check("cat goes on past a missing file",
          run(NULL, out, (char *[]){"cat", FS "/missing", FS "/a", NULL}) == 1
              && strcmp(out, "hello\n") == 0);
    check("ls of a missing path fails", status_of((char *[]){"ls", FS "/missing", NULL}) == 1);

    // Each "/." adds two bytes, so the entries' paths overrun ls's 512-byte buffer.
    char deep[600] = FS;
    for (int i = 0; i < 260; i++) {
        strcat(deep, "/.");
    }
    check("ls -l on a path too long for its buffer fails cleanly",
          status_of((char *[]){"ls", "-l", deep, NULL}) == 1);

    check("mkdir -p under a file fails",
          status_of((char *[]){"mkdir", "-p", FS "/a/x", NULL}) == 1);
    check("mkdir -p", status_of((char *[]){"mkdir", "-p", FS "/d1/d2", NULL}) == 0);
    check("mkdir -p made the tree", is_dir(FS "/d1/d2"));
    check("mkdir -p of an existing tree",
          status_of((char *[]){"mkdir", "-p", FS "/d1/d2", NULL}) == 0);

    check("rm -r refuses .", status_of((char *[]){"rm", "-r", FS "/d1/.", NULL}) == 1);
    check("rm -r refuses ..", status_of((char *[]){"rm", "-r", FS "/d1/d2/..", NULL}) == 1);
    check("rm -r refuses ../", status_of((char *[]){"rm", "-r", FS "/d1/d2/../", NULL}) == 1);
    check("the refusals left d1 alone", is_dir(FS "/d1/d2"));
    check("rm -r", status_of((char *[]){"rm", "-r", FS "/d1", NULL}) == 0);
    check("rm -r removed d1", !is_dir(FS "/d1"));
}

static void test_grep(void)
{
    char out[OUT_MAX];

    check("grep rejects an unknown option",
          run("a\n", out, (char *[]){"grep", "-v", "zzz", NULL}) == 2 && out[0] == '\0');
    check("grep of a missing file is an error",
          status_of((char *[]){"grep", "x", FS "/missing", NULL}) == 2);
    check("grep with no match", status_of((char *[]){"grep", "zzz", FS "/a", NULL}) == 1);

    static char longline[2100];
    memset(longline, 'a', 2000);
    strcpy(longline + 2000, "NEEDLE\n");
    check("grep finds a match past 1 KB into a line",
          outputs(longline, longline, (char *[]){"grep", "NEEDLE", NULL}));
    check("grep -i finds it too",
          outputs(longline, longline, (char *[]){"grep", "-i", "needle", NULL}));
}

static void test_kill(void)
{
    char pid[16];

    int victim = fork();
    if (victim == 0) {
        for (;;) {
            usleep(50 * 1000);
        }
    }
    snprintf(pid, sizeof(pid), "%d", victim);
    check("kill -NOPE is refused", status_of((char *[]){"kill", "-NOPE", pid, NULL}) == 1);
    check("kill abc is refused", status_of((char *[]){"kill", "abc", NULL}) == 1);
    int st = -1;
    check("the refusals sent nothing", waitpid(victim, &st, WNOHANG) == 0);
    check("kill -TERM by name", status_of((char *[]){"kill", "-TERM", pid, NULL}) == 0);
    check("SIGTERM arrived", waitpid(victim, &st, 0) == victim && st == 128 + SIGTERM);

    victim = fork();
    if (victim == 0) {
        for (;;) {
            usleep(50 * 1000);
        }
    }
    snprintf(pid, sizeof(pid), "%d", victim);
    check("kill -s SIGKILL", status_of((char *[]){"kill", "-s", "SIGKILL", pid, NULL}) == 0);
    check("SIGKILL arrived", waitpid(victim, &st, 0) == victim && st == 128 + SIGKILL);
}

// Each game checks its own rules with --selftest, without touching the terminal.
static char game_out[1 << 18];
static int game_len;

// Reads what is there without waiting; 0 once the writer is gone.
static int drain(int fd)
{
    for (;;) {
        int room = (int)sizeof(game_out) - game_len;
        char spill[256];
        int r =
            room > 0 ? read(fd, game_out + game_len, (size_t)room) : read(fd, spill, sizeof(spill));
        if (r == 0) {
            return 0;
        }
        if (r < 0) {
            return 1;
        }
        if (room > 0) {
            game_len += r;
        }
    }
}

// Starts game with stdin and stdout on pipes, its output drained into game_out as it runs.
static int spawn_game(const char *game, int *in_fd, int *out_fd)
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
        execvp(game, (char *[]){(char *)game, NULL});
        _exit(127);
    }
    close(in[0]);
    close(outp[1]);
    fcntl(outp[0], F_SETFL, O_NONBLOCK);
    game_len = 0;
    *in_fd = in[1];
    *out_fd = outp[0];
    return pid;
}

static void drain_for(int fd, int ms)
{
    for (int t = 0; t < ms; t += 10) {
        drain(fd);
        usleep(10000);
    }
}

// Waits for the game to end, keeping its output; its exit status.
static int finish_game(int pid, int in_fd, int out_fd)
{
    close(in_fd);
    while (drain(out_fd)) {
        usleep(10000);
    }
    close(out_fd);
    int status = -1;
    waitpid(pid, &status, 0);
    game_out[game_len < (int)sizeof(game_out) ? game_len : (int)sizeof(game_out) - 1] = '\0';
    return status;
}

// Pauses game, resumes it with SIGCONT a few times, and reports whether every repaint matched.
static int stays_paused(const char *game)
{
    int in, outp;
    int pid = spawn_game(game, &in, &outp);
    if (pid < 0) {
        return 0;
    }

    write(in, "p", 1);
    drain_for(outp, 300);
    for (int k = 0; k < 8; k++) {
        kill(pid, SIGCONT);
        drain_for(outp, 100);
    }
    write(in, "q", 1);
    int status = finish_game(pid, in, outp);

    // Each resume repaints the whole screen after a clear; a paused board repaints the same.
    const char *first = NULL, *last = NULL;
    size_t first_len = 0, last_len = 0;
    int repaints = 0;
    for (char *p = strstr(game_out, "\033[2J"); p;) {
        char *next = strstr(p + 4, "\033[2J");
        size_t len = next ? (size_t)(next - p) : strlen(p);
        if (len > 200) {
            repaints++;
            if (repaints == 2) {
                first = p;
                first_len = len;
            }
            last = p;
            last_len = len;
        }
        p = next;
    }
    return status == 0 && repaints >= 9 && first && first_len == last_len
           && memcmp(first, last, first_len) == 0;
}

// Kills a running game with sig: the terminal must come back, cursor and line wrap on.
static int restores_on(int sig)
{
    int in, outp;
    int pid = spawn_game("snake", &in, &outp);
    if (pid < 0) {
        return 0;
    }
    drain_for(outp, 300);
    kill(pid, sig);
    int status = finish_game(pid, in, outp);
    const char *release = "\033[0m\033[2J\033[H\033[?25h\033[?7h";
    size_t n = strlen(release);
    return WEXITSTATUS(status) == 128 + sig && (size_t)game_len >= n
           && memcmp(game_out + game_len - n, release, n) == 0;
}

static void test_game_selftests(void)
{
    check("2048 --selftest",
          outputs(NULL, "2048 selftest: ok (0 failed)\n", (char *[]){"2048", "--selftest", NULL}));
    check("snake --selftest", outputs(NULL, "snake selftest: ok (0 failed)\n",
                                      (char *[]){"snake", "--selftest", NULL}));
    check("tetris --selftest", outputs(NULL, "tetris selftest: ok (0 failed)\n",
                                       (char *[]){"tetris", "--selftest", NULL}));
    check("mines --selftest", outputs(NULL, "mines selftest: ok (0 failed)\n",
                                      (char *[]){"mines", "--selftest", NULL}));
    check("snake stays put while paused, however often it is resumed", stays_paused("snake"));
    check("tetris stays put while paused, however often it is resumed", stays_paused("tetris"));
    check("a term program killed by SIGQUIT gives the terminal back", restores_on(SIGQUIT));
    check("a term program killed by SIGHUP gives the terminal back", restores_on(SIGHUP));
    check("a term program killed by SIGSEGV gives the terminal back", restores_on(SIGSEGV));
}

int main(void)
{
    test_head_tail();
    test_sort_uniq();
    test_seq_sleep_clear();
    test_free_uptime();
    test_clock();
    test_kfetch();
    test_mandel();
    test_ptop();
    test_edit();
    test_less();
    test_limits();
    test_find_du();
    test_game_selftests();
    test_cp_mv();
    test_failure_status();
    test_grep();
    test_kill();

    unlink(FS "/a");
    rmdir(FS);

    if (failed) {
        printf("test_tools: %d of %d tests failed\n", failed, passed + failed);
        return 1;
    }
    printf("test_tools: all %d tests passed\n", passed);
    return 0;
}
