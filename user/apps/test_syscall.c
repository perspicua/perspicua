/*
 * test_syscall.c - Syscall semantics, from file cursors and truncate to bad arguments.
 */

#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <syscall.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <signal.h>
#include <wait.h>
#include "uapi/errno.h"
#include "uapi/syscalls.h"

#include "utest.h"

void test_pread_pwrite(void)
{
    printf("[ TEST ] Running pread/pwrite cursor tests...\n");

    int fd = open("test_pw.txt", O_CREAT | O_RDWR);
    CHECK(fd >= 0);

    // 1. Write initial data. Cursor moves to 10.
    int res = write(fd, "0123456789", 10);
    CHECK(res == 10);

    // 2. pwrite at offset 2. This must NOT move the cursor from 10.
    res = pwrite(fd, "abcde", 5, 2);
    CHECK(res == 5);

    /* 3. Normal write. Because pwrite didn't move the cursor,
          this should write starting at offset 10. */
    res = write(fd, "XYZ", 3);
    CHECK(res == 3);

    /* 4. pread from offset 2. Should read "abcde".
          This must NOT move the cursor from 13. */
    char buf[16] = {0};
    res = pread(fd, buf, 5, 2);
    CHECK(res == 5);
    CHECK(strcmp(buf, "abcde") == 0);
    // pread must not have moved the cursor: it is still at 13 from step 3.
    CHECK(lseek(fd, 0, SEEK_CUR) == 13);

    /* 5. Verify the entire file.
          Expected content: "01" + "abcde" + "789" + "XYZ" = "01abcde789XYZ" */
    lseek(fd, 0, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    res = read(fd, buf, 13);
    CHECK(res == 13);
    CHECK(strcmp(buf, "01abcde789XYZ") == 0);

    close(fd);
    unlink("test_pw.txt");
}

void test_getppid(void)
{
    printf("[ TEST ] Running getppid/fork tests...\n");

    int parent_pid = getpid();
    int child_pid = fork();

    if (child_pid == 0) {
        // We are in the child process
        int my_ppid = getppid();

        if (my_ppid != parent_pid) {
            printf("ERROR: getppid() returned %d, expected %d\n", my_ppid, parent_pid);
            _exit(1);
        }
        _exit(0);
    } else {
        // We are in the parent process
        CHECK(child_pid > 0);
        int status = -1;
        int res = waitpid(child_pid, &status, 0);
        CHECK(res == child_pid);
        CHECK(status == 0); // 0 means the child exited successfully
    }
}

void test_time_syscalls(void)
{
    printf("[ TEST ] Running gettimeofday & clock_gettime tests...\n");

    // 1. Invalid arguments
    CHECK(gettimeofday(NULL, NULL) < 0);
    CHECK(clock_gettime(9999, NULL) < 0);

    // 2. Valid gettimeofday
    struct timeval tv;
    memset(&tv, 0, sizeof(tv));
    int res = gettimeofday(&tv, NULL);
    CHECK(res == 0);
    CHECK(tv.tv_sec >= 0);
    CHECK(tv.tv_usec >= 0 && tv.tv_usec < 1000000);

    // 3. Valid clock_gettime
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    res = clock_gettime(CLOCK_MONOTONIC, &ts);
    CHECK(res == 0);
    CHECK(ts.tv_sec >= 0);
    CHECK(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000);

    struct timespec ts_real;
    memset(&ts_real, 0, sizeof(ts_real));
    res = clock_gettime(CLOCK_REALTIME, &ts_real);
    CHECK(res == 0);
    CHECK(ts_real.tv_sec >= 0);
    CHECK(ts_real.tv_nsec >= 0 && ts_real.tv_nsec < 1000000000);
}

void test_nanosleep(void)
{
    printf("[ TEST ] Running nanosleep tests...\n");

    // 1. Invalid inputs
    CHECK(nanosleep(NULL, NULL) < 0);

    struct timespec invalid_nsec = {0, 2000000000};
    CHECK(nanosleep(&invalid_nsec, NULL) < 0);

    struct timespec invalid_sec = {-1, 0};
    CHECK(nanosleep(&invalid_sec, NULL) < 0);

    // 2. Valid short sleep (50 ms)
    struct timespec t_before = {0, 0};
    struct timespec t_after = {0, 0};
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t_before) == 0);

    struct timespec req = {0, 50000000}; // 50 ms
    CHECK(nanosleep(&req, NULL) == 0);

    CHECK(clock_gettime(CLOCK_MONOTONIC, &t_after) == 0);

    unsigned long delta_ms = (unsigned long)(t_after.tv_sec - t_before.tv_sec) * 1000
                             + (unsigned long)(t_after.tv_nsec - t_before.tv_nsec) / 1000000;
    CHECK(delta_ms >= 40);

    // 3. rem zeroed when non-NULL pre-filled with garbage
    struct timespec rem;
    rem.tv_sec = 1234;
    rem.tv_nsec = 5678;
    struct timespec req_short = {0, 10000000}; // 10 ms
    CHECK(nanosleep(&req_short, &rem) == 0);
    CHECK(rem.tv_sec == 0 && rem.tv_nsec == 0);

    // 4. Zero sleep returns immediately
    struct timespec req_zero = {0, 0};
    CHECK(nanosleep(&req_zero, NULL) == 0);

    // 5. The longest sleep accepted sleeps, rather than overflowing into one that is already over
    int pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        struct timespec longest = {(time_t)(0x7FFFFFFFFFFFFFFFLL / 1000), 999999999};
        nanosleep(&longest, NULL);
        _exit(0);
    }
    usleep(200 * 1000);
    int status = 0;
    CHECK(waitpid(pid, &status, WNOHANG) == 0);
    kill(pid, SIGKILL);
    CHECK(waitpid(pid, &status, 0) == pid);
}

