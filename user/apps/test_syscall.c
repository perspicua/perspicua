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

int main(void)
{
    printf("--- Starting Syscall Functional Tests ---\n");
    
    test_pread_pwrite();
    test_getppid();

    printf("--- All Syscall Tests Passed! ---\n");
    return 0;
}
