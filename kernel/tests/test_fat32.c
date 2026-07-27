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

/*
 * A 256-byte name buffer fenced by guard bytes. An out-of-spec LFN sequence
 * number indexes from -13 to +806 relative to the buffer, so the fences are
 * sized to catch a write on either side.
 */
#define LFN_FENCE_LOW  64
#define LFN_NAME_SIZE  256
#define LFN_FENCE_HIGH 640
#define LFN_GUARD_BYTE 0xA5

static uint8_t lfn_guarded[LFN_FENCE_LOW + LFN_NAME_SIZE + LFN_FENCE_HIGH];

void test_fat32(void)
{
    TEST_SUITE_BEGIN("FAT32");

    /*
     * The LFN sequence number is an on-disk byte that indexes the write into
     * the name buffer. Values outside 1..20 must be refused outright: before
     * this was checked, sequence 0 wrote 13 bytes below the buffer and
     * sequence 63 wrote roughly 550 bytes past the end of a 256-byte stack
     * array, straight through the caller's saved registers.
     */
    {
        struct fat32_lfn_entry lfn;
        char *name = (char *)lfn_guarded + LFN_FENCE_LOW;

        memset(lfn_guarded, LFN_GUARD_BYTE, sizeof(lfn_guarded));
        memset(name, 0, LFN_NAME_SIZE);
        memset(&lfn, 0, sizeof(lfn));

        lfn.sequence = 0x00;
        TEST_ASSERT("LFN sequence 0 refused",
                    fat32_test_extract_lfn_part(&lfn, name, LFN_NAME_SIZE) != 0);

        lfn.sequence = 0x3F;
        TEST_ASSERT("LFN sequence 63 refused",
                    fat32_test_extract_lfn_part(&lfn, name, LFN_NAME_SIZE) != 0);

        // the 0x40 last-entry flag must not smuggle a bad sequence past the bound
        lfn.sequence = 0x40 | 0x3F;
        TEST_ASSERT("LFN last-entry flag does not bypass the bound",
                    fat32_test_extract_lfn_part(&lfn, name, LFN_NAME_SIZE) != 0);

        lfn.sequence = 21;
        TEST_ASSERT("LFN sequence past the 20-entry maximum refused",
                    fat32_test_extract_lfn_part(&lfn, name, LFN_NAME_SIZE) != 0);

        for (size_t i = 0; i < LFN_FENCE_LOW; i++) {
            TEST_ASSERT("no write below the name buffer", lfn_guarded[i] == LFN_GUARD_BYTE);
        }
        for (size_t i = LFN_FENCE_LOW + LFN_NAME_SIZE; i < sizeof(lfn_guarded); i++) {
            TEST_ASSERT("no write past the name buffer", lfn_guarded[i] == LFN_GUARD_BYTE);
        }
    }
    TEST_PASS("LFN sequence bounds");

    // a well-formed fragment still lands where the chain expects it
    {
        struct fat32_lfn_entry lfn;
        char *name = (char *)lfn_guarded + LFN_FENCE_LOW;

        memset(lfn_guarded, LFN_GUARD_BYTE, sizeof(lfn_guarded));
        memset(name, 0, LFN_NAME_SIZE);
        memset(&lfn, 0, sizeof(lfn));

        for (int i = 0; i < 5; i++) {
            lfn.name1[i] = (uint16_t)('a' + i);
        }
        for (int i = 0; i < 6; i++) {
            lfn.name2[i] = (uint16_t)('f' + i);
        }
        for (int i = 0; i < 2; i++) {
            lfn.name3[i] = (uint16_t)('l' + i);
        }

        lfn.sequence = 1;
        TEST_ASSERT_EQ("LFN sequence 1 accepted",
                       fat32_test_extract_lfn_part(&lfn, name, LFN_NAME_SIZE), 0);
        TEST_ASSERT("first fragment lands at offset 0", memcmp(name, "abcdefghijklm", 13) == 0);

        // the last legal fragment overruns the usable area and must be clipped
        lfn.sequence = 20;
        TEST_ASSERT_EQ("LFN sequence 20 accepted",
                       fat32_test_extract_lfn_part(&lfn, name, LFN_NAME_SIZE), 0);
        TEST_ASSERT("clipped fragment stops at the terminator", name[LFN_NAME_SIZE - 1] == '\0');

        for (size_t i = LFN_FENCE_LOW + LFN_NAME_SIZE; i < sizeof(lfn_guarded); i++) {
            TEST_ASSERT("clipped fragment stays in the buffer", lfn_guarded[i] == LFN_GUARD_BYTE);
        }
    }
    TEST_PASS("LFN fragment placement");

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