void test_fstat(void)
{
    printf("[ TEST ] Running fstat tests...\n");

    struct stat st;
    memset(&st, 0, sizeof(st));

    // 1. Bad fd: unopened or negative
    CHECK(fstat(-1, &st) < 0);
    CHECK(fstat(999, &st) < 0);

    // 2. A regular file reports its size and mode
    const char *test_path = "test_fstat.txt";
    int fd = open(test_path, O_CREAT | O_RDWR);
    CHECK(fd >= 0);

    const char *data = "Hello, fstat!";
    size_t len = strlen(data);
    int res = write(fd, data, len);
    CHECK(res == (int)len);

    res = fstat(fd, &st);
    CHECK(res == 0);
    CHECK(S_ISREG(st.st_mode));
    CHECK(st.st_size == (uint64_t)len);

    // 3. stat and fstat agree on the same file
    struct stat sp;
    memset(&sp, 0, sizeof(sp));
    res = stat(test_path, &sp);
    CHECK(res == 0);
    CHECK(sp.st_size == st.st_size);
    CHECK(sp.st_mode == st.st_mode);

    // 4. A closed fd is rejected
    close(fd);
    memset(&st, 0, sizeof(st));
    CHECK(fstat(fd, &st) < 0);

    // Cleanup test file
    unlink(test_path);
}

