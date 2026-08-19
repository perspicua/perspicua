#include <stdio.h>
#include <syscall.h>
#include <assert.h>
#include <string.h>
#include <errno.h>

void test_pread_pwrite(void)
{
    printf("[ TEST ] Running pread/pwrite cursor tests...\n");

    int fd = sys_open("test_pw.txt", VFS_O_CREAT | VFS_O_RDWR);
    assert(fd >= 0);

    /* 1. Write initial data. Cursor moves to 10. */
    int res = sys_write(fd, "0123456789", 10);
    assert(res == 10);

    /* 2. pwrite at offset 2. This must NOT move the cursor from 10. */
    res = sys_pwrite(fd, "abcde", 5, 2);
    assert(res == 5);

    /* 3. Normal write. Because pwrite didn't move the cursor,
          this should write starting at offset 10. */
    res = sys_write(fd, "XYZ", 3);
    assert(res == 3);

    /* 4. pread from offset 2. Should read "abcde".
          This must NOT move the cursor from 13. */
    char buf[16] = {0};
    res = sys_pread(fd, buf, 5, 2);
    assert(res == 5);
    assert(strcmp(buf, "abcde") == 0);
    /* pread must not have moved the cursor: it is still at 13 from step 3. */
    assert(sys_lseek(fd, 0, VFS_SEEK_CUR) == 13);

    /* 5. Verify the entire file.
          Expected content: "01" + "abcde" + "789" + "XYZ" = "01abcde789XYZ" */
    sys_lseek(fd, 0, VFS_SEEK_SET);
    memset(buf, 0, sizeof(buf));
    res = sys_read(fd, buf, 13);
    assert(res == 13);
    assert(strcmp(buf, "01abcde789XYZ") == 0);

    sys_close(fd);
    printf("[ TEST ] pread/pwrite passed!\n");
}

void test_getppid(void)
{
    printf("[ TEST ] Running getppid/fork tests...\n");

    int parent_pid = sys_getpid();
    int child_pid = sys_fork();

    if (child_pid == 0) {
        // We are in the child process
        int my_ppid = sys_getppid();

        if (my_ppid != parent_pid) {
            printf("ERROR: getppid() returned %d, expected %d\n", my_ppid, parent_pid);
            sys_exit(1);
        }
        sys_exit(0);
    } else {
        // We are in the parent process
        assert(child_pid > 0);
        int status = -1;
        int res = sys_waitpid(child_pid, &status, 0);
        assert(res == child_pid);
        assert(status == 0); // 0 means the child exited successfully
    }

    printf("[ TEST ] getppid passed!\n");
}

void test_time_syscalls(void)
{
    printf("[ TEST ] Running gettimeofday & clock_gettime tests...\n");

    /* 1. Invalid arguments */
    assert(sys_gettimeofday(NULL, NULL) < 0);
    assert(sys_clock_gettime(9999, NULL) < 0);

    /* 2. Valid gettimeofday */
    struct timeval tv;
    memset(&tv, 0, sizeof(tv));
    int res = sys_gettimeofday(&tv, NULL);
    assert(res == 0);
    assert(tv.tv_sec >= 0);
    assert(tv.tv_usec >= 0 && tv.tv_usec < 1000000);

    /* 3. Valid clock_gettime */
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    res = sys_clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(res == 0);
    assert(ts.tv_sec >= 0);
    assert(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000);

    struct timespec ts_real;
    memset(&ts_real, 0, sizeof(ts_real));
    res = sys_clock_gettime(CLOCK_REALTIME, &ts_real);
    assert(res == 0);
    assert(ts_real.tv_sec >= 0);
    assert(ts_real.tv_nsec >= 0 && ts_real.tv_nsec < 1000000000);

    printf("[ TEST ] gettimeofday & clock_gettime passed!\n");
}

void test_nanosleep(void)
{
    printf("[ TEST ] Running nanosleep tests...\n");

    /* 1. Invalid inputs */
    assert(sys_nanosleep(NULL, NULL) < 0);

    struct timespec invalid_nsec = {0, 2000000000};
    assert(sys_nanosleep(&invalid_nsec, NULL) < 0);

    struct timespec invalid_sec = {-1, 0};
    assert(sys_nanosleep(&invalid_sec, NULL) < 0);

    /* 2. Valid short sleep (50 ms) */
    struct timespec t_before = {0, 0};
    struct timespec t_after = {0, 0};
    assert(sys_clock_gettime(CLOCK_MONOTONIC, &t_before) == 0);

    struct timespec req = {0, 50000000}; /* 50 ms */
    assert(sys_nanosleep(&req, NULL) == 0);

    assert(sys_clock_gettime(CLOCK_MONOTONIC, &t_after) == 0);

    unsigned long delta_ms = (unsigned long)(t_after.tv_sec - t_before.tv_sec) * 1000
                             + (unsigned long)(t_after.tv_nsec - t_before.tv_nsec) / 1000000;
    assert(delta_ms >= 40);

    /* 3. rem zeroed when non-NULL pre-filled with garbage */
    struct timespec rem;
    rem.tv_sec = 1234;
    rem.tv_nsec = 5678;
    struct timespec req_short = {0, 10000000}; /* 10 ms */
    assert(sys_nanosleep(&req_short, &rem) == 0);
    assert(rem.tv_sec == 0 && rem.tv_nsec == 0);

    /* 4. Zero sleep returns immediately */
    struct timespec req_zero = {0, 0};
    assert(sys_nanosleep(&req_zero, NULL) == 0);

    printf("[ TEST ] nanosleep passed!\n");
}

void test_fstat(void)
{
    printf("[ TEST ] Running fstat tests...\n");

    struct stat st;
    memset(&st, 0, sizeof(st));

    /* 1. Bad fd: unopened or negative */
    assert(sys_fstat(-1, &st) < 0);
    assert(sys_fstat(999, &st) < 0);

    /* 2. Regular file: open a file you wrote N bytes to, sys_fstat(fd, &st) == 0, assert
     * S_ISREG(st.st_mode) and st.st_size == N */
    const char *test_path = "test_fstat.txt";
    int fd = sys_open(test_path, VFS_O_CREAT | VFS_O_RDWR);
    assert(fd >= 0);

    const char *data = "Hello, fstat!";
    size_t len = strlen(data);
    int res = sys_write(fd, data, len);
    assert(res == (int)len);

    res = sys_fstat(fd, &st);
    assert(res == 0);
    assert(S_ISREG(st.st_mode));
    assert(st.st_size == (uint64_t)len);

    /* 3. Consistency with path stat: sys_stat(path, &sp) and sys_fstat(fd, &sf) on the same file
     * agree on st_size and st_mode */
    struct stat sp;
    memset(&sp, 0, sizeof(sp));
    res = sys_stat(test_path, &sp);
    assert(res == 0);
    assert(sp.st_size == st.st_size);
    assert(sp.st_mode == st.st_mode);

    /* 4. Close and re-check bad fd: after sys_close(fd), sys_fstat(fd, &st) < 0 */
    sys_close(fd);
    memset(&st, 0, sizeof(st));
    assert(sys_fstat(fd, &st) < 0);

    /* Cleanup test file */
    sys_unlink(test_path);

    printf("[ TEST ] fstat passed!\n");
}

int main(void)
{
    printf("--- Starting Syscall Functional Tests ---\n");

    test_pread_pwrite();
    test_getppid();
    test_time_syscalls();
    test_nanosleep();
    test_fstat();

    printf("--- All Syscall Tests Passed! ---\n");
    return 0;
}
