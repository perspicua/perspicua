/*
 * test_fcntl.c - fcntl descriptor and status flags.
 */

#include <stdio.h>
#include <syscall.h>
#include <string.h>
#include <errno.h>

#include "utest.h"

int main(void)
{
    // Test 1: FD_CLOEXEC flag getting and setting
    int pipefd[2];
    int res = pipe(pipefd);
    CHECK(res == 0);

    int flags = fcntl(pipefd[0], F_GETFD, 0);
    CHECK(flags == 0);

    res = fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    CHECK(res == 0);
    flags = fcntl(pipefd[0], F_GETFD, 0);
    CHECK(flags == FD_CLOEXEC);

    // Test 2: O_NONBLOCK flag getting and setting
    flags = fcntl(pipefd[0], F_GETFL, 0);
    CHECK((flags & O_NONBLOCK) == 0); // Should not have nonblock by default

    res = fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    CHECK(res == 0);
    flags = fcntl(pipefd[0], F_GETFL, 0);
    CHECK((flags & O_NONBLOCK) != 0);

    // An empty non-blocking pipe reports EAGAIN instead of blocking.
    char buf[10];
    errno = 0;
    res = read(pipefd[0], buf, sizeof(buf));
    CHECK(res == -1 && errno == EAGAIN);

    close(pipefd[0]);
    close(pipefd[1]);
    return utest_summary("test_fcntl");
}