void test_truncate(void)
{
    printf("[ TEST ] Running truncate / ftruncate & O_TRUNC tests...\n");

    const char *test_path = "test_trunc.txt";
    int fd = open(test_path, O_CREAT | O_RDWR);
    CHECK(fd >= 0);

    // 1. Setup: write 100 bytes
    char buf100[100];
    memset(buf100, 'A', sizeof(buf100));
    int res = write(fd, buf100, sizeof(buf100));
    CHECK(res == 100);

    // 2. Shrink
    res = ftruncate(fd, 10);
    CHECK(res == 0);

    struct stat st;
    memset(&st, 0, sizeof(st));
    CHECK(fstat(fd, &st) == 0);
    CHECK(st.st_size == 10);

    lseek(fd, 0, SEEK_SET);
    char rbuf[64] = {0};
    int rbytes = read(fd, rbuf, sizeof(rbuf));
    CHECK(rbytes == 10);

    // 3. Truncate to zero
    res = ftruncate(fd, 0);
    CHECK(res == 0);
    CHECK(fstat(fd, &st) == 0);
    CHECK(st.st_size == 0);

    // 4. Path-based truncate
    lseek(fd, 0, SEEK_SET);
    CHECK(write(fd, "0123456789", 10) == 10);
    CHECK(truncate(test_path, 5) == 0);

    memset(&st, 0, sizeof(st));
    CHECK(stat(test_path, &st) == 0);
    CHECK(st.st_size == 5);

    // 5. O_TRUNC empties the file on open
    lseek(fd, 0, SEEK_SET);
    char buf50[50];
    memset(buf50, 'B', sizeof(buf50));
    CHECK(write(fd, buf50, sizeof(buf50)) == 50);
    close(fd);

    int trunc_fd = open(test_path, O_RDWR | O_TRUNC);
    CHECK(trunc_fd >= 0);
    memset(&st, 0, sizeof(st));
    CHECK(fstat(trunc_fd, &st) == 0);
    CHECK(st.st_size == 0);
    close(trunc_fd);

    // 6. Error handling
    CHECK(ftruncate(-1, 0) < 0);
    CHECK(truncate("/nonexistent_file_xyz", 0) < 0);

    int err_fd = open(test_path, O_RDWR);
    CHECK(err_fd >= 0);
    // Grow attempt returns error in first-cut implementation
    CHECK(ftruncate(err_fd, 999999) < 0);
    close(err_fd);
    unlink(test_path);

    /* 7. Multiple empty files regression test:
       Create a.txt (empty), create b.txt with data, ftruncate b.txt to 0.
       Verify a.txt size is untouched and b.txt reads back correctly. */
    const char *path_a = "trun_a.txt";
    const char *path_b = "trun_b.txt";
    int fd_a = open(path_a, O_CREAT | O_RDWR);
    CHECK(fd_a >= 0);
    close(fd_a);

    int fd_b = open(path_b, O_CREAT | O_RDWR);
    CHECK(fd_b >= 0);
    CHECK(write(fd_b, "12345678901234567890", 20) == 20);

    CHECK(ftruncate(fd_b, 0) == 0);

    struct stat st_a, st_b;
    memset(&st_a, 0, sizeof(st_a));
    memset(&st_b, 0, sizeof(st_b));
    CHECK(stat(path_a, &st_a) == 0);
    CHECK(stat(path_b, &st_b) == 0);
    CHECK(st_a.st_size == 0);
    CHECK(st_b.st_size == 0);

    lseek(fd_b, 0, SEEK_SET);
    char buf_b[16] = {0};
    CHECK(read(fd_b, buf_b, sizeof(buf_b)) == 0);

    close(fd_b);
    unlink(path_a);
    unlink(path_b);
}

