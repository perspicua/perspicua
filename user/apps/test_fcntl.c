#include <stdio.h>
#include <syscall.h>
#include <assert.h>
#include <string.h>
#include <errno.h>

int main(void)
{
    printf("Running fcntl tests...\n");

    // Test 1: FD_CLOEXEC flag getting and setting
    int pipefd[2];
    int res = sys_pipe(pipefd);
    assert(res == 0);

    int flags = sys_fcntl(pipefd[0], VFS_F_GETFD, 0);
    assert(flags == 0);

    res = sys_fcntl(pipefd[0], VFS_F_SETFD, VFS_FD_CLOEXEC);
    assert(res == 0);
    flags = sys_fcntl(pipefd[0], VFS_F_GETFD, 0);
    assert(flags == VFS_FD_CLOEXEC);

    // Test 2: O_NONBLOCK flag getting and setting
    flags = sys_fcntl(pipefd[0], VFS_F_GETFL, 0);
    assert((flags & VFS_O_NONBLOCK) == 0); // Should not have nonblock by default

    res = sys_fcntl(pipefd[0], VFS_F_SETFL, flags | VFS_O_NONBLOCK);
    assert(res == 0);
    flags = sys_fcntl(pipefd[0], VFS_F_GETFL, 0);
    assert((flags & VFS_O_NONBLOCK) != 0);

    // Test non-blocking read
    char buf[10];
    res = sys_read(pipefd[0], buf, sizeof(buf));
    if (res == -1 && errno == EAGAIN) {
        printf("Non-blocking read returned -1 with EAGAIN as expected\n");
    } else {
        printf("ERROR: Non-blocking read returned: %d, errno: %d\n", res, errno);
        return 1;
    }

    printf("All fcntl tests passed!\n");
    return 0;
}
