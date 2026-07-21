/*
 * test_vfs.c - Boot-phase tests for the virtual filesystem layer.
 *
 * Runs against the mounted FAT32 root, so it doubles as coverage of the
 * FAT32 vnode ops reached through the VFS. Every file this suite creates is
 * removed again before it returns.
 */

#include "test.h"

#include "string.h"

#include "fs/vfs.h"

/* Scratch paths live at the root of the mounted image. */
#define SCRATCH_FILE "/tvfs.tmp"
#define SCRATCH_DIR  "/tvfsdir"
#define RENAMED_FILE "/tvfs2.tmp"

void test_vfs(void)
{
    TEST_SUITE_BEGIN("VFS");

    // path resolution
    {
        int err = 0;
        struct vfs_vnode *root = vfs_resolve_path("/", NULL, &err);
        TEST_ASSERT("resolve root", root != NULL);
        TEST_ASSERT_EQ("root is a directory", root->type, VFS_VNODE_TYPE_DIR);

        err = 0;
        struct vfs_vnode *missing = vfs_resolve_path("/definitely_not_here", NULL, &err);
        TEST_ASSERT("resolve missing returns NULL", missing == NULL);
        TEST_ASSERT("resolve missing sets error", err != 0);
    }
    TEST_PASS("path resolution");

    // opening a file that does not exist must fail rather than create one
    {
        int fd = vfs_open("/definitely_not_here", VFS_O_RDONLY);
        TEST_ASSERT("open missing fails", fd < 0);
    }
    TEST_PASS("open missing");

    /*
     * Create, write, read back, and remove a scratch file. This exercises the
     * whole FAT32 write path (directory entry creation, cluster allocation,
     * write-through) through the VFS interface.
     */
    {
        static const char payload[] = "perspicua vfs scratch payload";
        const size_t len = sizeof(payload) - 1;

        int fd = vfs_open(SCRATCH_FILE, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC);
        TEST_ASSERT("create scratch file", fd >= 0);

        int written = vfs_write(fd, payload, len);
        TEST_ASSERT_EQ("write returns full length", written, (int)len);

        vfs_off_t pos = vfs_lseek(fd, 0, VFS_SEEK_SET);
        TEST_ASSERT_EQ("lseek to start", (int)pos, 0);

        static char readbuf[64];
        memset(readbuf, 0, sizeof(readbuf));
        int got = vfs_read(fd, readbuf, len);
        TEST_ASSERT_EQ("read returns full length", got, (int)len);
        TEST_ASSERT("read matches written", memcmp(readbuf, payload, len) == 0);

        vfs_off_t end = vfs_lseek(fd, 0, VFS_SEEK_END);
        TEST_ASSERT_EQ("lseek to end reports size", (int)end, (int)len);

        TEST_ASSERT_EQ("close scratch file", vfs_close(fd), 0);
    }
    TEST_PASS("create/write/read");

    // the file must be visible to stat with the size just written
    {
        struct stat st;
        memset(&st, 0, sizeof(st));
        TEST_ASSERT_EQ("stat scratch file", vfs_stat(SCRATCH_FILE, &st), 0);
        TEST_ASSERT_EQ("stat reports size", (int)st.st_size, 29);
    }
    TEST_PASS("stat");

    // reopening must see the persisted contents, not a fresh file
    {
        int fd = vfs_open(SCRATCH_FILE, VFS_O_RDONLY);
        TEST_ASSERT("reopen scratch file", fd >= 0);

        static char buf[64];
        memset(buf, 0, sizeof(buf));
        int got = vfs_read(fd, buf, sizeof(buf) - 1);
        TEST_ASSERT_EQ("reopen read length", got, 29);
        TEST_ASSERT("reopen contents persisted", strncmp(buf, "perspicua vfs", 13) == 0);
        vfs_close(fd);
    }
    TEST_PASS("persistence across reopen");

    // rename then unlink
    {
        TEST_ASSERT_EQ("rename scratch file", vfs_rename(SCRATCH_FILE, RENAMED_FILE), 0);

        struct stat st;
        TEST_ASSERT("old name is gone", vfs_stat(SCRATCH_FILE, &st) != 0);
        TEST_ASSERT_EQ("new name exists", vfs_stat(RENAMED_FILE, &st), 0);

        TEST_ASSERT_EQ("unlink renamed file", vfs_unlink(RENAMED_FILE), 0);
        TEST_ASSERT("unlinked file is gone", vfs_stat(RENAMED_FILE, &st) != 0);
    }
    TEST_PASS("rename/unlink");

    // directory create and remove
    {
        TEST_ASSERT_EQ("mkdir", vfs_mkdir(SCRATCH_DIR), 0);

        struct stat st;
        TEST_ASSERT_EQ("stat new directory", vfs_stat(SCRATCH_DIR, &st), 0);

        int err = 0;
        struct vfs_vnode *dir = vfs_resolve_path(SCRATCH_DIR, NULL, &err);
        TEST_ASSERT("resolve new directory", dir != NULL);
        TEST_ASSERT_EQ("new directory is a dir", dir->type, VFS_VNODE_TYPE_DIR);

        TEST_ASSERT_EQ("rmdir", vfs_rmdir(SCRATCH_DIR), 0);
        TEST_ASSERT("removed directory is gone", vfs_stat(SCRATCH_DIR, &st) != 0);
    }
    TEST_PASS("mkdir/rmdir");

    // bad descriptors must be rejected, not indexed blindly
    {
        static char buf[8];
        TEST_ASSERT("read on bad fd fails", vfs_read(-1, buf, sizeof(buf)) < 0);
        TEST_ASSERT("read on unopened fd fails", vfs_read(VFS_MAX_FDS - 1, buf, sizeof(buf)) < 0);
        TEST_ASSERT("close on bad fd fails", vfs_close(-1) < 0);
        TEST_ASSERT("close on out-of-range fd fails", vfs_close(VFS_MAX_FDS + 100) < 0);
    }
    TEST_PASS("bad descriptors");

    TEST_SUITE_END("VFS");
}