void test_procfs(void)
{
    printf("[ TEST ] Running procfs completeness tests...\n");

    // 1. Read /proc/mounts
    int fd = open("/proc/mounts", O_RDONLY);
    CHECK(fd >= 0);
    char buf[512] = {0};
    int r = read(fd, buf, sizeof(buf) - 1);
    CHECK(r > 0);
    CHECK(strstr(buf, "procfs") != NULL);
    close(fd);

    // 2. Read /proc/cpuinfo
    fd = open("/proc/cpuinfo", O_RDONLY);
    CHECK(fd >= 0);
    memset(buf, 0, sizeof(buf));
    r = read(fd, buf, sizeof(buf) - 1);
    CHECK(r > 0);
    CHECK(strstr(buf, "ARM Cortex-A72") != NULL);
    close(fd);

    // 3. Read /proc/stat
    fd = open("/proc/stat", O_RDONLY);
    CHECK(fd >= 0);
    memset(buf, 0, sizeof(buf));
    r = read(fd, buf, sizeof(buf) - 1);
    CHECK(r > 0);
    CHECK(strstr(buf, "ctxt") != NULL);
    CHECK(strstr(buf, "processes") != NULL);
    close(fd);

    // 4. Read /proc/self/status
    fd = open("/proc/self/status", O_RDONLY);
    CHECK(fd >= 0);
    memset(buf, 0, sizeof(buf));
    r = read(fd, buf, sizeof(buf) - 1);
    CHECK(r > 0);
    CHECK(strstr(buf, "Pid:") != NULL);
    close(fd);

    // 5. /proc/self/maps lists every region, even with the region table full. Nothing unmaps,
    // so a child fills its own table and exits 0 only if the listing has a line per region.
    {
        int pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            static char listing[4096];
            int full = 0;
            for (int i = 0; i < 64 && !full; i++) {
                full = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0) == MAP_FAILED;
            }
            int lines = -1;
            int mfd = open("/proc/self/maps", O_RDONLY);
            if (mfd >= 0) {
                int total = 0;
                int n;
                while (total < (int)sizeof(listing) - 1
                       && (n = read(mfd, listing + total, sizeof(listing) - 1 - (size_t)total))
                              > 0) {
                    total += n;
                }
                lines = 0;
                for (int i = 0; i < total; i++) {
                    lines += listing[i] == '\n';
                }
            }
            // mmap is refused once the table holds 64 regions, whatever was there before.
            _exit(full && lines == 64 ? 0 : 1);
        }
        int st = 0;
        CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    // 6. StartTicks tells apart two processes that held the same pid one after the other
    {
        unsigned long start[2] = {0, 0};
        for (int k = 0; k < 2; k++) {
            int gate[2];
            CHECK(pipe(gate) == 0);
            int pid = fork();
            if (pid == 0) {
                char c;
                close(gate[1]);
                read(gate[0], &c, 1);
                _exit(0);
            }
            close(gate[0]);
            char path[48];
            snprintf(path, sizeof(path), "/proc/%d/status", pid);
            fd = open(path, O_RDONLY);
            memset(buf, 0, sizeof(buf));
            if (fd >= 0) {
                read(fd, buf, sizeof(buf) - 1);
                close(fd);
            }
            const char *p = strstr(buf, "StartTicks:");
            for (p = p ? p + 11 : ""; *p == ' '; p++) {}
            for (; *p >= '0' && *p <= '9'; p++) {
                start[k] = start[k] * 10 + (unsigned long)(*p - '0');
            }
            close(gate[1]);
            waitpid(pid, NULL, 0);
        }
        CHECK(start[0] != 0 && start[1] != 0);
        CHECK(start[0] != start[1]);
    }
}

// Issues a syscall directly so an unassigned number can be reached.
static long raw_syscall(long nr, long arg0)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(arg0), "r"(nr)
                 : "x0", "x8", "memory");
    return res;
}

void test_review_bugfixes(void)
{
    printf("[ TEST ] Running review bug-fix regressions...\n");

    // An unassigned syscall number must report ENOSYS rather than handing back
    // whatever the caller happened to leave in x0.
    {
        long res = raw_syscall(999, 0x1234);
        CHECK(res != 0x1234);
        CHECK(res == -ENOSYS);
    }

    // The same regular file may be opened more than once at a time.
    {
        const char *path = "dupopen.txt";
        int a = open(path, O_RDWR | O_CREAT | O_TRUNC);
        CHECK(a >= 0);
        int b = open(path, O_RDONLY);
        CHECK(b >= 0);
        CHECK(a != b);
        close(a);
        close(b);
        unlink(path);
    }

    // printf must not stop at its internal buffer size.
    {
        static char big[1001];
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';

        const char *path = "ptrunc.txt";
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
        CHECK(fd >= 0);

        int saved = dup2(1, 9);
        CHECK(saved >= 0);
        CHECK(dup2(fd, 1) >= 0);
        printf("%s", big);
        CHECK(dup2(saved, 1) >= 0);
        close(saved);
        close(fd);

        int rfd = open(path, O_RDONLY);
        CHECK(rfd >= 0);
        int total = 0, n;
        char buf[256];
        while ((n = read(rfd, buf, sizeof(buf))) > 0) {
            total += n;
        }
        close(rfd);
        unlink(path);
        CHECK(total == (int)sizeof(big) - 1);
    }

    // A process killed by a bad sigreturn frame must still become a zombie its
    // parent can reap, rather than leaking its slot.
    {
        int pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            // sp_el0 == 0 makes the frame pointer fail validation outright.
            asm volatile("mov x9, #0\n"
                         "mov sp, x9\n"
                         "mov x8, %0\n"
                         "svc #0" ::"i"(SYS_SIGRETURN)
                         : "x8", "x9", "memory");
            _exit(0);
        }
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
    }
}

