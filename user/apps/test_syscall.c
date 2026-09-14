#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <syscall.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include "uapi/errno.h"
#include "uapi/syscalls.h"

void test_pread_pwrite(void)
{
    printf("[ TEST ] Running pread/pwrite cursor tests...\n");

    int fd = open("test_pw.txt", O_CREAT | O_RDWR);
    assert(fd >= 0);

    // 1. Write initial data. Cursor moves to 10.
    int res = write(fd, "0123456789", 10);
    assert(res == 10);

    // 2. pwrite at offset 2. This must NOT move the cursor from 10.
    res = pwrite(fd, "abcde", 5, 2);
    assert(res == 5);

    /* 3. Normal write. Because pwrite didn't move the cursor,
          this should write starting at offset 10. */
    res = write(fd, "XYZ", 3);
    assert(res == 3);

    /* 4. pread from offset 2. Should read "abcde".
          This must NOT move the cursor from 13. */
    char buf[16] = {0};
    res = pread(fd, buf, 5, 2);
    assert(res == 5);
    assert(strcmp(buf, "abcde") == 0);
    // pread must not have moved the cursor: it is still at 13 from step 3.
    assert(lseek(fd, 0, SEEK_CUR) == 13);

    /* 5. Verify the entire file.
          Expected content: "01" + "abcde" + "789" + "XYZ" = "01abcde789XYZ" */
    lseek(fd, 0, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    res = read(fd, buf, 13);
    assert(res == 13);
    assert(strcmp(buf, "01abcde789XYZ") == 0);

    close(fd);
    printf("[ TEST ] pread/pwrite passed!\n");
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
        assert(child_pid > 0);
        int status = -1;
        int res = waitpid(child_pid, &status, 0);
        assert(res == child_pid);
        assert(status == 0); // 0 means the child exited successfully
    }

    printf("[ TEST ] getppid passed!\n");
}

void test_time_syscalls(void)
{
    printf("[ TEST ] Running gettimeofday & clock_gettime tests...\n");

    // 1. Invalid arguments
    assert(gettimeofday(NULL, NULL) < 0);
    assert(clock_gettime(9999, NULL) < 0);

    // 2. Valid gettimeofday
    struct timeval tv;
    memset(&tv, 0, sizeof(tv));
    int res = gettimeofday(&tv, NULL);
    assert(res == 0);
    assert(tv.tv_sec >= 0);
    assert(tv.tv_usec >= 0 && tv.tv_usec < 1000000);

    // 3. Valid clock_gettime
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    res = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(res == 0);
    assert(ts.tv_sec >= 0);
    assert(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000);

    struct timespec ts_real;
    memset(&ts_real, 0, sizeof(ts_real));
    res = clock_gettime(CLOCK_REALTIME, &ts_real);
    assert(res == 0);
    assert(ts_real.tv_sec >= 0);
    assert(ts_real.tv_nsec >= 0 && ts_real.tv_nsec < 1000000000);

    printf("[ TEST ] gettimeofday & clock_gettime passed!\n");
}

void test_nanosleep(void)
{
    printf("[ TEST ] Running nanosleep tests...\n");

    // 1. Invalid inputs
    assert(nanosleep(NULL, NULL) < 0);

    struct timespec invalid_nsec = {0, 2000000000};
    assert(nanosleep(&invalid_nsec, NULL) < 0);

    struct timespec invalid_sec = {-1, 0};
    assert(nanosleep(&invalid_sec, NULL) < 0);

    // 2. Valid short sleep (50 ms)
    struct timespec t_before = {0, 0};
    struct timespec t_after = {0, 0};
    assert(clock_gettime(CLOCK_MONOTONIC, &t_before) == 0);

    struct timespec req = {0, 50000000}; // 50 ms
    assert(nanosleep(&req, NULL) == 0);

    assert(clock_gettime(CLOCK_MONOTONIC, &t_after) == 0);

    unsigned long delta_ms = (unsigned long)(t_after.tv_sec - t_before.tv_sec) * 1000
                             + (unsigned long)(t_after.tv_nsec - t_before.tv_nsec) / 1000000;
    assert(delta_ms >= 40);

    // 3. rem zeroed when non-NULL pre-filled with garbage
    struct timespec rem;
    rem.tv_sec = 1234;
    rem.tv_nsec = 5678;
    struct timespec req_short = {0, 10000000}; // 10 ms
    assert(nanosleep(&req_short, &rem) == 0);
    assert(rem.tv_sec == 0 && rem.tv_nsec == 0);

    // 4. Zero sleep returns immediately
    struct timespec req_zero = {0, 0};
    assert(nanosleep(&req_zero, NULL) == 0);

    printf("[ TEST ] nanosleep passed!\n");
}

