/*
 * test_pipe.c - Tests for anonymous pipe creation and byte transfer.
 *
 * Only non-blocking traffic is exercised: a read on an empty pipe with a
 * live writer parks the caller, which cannot be satisfied from a single
 * boot-phase task, so every read here is preceded by a matching write.
 */

#include "test.h"

#include "string.h"

#include "fs/pipe.h"
#include "fs/vfs.h"

/* Mirrors PIPE_BUF_SIZE in fs/pipe.c. */
#define TEST_PIPE_CAPACITY 4096

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
    TEST_PASS("create/close");

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
    TEST_PASS("byte transfer");

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
    TEST_PASS("stream semantics");

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
    TEST_PASS("partial reads");

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
    TEST_PASS("EOF on writer close");

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
    TEST_PASS("full capacity");

    TEST_SUITE_END("Pipe");
}