// Entries an exec vector may hold, the terminator not counted.
#define EXEC_LIMIT 127

// 0 when exec refuses with E2BIG, 98 for any other refusal, else the status of the sh that ran.
static int exec_sh_status(char *const argv[], char *const envp[])
{
    int pid = fork();
    if (pid == 0) {
        execve("/bin/sh.elf", argv, envp);
        _exit(errno == E2BIG ? 0 : 98);
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) {
        return -1;
    }
    return WEXITSTATUS(st);
}

static char e2big_arg[1000];

// Regressions from the pre-merge audit.
void test_audit_fixes(void)
{
    printf("[ TEST ] Running audit regressions...\n");

    // An argument vector too large for the new stack is refused while the
    // caller still exists to hear about it.
    {
        static char *argv[101];
        memset(e2big_arg, 'x', sizeof(e2big_arg) - 1);
        for (int i = 0; i < 100; i++) {
            argv[i] = e2big_arg;
        }
        argv[100] = NULL;

        errno = 0;
        CHECK(execve("/bin/wc.elf", argv, NULL) < 0);
        CHECK(errno == E2BIG);
    }

    // Vectors past 127 entries, and entries past the per-entry limit, are refused, never cut short.
    {
        static char *vec[EXEC_LIMIT + 2];
        static char *quiet[] = {"sh", "-c", "exit 7", NULL};
        static char long_arg[1100];
        static char *one[] = {"sh", "-c", "exit 7", long_arg, NULL};

        vec[0] = "sh";
        vec[1] = "-c";
        vec[2] = "exit 7";
        for (int i = 3; i < EXEC_LIMIT + 1; i++) {
            vec[i] = "x";
        }
        vec[EXEC_LIMIT + 1] = NULL;
        memset(long_arg, 'x', sizeof(long_arg) - 1);

        CHECK(exec_sh_status(vec, NULL) == 0);
        CHECK(exec_sh_status(quiet, vec) == 0);
        CHECK(exec_sh_status(one, NULL) == 0);

        vec[EXEC_LIMIT] = NULL;
        CHECK(exec_sh_status(vec, NULL) == 7);
        CHECK(exec_sh_status(quiet, vec) == 7);
    }

    // Every image has to provide it, so the test makes its own regular file.
    const char *plain = "audit_plain.txt";
    int made = open(plain, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(made >= 0);
    close(made);

    // opendir refuses what is not a directory.
    {
        errno = 0;
        CHECK(opendir(plain) == NULL);
        CHECK(errno == ENOTDIR);
    }

    // F_SETFD keeps only the flag it defines.
    {
        int fd = open(plain, O_RDONLY);
        CHECK(fd >= 0);
        CHECK(fcntl(fd, F_SETFD, 0xFF) == 0);
        CHECK(fcntl(fd, F_GETFD, 0) == FD_CLOEXEC);
        close(fd);
    }

    // A buffer too small for the cwd is ERANGE, which tells the caller to grow it.
    {
        char small[1];
        errno = 0;
        CHECK(getcwd(small, sizeof(small)) == NULL && errno == ERANGE);
    }

    // A pipe has no position: seeks and positioned I/O are ESPIPE, and fstat calls it a FIFO.
    {
        int pfd[2];
        CHECK(pipe(pfd) == 0);
        char c = 0;
        struct stat st;
        errno = 0;
        CHECK(lseek(pfd[0], 0, SEEK_CUR) < 0 && errno == ESPIPE);
        errno = 0;
        CHECK(pread(pfd[0], &c, 1, 0) < 0 && errno == ESPIPE);
        errno = 0;
        CHECK(pwrite(pfd[1], &c, 1, 0) < 0 && errno == ESPIPE);
        CHECK(fstat(pfd[1], &st) == 0 && S_ISFIFO(st.st_mode));
        close(pfd[0]);
        close(pfd[1]);
    }

    // An unknown whence is EINVAL.
    {
        int fd = open(plain, O_RDONLY);
        CHECK(fd >= 0);
        errno = 0;
        CHECK(lseek(fd, 0, 99) < 0);
        CHECK(errno == EINVAL);
        close(fd);
    }

    unlink(plain);
}

static int read_proc(const char *path, char *buf, int size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    int n = read(fd, buf, (size_t)(size - 1));
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    return n;
}

// Reads a child's cwd and fd files until it is gone; exits 1 if one ever read as garbage.
static void hammer_proc(int pid)
{
    char cwd_path[32], fd_path[32], buf[64];
    snprintf(cwd_path, sizeof(cwd_path), "/proc/%d/cwd", pid);
    snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd", pid);
    for (int i = 0; i < 200; i++) {
        int n = read_proc(cwd_path, buf, sizeof(buf));
        if (n < 0) {
            break;
        }
        if (n > 0 && strcmp(buf, "/\n") != 0 && strcmp(buf, "/bin\n") != 0) {
            _exit(1);
        }
        DIR *d = opendir(fd_path);
        if (d) {
            while (readdir(d)) {}
            closedir(d);
        }
    }
    _exit(0);
}

// The per-pid procfs files read a process's cwd and descriptors, even while it exits.
static void test_procfs_pid_files(void)
{
    printf("[ TEST ] Running /proc/<pid> cwd and fd tests...\n");

    CHECK(chdir("/") == 0);
    char buf[64];
    CHECK(read_proc("/proc/self/cwd", buf, sizeof(buf)) > 0 && strcmp(buf, "/\n") == 0);

    int fd = open("procfd.txt", O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0);
    char path[32];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    CHECK(read_proc(path, buf, sizeof(buf)) > 0 && strcmp(buf, "/procfd.txt\n") == 0);

    int listed = 0;
    DIR *d = opendir("/proc/self/fd");
    CHECK(d != NULL);
    struct dirent *ent;
    while (d && (ent = readdir(d)) != NULL) {
        listed |= atoi(ent->d_name) == fd;
    }
    if (d) {
        closedir(d);
    }
    CHECK(listed);
    close(fd);
    unlink("procfd.txt");

    int garbled = 0, unreaped = 0;
    for (int round = 0; round < 50; round++) {
        int child = fork();
        if (child == 0) {
            chdir("/bin");
            _exit(0);
        }
        int reader = fork();
        if (reader == 0) {
            hammer_proc(child);
        }
        int status = -1;
        unreaped += waitpid(child, &status, 0) != child;
        garbled += waitpid(reader, &status, 0) != reader || status != 0;
    }
    CHECK(unreaped == 0);
    CHECK(garbled == 0);
}

static int exit_status_of(int code)
{
    int pid = fork();
    if (pid == 0) {
        _exit(code);
    }
    int status = -1;
    waitpid(pid, &status, WUNTRACED);
    return status;
}

// Statuses are 8 bits; waitpid finds children by group; init and live groups are protected.
static void test_wait_and_kill(void)
{
    printf("[ TEST ] Running exit status, waitpid group and kill tests...\n");

    int st = exit_status_of(-1);
    CHECK(!WIFSTOPPED(st) && WIFEXITED(st) && WEXITSTATUS(st) == 255);
    st = exit_status_of(256 + 3);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3);

    int same = fork();
    if (same == 0) {
        _exit(4);
    }
    CHECK(waitpid(0, &st, 0) == same && WEXITSTATUS(st) == 4);

    // The child joins its group itself: it may exit before the parent's setpgid runs.
    int other = fork();
    if (other == 0) {
        setpgid(0, 0);
        _exit(5);
    }
    setpgid(other, other);
    CHECK(waitpid(-other, &st, 0) == other && WEXITSTATUS(st) == 5);
    errno = 0;
    CHECK(waitpid(-other, &st, 0) < 0 && errno == ECHILD);

    // A child of init may signal it, but only signals init handles get through.
    int fds[2];
    CHECK(pipe(fds) == 0);
    int middle = fork();
    if (middle == 0) {
        if (fork() == 0) {
            while (getppid() != 1) {
                usleep(1000);
            }
            int results[2] = {kill(1, SIGTERM), kill(1, SIGKILL)};
            write(fds[1], (const char *)results, sizeof(results));
            _exit(0);
        }
        _exit(0);
    }
    waitpid(middle, &st, 0);
    int results[2] = {-1, -1};
    CHECK(read(fds[0], (char *)results, sizeof(results)) == sizeof(results));
    CHECK(results[0] == 0 && results[1] == 0);
    // A dead init stays a zombie nobody reaps, so its status file outlives it.
    char init_status[256];
    CHECK(read_proc("/proc/1/status", init_status, sizeof(init_status)) > 0
          && strstr(init_status, "running") != NULL);
    close(fds[0]);
    close(fds[1]);

    // A group outlives its leader, so the leader's pid is not handed out while it has members.
    // The leader waits to be told to go, so the member can join a group that exists.
    int go[2];
    CHECK(pipe(go) == 0);
    int leader = fork();
    if (leader == 0) {
        setpgid(0, 0);
        char c;
        read(go[0], &c, 1);
        _exit(0);
    }
    setpgid(leader, leader);
    int member = fork();
    if (member == 0) {
        for (;;) {
            usleep(50 * 1000);
        }
    }
    CHECK(setpgid(member, leader) == 0);
    write(go[1], "x", 1);
    close(go[0]);
    close(go[1]);
    waitpid(leader, &st, 0);
    int next = fork();
    if (next == 0) {
        _exit(0);
    }
    CHECK(next != leader);
    waitpid(next, &st, 0);
    kill(member, SIGKILL);
    waitpid(member, &st, 0);
}