void test_fstat(void)
{
    printf("[ TEST ] Running fstat tests...\n");

    struct stat st;
    memset(&st, 0, sizeof(st));

    // 1. Bad fd: unopened or negative
    assert(fstat(-1, &st) < 0);
    assert(fstat(999, &st) < 0);

    // 2. A regular file reports its size and mode
    const char *test_path = "test_fstat.txt";
    int fd = open(test_path, O_CREAT | O_RDWR);
    assert(fd >= 0);

    const char *data = "Hello, fstat!";
    size_t len = strlen(data);
    int res = write(fd, data, len);
    assert(res == (int)len);

    res = fstat(fd, &st);
    assert(res == 0);
    assert(S_ISREG(st.st_mode));
    assert(st.st_size == (uint64_t)len);

    // 3. stat and fstat agree on the same file
    struct stat sp;
    memset(&sp, 0, sizeof(sp));
    res = stat(test_path, &sp);
    assert(res == 0);
    assert(sp.st_size == st.st_size);
    assert(sp.st_mode == st.st_mode);

    // 4. A closed fd is rejected
    close(fd);
    memset(&st, 0, sizeof(st));
    assert(fstat(fd, &st) < 0);

    // Cleanup test file
    unlink(test_path);

    printf("[ TEST ] fstat passed!\n");
}

void test_truncate(void)
{
    printf("[ TEST ] Running truncate / ftruncate & O_TRUNC tests...\n");

    const char *test_path = "test_trunc.txt";
    int fd = open(test_path, O_CREAT | O_RDWR);
    assert(fd >= 0);

    // 1. Setup: write 100 bytes
    char buf100[100];
    memset(buf100, 'A', sizeof(buf100));
    int res = write(fd, buf100, sizeof(buf100));
    assert(res == 100);

    // 2. Shrink
    res = ftruncate(fd, 10);
    assert(res == 0);

    struct stat st;
    memset(&st, 0, sizeof(st));
    assert(fstat(fd, &st) == 0);
    assert(st.st_size == 10);

    lseek(fd, 0, SEEK_SET);
    char rbuf[64] = {0};
    int rbytes = read(fd, rbuf, sizeof(rbuf));
    assert(rbytes == 10);

    // 3. Truncate to zero
    res = ftruncate(fd, 0);
    assert(res == 0);
    assert(fstat(fd, &st) == 0);
    assert(st.st_size == 0);

    // 4. Path-based truncate
    lseek(fd, 0, SEEK_SET);
    assert(write(fd, "0123456789", 10) == 10);
    assert(truncate(test_path, 5) == 0);

    memset(&st, 0, sizeof(st));
    assert(stat(test_path, &st) == 0);
    assert(st.st_size == 5);

    // 5. O_TRUNC empties the file on open
    lseek(fd, 0, SEEK_SET);
    char buf50[50];
    memset(buf50, 'B', sizeof(buf50));
    assert(write(fd, buf50, sizeof(buf50)) == 50);
    close(fd);

    int trunc_fd = open(test_path, O_RDWR | O_TRUNC);
    assert(trunc_fd >= 0);
    memset(&st, 0, sizeof(st));
    assert(fstat(trunc_fd, &st) == 0);
    assert(st.st_size == 0);
    close(trunc_fd);

    // 6. Error handling
    assert(ftruncate(-1, 0) < 0);
    assert(truncate("/nonexistent_file_xyz", 0) < 0);

    int err_fd = open(test_path, O_RDWR);
    assert(err_fd >= 0);
    // Grow attempt returns error in first-cut implementation
    assert(ftruncate(err_fd, 999999) < 0);
    close(err_fd);
    unlink(test_path);

    /* 7. Multiple empty files regression test:
       Create a.txt (empty), create b.txt with data, ftruncate b.txt to 0.
       Verify a.txt size is untouched and b.txt reads back correctly. */
    const char *path_a = "trun_a.txt";
    const char *path_b = "trun_b.txt";
    int fd_a = open(path_a, O_CREAT | O_RDWR);
    assert(fd_a >= 0);
    close(fd_a);

    int fd_b = open(path_b, O_CREAT | O_RDWR);
    assert(fd_b >= 0);
    assert(write(fd_b, "12345678901234567890", 20) == 20);

    assert(ftruncate(fd_b, 0) == 0);

    struct stat st_a, st_b;
    memset(&st_a, 0, sizeof(st_a));
    memset(&st_b, 0, sizeof(st_b));
    assert(stat(path_a, &st_a) == 0);
    assert(stat(path_b, &st_b) == 0);
    assert(st_a.st_size == 0);
    assert(st_b.st_size == 0);

    lseek(fd_b, 0, SEEK_SET);
    char buf_b[16] = {0};
    assert(read(fd_b, buf_b, sizeof(buf_b)) == 0);

    close(fd_b);
    unlink(path_a);
    unlink(path_b);

    printf("[ TEST ] truncate / ftruncate & O_TRUNC passed!\n");
}

