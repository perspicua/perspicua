/*
 * test_fat32.c - Tests for FAT32 behaviour beyond the generic VFS paths.
 *
 * test_vfs already covers single-cluster create/read/write through the VFS.
 * This suite targets the filesystem-specific machinery: cluster chains for
 * files larger than one cluster, nested directory traversal, and truncation
 * releasing a chain. Everything it creates is removed before it returns.
 */

#include "test.h"

#include "string.h"

#include "fs/fat32.h"
#include "fs/vfs.h"

#define BIG_FILE  "/tfatbig.tmp"
#define NEST_DIR  "/tfatd"
#define NEST_SUB  "/tfatd/sub"
#define NEST_FILE "/tfatd/sub/deep.txt"

/* Comfortably larger than any plausible cluster size for a 32 MB volume. */
#define BIG_SIZE 16384

static uint8_t big_pattern[BIG_SIZE];
static uint8_t big_readback[BIG_SIZE];

void test_fat32(void)
{
    TEST_SUITE_BEGIN("FAT32");

    // the root node the VFS was mounted on must be a directory
    {
        struct vfs_vnode *root = fat32_get_root_node();
        TEST_ASSERT("root node exists", root != NULL);
        TEST_ASSERT_EQ("root node is a directory", root->type, VFS_VNODE_TYPE_DIR);
        TEST_ASSERT("root node has ops", root->ops != NULL);
    }
    TEST_PASS("root node");

    /*
     * A file spanning many clusters exercises chain allocation on write and
     * chain traversal on read — the part a single-cluster test never touches.
     */
    {
        for (int i = 0; i < BIG_SIZE; i++) {
            big_pattern[i] = (uint8_t)((i * 31) & 0xFF);
        }

        int fd = vfs_open(BIG_FILE, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC);
        TEST_ASSERT("create multi-cluster file", fd >= 0);

        int written = vfs_write(fd, big_pattern, BIG_SIZE);
        TEST_ASSERT_EQ("write spans clusters", written, BIG_SIZE);
        TEST_ASSERT_EQ("close after write", vfs_close(fd), 0);

        struct stat st;
        TEST_ASSERT_EQ("stat multi-cluster file", vfs_stat(BIG_FILE, &st), 0);
        TEST_ASSERT_EQ("size matches bytes written", (int)st.st_size, BIG_SIZE);
    }
    TEST_PASS("multi-cluster write");

    // reading it back must walk the chain and return every byte in order
    {
        int fd = vfs_open(BIG_FILE, VFS_O_RDONLY);
        TEST_ASSERT("reopen multi-cluster file", fd >= 0);

        memset(big_readback, 0, BIG_SIZE);
        int got = vfs_read(fd, big_readback, BIG_SIZE);
        TEST_ASSERT_EQ("read spans clusters", got, BIG_SIZE);
        TEST_ASSERT("chain data intact", memcmp(big_readback, big_pattern, BIG_SIZE) == 0);

        vfs_close(fd);
    }
    TEST_PASS("multi-cluster read");

    // seeking into a later cluster must land on the right bytes
    {
        int fd = vfs_open(BIG_FILE, VFS_O_RDONLY);
        TEST_ASSERT("open for seek", fd >= 0);

        const int offset = 8192;
        vfs_off_t pos = vfs_lseek(fd, offset, VFS_SEEK_SET);
        TEST_ASSERT_EQ("seek into later cluster", (int)pos, offset);

        static uint8_t chunk[64];
        memset(chunk, 0, sizeof(chunk));
        TEST_ASSERT_EQ("read after seek", vfs_read(fd, chunk, sizeof(chunk)), (int)sizeof(chunk));
        TEST_ASSERT("seeked data correct", memcmp(chunk, big_pattern + offset, sizeof(chunk)) == 0);

        vfs_close(fd);
    }
    TEST_PASS("seek across clusters");

    /*
     * NOT COVERED: reopening with VFS_O_TRUNC should release the chain and
     * report size 0, but O_TRUNC is defined in vfs.h and never acted on
     * anywhere in the kernel, so the file keeps its old contents. Add the
     * assertion here once truncation lands (Phase 1 item 2 in docs/order.txt).
     */
    {
        TEST_ASSERT_EQ("unlink multi-cluster file", vfs_unlink(BIG_FILE), 0);

        struct stat st;
        TEST_ASSERT("unlinked file is gone", vfs_stat(BIG_FILE, &st) != 0);
    }
    TEST_PASS("unlink releases file");

    // nested directories must resolve through multiple levels
    {
        TEST_ASSERT_EQ("mkdir level 1", vfs_mkdir(NEST_DIR), 0);
        TEST_ASSERT_EQ("mkdir level 2", vfs_mkdir(NEST_SUB), 0);

        int fd = vfs_open(NEST_FILE, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC);
        TEST_ASSERT("create file two levels deep", fd >= 0);
        TEST_ASSERT_EQ("write nested file", vfs_write(fd, "nested", 6), 6);
        vfs_close(fd);

        static char buf[16];
        memset(buf, 0, sizeof(buf));
        fd = vfs_open(NEST_FILE, VFS_O_RDONLY);
        TEST_ASSERT("reopen nested file", fd >= 0);
        TEST_ASSERT_EQ("read nested file", vfs_read(fd, buf, sizeof(buf) - 1), 6);
        TEST_ASSERT("nested contents correct", strcmp(buf, "nested") == 0);
        vfs_close(fd);
    }
    TEST_PASS("nested directories");

    // a non-empty directory must not be removable
    {
        TEST_ASSERT("rmdir on non-empty dir fails", vfs_rmdir(NEST_SUB) != 0);
    }
    TEST_PASS("rmdir refuses non-empty");

    // teardown, innermost first
    {
        TEST_ASSERT_EQ("unlink nested file", vfs_unlink(NEST_FILE), 0);
        TEST_ASSERT_EQ("rmdir level 2", vfs_rmdir(NEST_SUB), 0);
        TEST_ASSERT_EQ("rmdir level 1", vfs_rmdir(NEST_DIR), 0);

        struct stat st;
        TEST_ASSERT("nested tree removed", vfs_stat(NEST_DIR, &st) != 0);
    }
    TEST_PASS("teardown");

    TEST_SUITE_END("FAT32");
}