// Bad descriptors, pointers, pids and signals are refused rather than acted on.
static void test_bad_arguments(void)
{
    printf("[ TEST ] Running bad-argument checks...\n");

    char buf[16];
    CHECK(read(-1, buf, sizeof(buf)) < 0);
    CHECK(read(9999, buf, sizeof(buf)) < 0);
    CHECK(write(-1, "test", 4) < 0);
    CHECK(write(9999, "test", 4) < 0);
    CHECK(pread(-1, buf, sizeof(buf), 0) < 0);
    CHECK(pwrite(9999, "test", 4, 0) < 0);
    CHECK(close(-1) < 0);
    CHECK(close(9999) < 0);
    CHECK(getdents(-1, buf, sizeof(buf)) < 0);

    CHECK(write(1, NULL, 10) < 0);
    CHECK(write(1, (void *)0xdeadbeef, 10) < 0);
    CHECK(pwrite(1, NULL, 10, 0) < 0);
    CHECK(open(NULL, 0) < 0);
    CHECK(open((void *)0xffffffffffffffff, 0) < 0);
    CHECK(chdir(NULL) < 0);
    CHECK(stat(NULL, NULL) < 0);
    CHECK(pread(0, buf, 10, -1) < 0);
    CHECK(pwrite(1, "test", 4, -1) < 0);

    CHECK(execve(NULL, NULL, NULL) < 0);
    char *bad_argv[] = {(char *)0xdeadbeef, NULL};
    CHECK(execve("/bin/ls", bad_argv, NULL) < 0);

    int status;
    CHECK(waitpid(-999, &status, 0) < 0);
    CHECK(waitpid(999999, NULL, 0) < 0);

    CHECK(mmap(NULL, 0, 0, 0, -1, 0) == MAP_FAILED);
    CHECK(mmap((void *)0x1000, 0xffffffff, 0, 0, -1, 0) == MAP_FAILED);

    CHECK(signal(-1, SIG_DFL) == SIG_ERR);
    CHECK(signal(999, SIG_DFL) == SIG_ERR);
    CHECK(kill(-1, 0) < 0);
    CHECK(kill(getpid(), -1) < 0);
    CHECK(kill(getpid(), 0) == 0);
    errno = 0;
    CHECK(kill(999999, 0) < 0 && errno == ESRCH);
    CHECK(kill(999999, SIGKILL) < 0);
    CHECK(sigaction(-1, NULL, NULL) < 0);

    // Seconds whose conversion to ms would overflow must be refused, not slept.
    struct timespec huge = {.tv_sec = (time_t)0x7FFFFFFFFFFFFFFFLL, .tv_nsec = 0};
    CHECK(nanosleep(&huge, NULL) < 0);
}