void test_procfs(void)
{
    printf("[ TEST ] Running procfs completeness tests...\n");

    // 1. Read /proc/mounts
    int fd = open("/proc/mounts", O_RDONLY);
    assert(fd >= 0);
    char buf[512] = {0};
    int r = read(fd, buf, sizeof(buf) - 1);
    assert(r > 0);
    assert(strstr(buf, "procfs") != NULL);
    close(fd);

    // 2. Read /proc/cpuinfo
    fd = open("/proc/cpuinfo", O_RDONLY);
    assert(fd >= 0);
    memset(buf, 0, sizeof(buf));
    r = read(fd, buf, sizeof(buf) - 1);
    assert(r > 0);
    assert(strstr(buf, "ARM Cortex-A72") != NULL);
    close(fd);

    // 3. Read /proc/stat
    fd = open("/proc/stat", O_RDONLY);
    assert(fd >= 0);
    memset(buf, 0, sizeof(buf));
    r = read(fd, buf, sizeof(buf) - 1);
    assert(r > 0);
    assert(strstr(buf, "ctxt") != NULL);
    assert(strstr(buf, "processes") != NULL);
    close(fd);

    // 4. Read /proc/self/status
    fd = open("/proc/self/status", O_RDONLY);
    assert(fd >= 0);
    memset(buf, 0, sizeof(buf));
    r = read(fd, buf, sizeof(buf) - 1);
    assert(r > 0);
    assert(strstr(buf, "Pid:") != NULL);
    close(fd);

    printf("[ TEST ] procfs completeness passed!\n");
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
        assert(res != 0x1234);
        assert(res == -ENOSYS);
    }

    // The same regular file may be opened more than once at a time.
    {
        const char *path = "dupopen.txt";
        int a = open(path, O_RDWR | O_CREAT | O_TRUNC);
        assert(a >= 0);
        int b = open(path, O_RDONLY);
        assert(b >= 0);
        assert(a != b);
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
        assert(fd >= 0);

        int saved = dup2(1, 9);
        assert(saved >= 0);
        assert(dup2(fd, 1) >= 0);
        printf("%s", big);
        assert(dup2(saved, 1) >= 0);
        close(saved);
        close(fd);

        int rfd = open(path, O_RDONLY);
        assert(rfd >= 0);
        int total = 0, n;
        char buf[256];
        while ((n = read(rfd, buf, sizeof(buf))) > 0) {
            total += n;
        }
        close(rfd);
        unlink(path);
        assert(total == (int)sizeof(big) - 1);
    }

    // A process killed by a bad sigreturn frame must still become a zombie its
    // parent can reap, rather than leaking its slot.
    {
        int pid = fork();
        assert(pid >= 0);
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
        assert(waitpid(pid, &status, 0) == pid);
    }

    printf("[ TEST ] review bug-fix regressions passed!\n");
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

    printf("--- All Syscall Tests Passed! ---\n");
    return 0;
}
