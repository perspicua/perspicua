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
    int res = pipe(pipefd);
    assert(res == 0);

    int flags = fcntl(pipefd[0], F_GETFD, 0);
    assert(flags == 0);

    res = fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    assert(res == 0);
    flags = fcntl(pipefd[0], F_GETFD, 0);
    assert(flags == FD_CLOEXEC);

    // Test 2: O_NONBLOCK flag getting and setting
    flags = fcntl(pipefd[0], F_GETFL, 0);
    assert((flags & O_NONBLOCK) == 0); // Should not have nonblock by default

    res = fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    assert(res == 0);
    flags = fcntl(pipefd[0], F_GETFL, 0);
    assert((flags & O_NONBLOCK) != 0);

    // Test non-blocking read
    char buf[10];
    res = read(pipefd[0], buf, sizeof(buf));
    if (res == -1 && errno == EAGAIN) {
        printf("Non-blocking read returned -1 with EAGAIN as expected\n");
    } else {
        printf("ERROR: Non-blocking read returned: %d, errno: %d\n", res, errno);
        return 1;
    }

    printf("All fcntl tests passed!\n");
    return 0;
}