// The kernel's signal_frame: a trap frame, whose spsr_el1 is at byte 24, then the saved mask.
struct forged_frame {
    uint64_t sp_el0, pad, elr, spsr, x[30], x30, pad_end;
    uint32_t mask;
} __attribute__((aligned(16)));

#define SPSR_FLAGS 0xF0000000ULL

uint64_t forged_seen_spsr = ~0ULL;
static char forged_stack[8192] __attribute__((aligned(16)));

// Entered with sp at its signal frame; records the state the interrupted code ran in.
void forged_spsr_handler(int sig);
asm(".pushsection .text\n"
    ".balign 4\n"
    ".global forged_spsr_handler\n"
    ".type forged_spsr_handler, %function\n"
    "forged_spsr_handler:\n"
    "    ldr x1, [sp, #24]\n"
    "    adrp x2, forged_seen_spsr\n"
    "    str x1, [x2, :lo12:forged_seen_spsr]\n"
    "    ret\n"
    ".size forged_spsr_handler, . - forged_spsr_handler\n"
    ".popsection\n");

static void __attribute__((noreturn)) forged_resume(void)
{
    signal(SIGUSR1, forged_spsr_handler);
    kill(getpid(), SIGUSR1);
    _exit((forged_seen_spsr & ~SPSR_FLAGS) == 0 ? 42 : 1);
}

// A frame asking sigreturn for EL1h, masked interrupts, single-step and an illegal state
// must come back as plain EL0.
static void test_forged_sigreturn(void)
{
    printf("[ TEST ] Running forged sigreturn frame...\n");

    int pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        static struct forged_frame frame;
        frame.elr = (uintptr_t)forged_resume;
        frame.sp_el0 = (uintptr_t)(forged_stack + sizeof(forged_stack));
        frame.spsr = 0x203003C5ULL;
        asm volatile("mov sp, %0\n"
                     "mov x8, %1\n"
                     "svc #0" ::"r"(&frame),
                     "i"(SYS_SIGRETURN)
                     : "x8", "memory");
        _exit(2);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);
}

int main(void)
{
    printf("--- Starting Syscall Functional Tests ---\n");

    test_pread_pwrite();
    test_getppid();
    test_time_syscalls();
    test_nanosleep();
    test_fstat();
    test_truncate();
    test_procfs();
    test_review_bugfixes();
    test_audit_fixes();
    test_bad_arguments();
    test_procfs_pid_files();
    test_wait_and_kill();
    test_forged_sigreturn();

    return utest_summary("test_syscall");
}
