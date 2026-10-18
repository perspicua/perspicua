/*
 * test_pipe.c - Pipes from the boot task, so every read follows a write that satisfies it.
 */

#include "test.h"

#include "string.h"

#include "uapi/errno.h"
#include "uapi/fcntl.h"
#include "uapi/syscalls.h"

#include "fs/pipe.h"
#include "fs/vfs.h"
#include "sched/process.h"

#define TEST_PIPE_CAPACITY PIPE_BUF_SIZE

static void set_nonblock(int fd)
{
    struct process *p = process_current();
    unsigned long flags = spin_lock_irqsave(&p->fd_lock);
    p->fd_table[fd]->flags |= O_NONBLOCK;
    spin_unlock_irqrestore(&p->fd_lock, flags);
}

static int pattern_ok(const char *buf, size_t len, size_t from)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != (char)((from + i) % 251)) {
            return 0;
        }
    }
    return 1;
}

void test_pipe(void)
{
    TEST_SUITE_BEGIN("Pipe");

    // creation hands back two distinct, valid descriptors
    {
        int fds[2] = {-1, -1};
        TEST_ASSERT_EQ("pipe_create succeeds", pipe_create(fds), 0);
        TEST_ASSERT("read end valid", fds[0] >= 0);
        TEST_ASSERT("write end valid", fds[1] >= 0);
        TEST_ASSERT("ends are distinct", fds[0] != fds[1]);

        TEST_ASSERT_EQ("close read end", vfs_close(fds[0]), 0);
        TEST_ASSERT_EQ("close write end", vfs_close(fds[1]), 0);
    }

    // bytes written come back in order and unmodified
    {
        int fds[2];
        TEST_ASSERT_EQ("create for transfer", pipe_create(fds), 0);

        static const char msg[] = "pipe round trip";
        const int len = (int)sizeof(msg) - 1;

        TEST_ASSERT_EQ("write payload", vfs_write(fds[1], msg, len), len);

        static char buf[64];
        memset(buf, 0, sizeof(buf));
        TEST_ASSERT_EQ("read payload", vfs_read(fds[0], buf, len), len);
        TEST_ASSERT("payload survives round trip", memcmp(buf, msg, len) == 0);

        vfs_close(fds[0]);
        vfs_close(fds[1]);
    }

    // the pipe is a stream: separate writes coalesce for the reader
    {
        int fds[2];
        TEST_ASSERT_EQ("create for stream", pipe_create(fds), 0);

        vfs_write(fds[1], "abc", 3);
        vfs_write(fds[1], "def", 3);

        static char buf[16];
        memset(buf, 0, sizeof(buf));
        TEST_ASSERT_EQ("single read drains both writes", vfs_read(fds[0], buf, 6), 6);
        TEST_ASSERT("stream order preserved", memcmp(buf, "abcdef", 6) == 0);

        vfs_close(fds[0]);
        vfs_close(fds[1]);
    }

    // a partial read leaves the remainder queued
    {
        int fds[2];
        TEST_ASSERT_EQ("create for partial read", pipe_create(fds), 0);

        vfs_write(fds[1], "12345", 5);

        static char buf[8];
        memset(buf, 0, sizeof(buf));
        TEST_ASSERT_EQ("read first two bytes", vfs_read(fds[0], buf, 2), 2);
        TEST_ASSERT("first chunk correct", memcmp(buf, "12", 2) == 0);

        memset(buf, 0, sizeof(buf));
        TEST_ASSERT_EQ("read remainder", vfs_read(fds[0], buf, 3), 3);
        TEST_ASSERT("remainder correct", memcmp(buf, "345", 3) == 0);

        vfs_close(fds[0]);
        vfs_close(fds[1]);
    }

    // reading after the write end closes must report EOF, not block
    {
        int fds[2];
        TEST_ASSERT_EQ("create for EOF", pipe_create(fds), 0);

        vfs_write(fds[1], "tail", 4);
        vfs_close(fds[1]);

        static char buf[16];
        memset(buf, 0, sizeof(buf));
        TEST_ASSERT_EQ("buffered data still readable", vfs_read(fds[0], buf, sizeof(buf)), 4);
        TEST_ASSERT("buffered data intact", memcmp(buf, "tail", 4) == 0);
        TEST_ASSERT_EQ("drained pipe reports EOF", vfs_read(fds[0], buf, sizeof(buf)), 0);

        vfs_close(fds[0]);
    }

    // a full pipe must accept exactly its capacity and give it all back
    {
        int fds[2];
        TEST_ASSERT_EQ("create for capacity", pipe_create(fds), 0);

        static char big[TEST_PIPE_CAPACITY];
        for (int i = 0; i < TEST_PIPE_CAPACITY; i++) {
            big[i] = (char)(i & 0x7F);
        }

        TEST_ASSERT_EQ("write fills capacity", vfs_write(fds[1], big, TEST_PIPE_CAPACITY),
                       TEST_PIPE_CAPACITY);

        static char back[TEST_PIPE_CAPACITY];
        memset(back, 0, sizeof(back));
        TEST_ASSERT_EQ("read drains capacity", vfs_read(fds[0], back, TEST_PIPE_CAPACITY),
                       TEST_PIPE_CAPACITY);
        TEST_ASSERT("capacity payload intact", memcmp(back, big, TEST_PIPE_CAPACITY) == 0);

        vfs_close(fds[0]);
        vfs_close(fds[1]);
    }

    // a write that wraps the ring past its end comes back whole and in order
    {
        int fds[2];
        TEST_ASSERT_EQ("create for wrap", pipe_create(fds), 0);

        static char big[TEST_PIPE_CAPACITY];
        static char back[TEST_PIPE_CAPACITY];
        for (int i = 0; i < TEST_PIPE_CAPACITY; i++) {
            big[i] = (char)(i % 251);
        }

        vfs_write(fds[1], big, 3000);
        vfs_read(fds[0], back, 3000);
        TEST_ASSERT_EQ("wrap: a full write across the end",
                       vfs_write(fds[1], big, TEST_PIPE_CAPACITY), TEST_PIPE_CAPACITY);
        memset(back, 0, sizeof(back));
        TEST_ASSERT_EQ("wrap: all of it reads back", vfs_read(fds[0], back, TEST_PIPE_CAPACITY),
                       TEST_PIPE_CAPACITY);
        TEST_ASSERT("wrap: bytes intact and in order", pattern_ok(back, TEST_PIPE_CAPACITY, 0));
        vfs_write(fds[1], "after", 5);
        TEST_ASSERT("wrap: the next write lands where reads resume",
                    vfs_read(fds[0], back, 5) == 5 && memcmp(back, "after", 5) == 0);

        vfs_close(fds[0]);
        vfs_close(fds[1]);
    }

    // a non-blocking write that fits goes in whole or not at all; a larger one may be partial
    {
        int fds[2];
        TEST_ASSERT_EQ("create for non-blocking writes", pipe_create(fds), 0);
        set_nonblock(fds[1]);
        set_nonblock(fds[0]);

        static char fill[TEST_PIPE_CAPACITY + 1000];
        static char back[TEST_PIPE_CAPACITY + 1000];
        memset(fill, 'f', sizeof(fill));

        TEST_ASSERT_EQ("nonblock: 4000 bytes fit", vfs_write(fds[1], fill, 4000), 4000);
        TEST_ASSERT_EQ("nonblock: 200 more do not, and none land", vfs_write(fds[1], fill, 200),
                       -EAGAIN);
        TEST_ASSERT_EQ("nonblock: only the 4000 are queued", vfs_read(fds[0], back, sizeof(back)),
                       4000);
        TEST_ASSERT_EQ("nonblock: an empty pipe reads EAGAIN", vfs_read(fds[0], back, 1), -EAGAIN);

        TEST_ASSERT_EQ("nonblock: a write past the capacity fills the pipe",
                       vfs_write(fds[1], fill, sizeof(fill)), TEST_PIPE_CAPACITY);
        TEST_ASSERT_EQ("nonblock: a full pipe takes nothing", vfs_write(fds[1], fill, 1), -EAGAIN);

        vfs_close(fds[0]);
        vfs_close(fds[1]);
    }

    // a write with no reader left fails with EPIPE
    {
        int fds[2];
        TEST_ASSERT_EQ("create for EPIPE", pipe_create(fds), 0);
        vfs_close(fds[0]);
        TEST_ASSERT_EQ("no reader: EPIPE", vfs_write(fds[1], "x", 1), -EPIPE);
        vfs_close(fds[1]);
    }

    // a pipe is a FIFO with no position: fstat says so, and seeks and positioned I/O are ESPIPE
    {
        int fds[2];
        TEST_ASSERT_EQ("create for ESPIPE", pipe_create(fds), 0);

        struct stat st;
        TEST_ASSERT("fstat: a FIFO", vfs_fstat(fds[0], &st) == 0 && S_ISFIFO(st.st_mode));

        char c = 0;
        uint64_t seek[6] = {(uint64_t)fds[0], 0, SEEK_SET};
        uint64_t pread[6] = {(uint64_t)fds[0], (uint64_t)(uintptr_t)&c, 1, 0};
        uint64_t pwrite[6] = {(uint64_t)fds[1], (uint64_t)(uintptr_t)&c, 1, 0};
        TEST_ASSERT_EQ("lseek: ESPIPE", test_syscall(SYS_LSEEK, seek), -ESPIPE);
        TEST_ASSERT_EQ("pread: ESPIPE", test_syscall(SYS_PREAD, pread), -ESPIPE);
        TEST_ASSERT_EQ("pwrite: ESPIPE", test_syscall(SYS_PWRITE, pwrite), -ESPIPE);

        vfs_close(fds[0]);
        vfs_close(fds[1]);
    }

    TEST_SUITE_END("Pipe");
}
