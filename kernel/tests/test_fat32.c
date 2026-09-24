/*
 * test_fat32.c - Tests for FAT32 behaviour beyond the generic VFS paths.
 */

#include <stddef.h>
#include <stdint.h>

#include "test.h"

#include "string.h"

#include "uapi/errno.h"

#include "fs/fat32.h"
#include "fs/vfs.h"
#include "mm/pmm.h"
#include "mm/slab.h"

#define BIG_FILE  "/tfatbig.tmp"
#define NEST_DIR  "/tfatd"
#define NEST_SUB  "/tfatd/sub"
#define NEST_FILE "/tfatd/sub/deep.txt"

// Comfortably larger than any plausible cluster size for a 32 MB volume.
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

static struct dirent listing[8];

static int count_listed(const char *dir, const char *name)
{
    int fd = vfs_open(dir, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    int count = 0;
    int n;
    while ((n = vfs_readdir(fd, listing, sizeof(listing))) > 0) {
        for (int i = 0; i < n; i++) {
            if (strcmp(listing[i].d_name, name) == 0) {
                count++;
            }
        }
    }
    vfs_close(fd);
    return count;
}

// The first byte of path, or -1 if it cannot be read.
static int first_byte(const char *path)
{
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    unsigned char c = 0;
    int n = vfs_read(fd, &c, 1);
    vfs_close(fd);
    return n == 1 ? c : -1;
}

static int write_file(const char *path, const char *text)
{
    int fd = vfs_open(path, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return fd;
    }
    int len = (int)strlen(text);
    int n = vfs_write(fd, text, (size_t)len);
    vfs_close(fd);
    return n == len ? 0 : -1;
}

void test_fat32(void)
{
    TEST_SUITE_BEGIN("FAT32");

    /*
     * The LFN sequence number is an on-disk byte that indexes the write into
     * the name buffer. Values outside 1..20 must be refused outright: sequence
     * 0 lands 13 bytes below the buffer and sequence 63 roughly 550 bytes past
     * the end of a 256-byte stack array, straight through the caller's saved
     * registers.
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

    /*
     * Every BPB field is attacker-controlled on a removable volume, and the
     * driver divides by some of them and turns others into block addresses.
     * A geometry it cannot describe must be refused at mount.
     */
    {
        struct fat32_bpb bpb;
        struct fat32_fs fs;
        const uint64_t blocks = 65536; // a 32 MB volume

        // a plausible baseline that must be accepted
        memset(&bpb, 0, sizeof(bpb));
        bpb.bytes_per_sector = 512;
        bpb.sectors_per_cluster = 8;
        bpb.reserved_sectors = 32;
        bpb.num_fats = 2;
        bpb.sectors_per_fat_32 = 128;
        bpb.root_cluster = 2;

        memset(&fs, 0, sizeof(fs));
        TEST_ASSERT_EQ("valid BPB accepted", fat32_test_geometry_from_bpb(&bpb, 0, blocks, &fs), 0);
        TEST_ASSERT_EQ("data area follows both FATs", (long)fs.data_lba_start, 32 + 2 * 128);
        TEST_ASSERT("max cluster is bounded by the device",
                    fs.max_cluster >= 2 && fs.max_cluster < blocks);

        // sectors_per_cluster is a divisor: zero must never reach it
        struct fat32_bpb bad = bpb;
        bad.sectors_per_cluster = 0;
        TEST_ASSERT("zero sectors_per_cluster refused",
                    fat32_test_geometry_from_bpb(&bad, 0, blocks, &fs) != 0);

        bad = bpb;
        bad.sectors_per_cluster = 3; // not a power of two
        TEST_ASSERT("non-power-of-two cluster refused",
                    fat32_test_geometry_from_bpb(&bad, 0, blocks, &fs) != 0);

        bad = bpb;
        bad.sectors_per_cluster = 255;
        TEST_ASSERT("oversized cluster refused",
                    fat32_test_geometry_from_bpb(&bad, 0, blocks, &fs) != 0);

        // the driver reads 512-byte sectors everywhere
        bad = bpb;
        bad.bytes_per_sector = 4096;
        TEST_ASSERT("non-512 sector size refused",
                    fat32_test_geometry_from_bpb(&bad, 0, blocks, &fs) != 0);

        bad = bpb;
        bad.reserved_sectors = 0;
        TEST_ASSERT("zero reserved sectors refused",
                    fat32_test_geometry_from_bpb(&bad, 0, blocks, &fs) != 0);

        bad = bpb;
        bad.num_fats = 0;
        TEST_ASSERT("zero FATs refused", fat32_test_geometry_from_bpb(&bad, 0, blocks, &fs) != 0);

        bad = bpb;
        bad.sectors_per_fat_32 = 0;
        TEST_ASSERT("zero-length FAT refused",
                    fat32_test_geometry_from_bpb(&bad, 0, blocks, &fs) != 0);

        bad = bpb;
        bad.root_cluster = 0;
        TEST_ASSERT("root cluster below 2 refused",
                    fat32_test_geometry_from_bpb(&bad, 0, blocks, &fs) != 0);

        // num_fats * sectors_per_fat wraps a uint32_t and lands data_lba_start
        // back on top of the FAT
        bad = bpb;
        bad.num_fats = 2;
        bad.sectors_per_fat_32 = 0x80000000U;
        TEST_ASSERT("overflowing FAT size refused",
                    fat32_test_geometry_from_bpb(&bad, 0, blocks, &fs) != 0);

        // a data area that starts past the end of the device describes nothing
        bad = bpb;
        bad.sectors_per_fat_32 = 100000;
        TEST_ASSERT("data area past the device refused",
                    fat32_test_geometry_from_bpb(&bad, 0, blocks, &fs) != 0);
    }

    // the root node the VFS was mounted on must be a directory
    {
        struct vfs_vnode *root = fat32_get_root_node();
        TEST_ASSERT("root node exists", root != NULL);
        TEST_ASSERT_EQ("root node is a directory", root->type, VFS_VNODE_TYPE_DIR);
        TEST_ASSERT("root node has ops", root->ops != NULL);
    }

    /*
     * A file spanning many clusters exercises chain allocation on write and
     * chain traversal on read — the part a single-cluster test never touches.
     */
    {
        for (int i = 0; i < BIG_SIZE; i++) {
            big_pattern[i] = (uint8_t)((i * 31) & 0xFF);
        }

        int fd = vfs_open(BIG_FILE, O_RDWR | O_CREAT | O_TRUNC);
        TEST_ASSERT("create multi-cluster file", fd >= 0);

        int written = vfs_write(fd, big_pattern, BIG_SIZE);
        TEST_ASSERT_EQ("write spans clusters", written, BIG_SIZE);
        TEST_ASSERT_EQ("close after write", vfs_close(fd), 0);

        struct stat st;
        TEST_ASSERT_EQ("stat multi-cluster file", vfs_stat(BIG_FILE, &st), 0);
        TEST_ASSERT_EQ("size matches bytes written", (int)st.st_size, BIG_SIZE);
    }

    // reading it back must walk the chain and return every byte in order
    {
        int fd = vfs_open(BIG_FILE, O_RDONLY);
        TEST_ASSERT("reopen multi-cluster file", fd >= 0);

        memset(big_readback, 0, BIG_SIZE);
        int got = vfs_read(fd, big_readback, BIG_SIZE);
        TEST_ASSERT_EQ("read spans clusters", got, BIG_SIZE);
        TEST_ASSERT("chain data intact", memcmp(big_readback, big_pattern, BIG_SIZE) == 0);

        vfs_close(fd);
    }

    // seeking into a later cluster must land on the right bytes
    {
        int fd = vfs_open(BIG_FILE, O_RDONLY);
        TEST_ASSERT("open for seek", fd >= 0);

        const int offset = 8192;
        vfs_off_t pos = vfs_lseek(fd, offset, SEEK_SET);
        TEST_ASSERT_EQ("seek into later cluster", (int)pos, offset);

        static uint8_t chunk[64];
        memset(chunk, 0, sizeof(chunk));
        TEST_ASSERT_EQ("read after seek", vfs_read(fd, chunk, sizeof(chunk)), (int)sizeof(chunk));
        TEST_ASSERT("seeked data correct", memcmp(chunk, big_pattern + offset, sizeof(chunk)) == 0);

        vfs_close(fd);
    }

    // reopening with O_TRUNC must release the chain and report size 0
    {
        int fd = vfs_open(BIG_FILE, O_RDWR | O_TRUNC);
        TEST_ASSERT("reopen with O_TRUNC", fd >= 0);
        TEST_ASSERT_EQ("close after O_TRUNC", vfs_close(fd), 0);

        struct stat st;
        TEST_ASSERT_EQ("stat truncated file", vfs_stat(BIG_FILE, &st), 0);
        TEST_ASSERT_EQ("O_TRUNC emptied the file", (int)st.st_size, 0);
    }

    {
        TEST_ASSERT_EQ("unlink multi-cluster file", vfs_unlink(BIG_FILE), 0);

        struct stat st;
        TEST_ASSERT("unlinked file is gone", vfs_stat(BIG_FILE, &st) != 0);
    }

    /*
     * Page cache pages arrive without being zeroed, so a hole written past EOF
     * reads back as whatever the page last held unless fat32_read_page clears
     * it. Dirty a batch of pages, release them, then write past EOF and
     * require the gap to be zero.
     */
    {
#define HOLE_PAGES  16
#define HOLE_BYTE   0x5A
#define HOLE_OFFSET 3000
#define HOLE_TAIL   10

        void *dirty[HOLE_PAGES];
        for (int i = 0; i < HOLE_PAGES; i++) {
            dirty[i] = pmm_alloc_pages_nozero(1);
            if (dirty[i]) {
                memset(dirty[i], HOLE_BYTE, PAGE_SIZE);
            }
        }
        for (int i = 0; i < HOLE_PAGES; i++) {
            if (dirty[i]) {
                pmm_free_page(dirty[i]);
            }
        }

        int fd = vfs_open(BIG_FILE, O_RDWR | O_CREAT | O_TRUNC);
        TEST_ASSERT("open file for hole write", fd >= 0);
        TEST_ASSERT_EQ("seek past EOF", (int)vfs_lseek(fd, HOLE_OFFSET, SEEK_SET), HOLE_OFFSET);
        TEST_ASSERT_EQ("write past EOF", vfs_write(fd, "0123456789", HOLE_TAIL), HOLE_TAIL);
        vfs_close(fd);

        memset(big_readback, HOLE_BYTE, HOLE_OFFSET + HOLE_TAIL);
        fd = vfs_open(BIG_FILE, O_RDONLY);
        TEST_ASSERT("reopen file with hole", fd >= 0);
        TEST_ASSERT_EQ("read whole file", vfs_read(fd, big_readback, HOLE_OFFSET + HOLE_TAIL),
                       HOLE_OFFSET + HOLE_TAIL);
        vfs_close(fd);

        int hole_is_zero = 1;
        for (int i = 0; i < HOLE_OFFSET; i++) {
            if (big_readback[i] != 0) {
                hole_is_zero = 0;
                break;
            }
        }
        TEST_ASSERT("hole reads as zero, not as stale page data", hole_is_zero);
        TEST_ASSERT("data after the hole is intact",
                    memcmp(big_readback + HOLE_OFFSET, "0123456789", HOLE_TAIL) == 0);

        TEST_ASSERT_EQ("unlink file with hole", vfs_unlink(BIG_FILE), 0);
    }

    // nested directories must resolve through multiple levels
    {
        TEST_ASSERT_EQ("mkdir level 1", vfs_mkdir(NEST_DIR), 0);
        TEST_ASSERT_EQ("mkdir level 2", vfs_mkdir(NEST_SUB), 0);

        int fd = vfs_open(NEST_FILE, O_RDWR | O_CREAT | O_TRUNC);
        TEST_ASSERT("create file two levels deep", fd >= 0);
        TEST_ASSERT_EQ("write nested file", vfs_write(fd, "nested", 6), 6);
        vfs_close(fd);

        static char buf[16];
        memset(buf, 0, sizeof(buf));
        fd = vfs_open(NEST_FILE, O_RDONLY);
        TEST_ASSERT("reopen nested file", fd >= 0);
        TEST_ASSERT_EQ("read nested file", vfs_read(fd, buf, sizeof(buf) - 1), 6);
        TEST_ASSERT("nested contents correct", strcmp(buf, "nested") == 0);
        vfs_close(fd);
    }

    // a non-empty directory must not be removable
    {
        TEST_ASSERT("rmdir on non-empty dir fails", vfs_rmdir(NEST_SUB) != 0);
    }

    /*
     * unlink and rmdir reject each other's target. Both resolve the name the
     * same way and differ only in which kind they accept, so the two codes pin
     * that the distinction survives.
     */
    {
        TEST_ASSERT_EQ("unlink refuses a directory", vfs_unlink(NEST_SUB), -EISDIR);
        TEST_ASSERT_EQ("rmdir refuses a file", vfs_rmdir(NEST_FILE), -ENOTDIR);
    }

    /*
     * mkdir on a name that already exists must release the vnode its lookup
     * returned. That vnode holds a reference on its parent, so freeing it
     * without going through vfs_vnode_put strands the parent: the directory
     * vnode is never reclaimed, and slab use climbs once per failed call.
     */
    {
        TEST_ASSERT("duplicate mkdir refused", vfs_mkdir(NEST_SUB) != 0);

        unsigned long before = slab_get_used();
        for (int i = 0; i < 16; i++) {
            TEST_ASSERT("duplicate mkdir refused", vfs_mkdir(NEST_SUB) != 0);
        }
        TEST_ASSERT_EQ("failed mkdir reclaims every vnode", slab_get_used(), before);
    }

    /*
     * Truncating to zero releases the start cluster, and the page cache is keyed
     * on it. Rewriting afterwards must serve the new content, not whatever was
     * cached against the old key -- and the allocator hands the same cluster
     * straight back, so a stale page lands on exactly the file that freed it.
     */
    {
        const int len = 2048;
        int ok = 1;

        for (int round = 0; round < 3 && ok; round++) {
            uint8_t mark = (uint8_t)(0x10 + round);

            int fd = vfs_open(BIG_FILE, O_RDWR | O_CREAT | O_TRUNC);
            if (fd < 0) {
                ok = 0;
                break;
            }

            for (int i = 0; i < len; i++) {
                big_pattern[i] = mark;
            }
            if (vfs_write(fd, big_pattern, len) != len) {
                ok = 0;
            }

            // Read back through the same descriptor, before any close.
            memset(big_pattern, 0, len);
            if (vfs_lseek(fd, 0, SEEK_SET) != 0 || vfs_read(fd, big_pattern, len) != len) {
                ok = 0;
            }
            for (int i = 0; i < len && ok; i++) {
                if (big_pattern[i] != mark) {
                    ok = 0;
                }
            }
            vfs_close(fd);

            // And again after reopening, which must find the same bytes.
            fd = vfs_open(BIG_FILE, O_RDONLY);
            if (fd < 0) {
                ok = 0;
                break;
            }
            memset(big_pattern, 0, len);
            if (vfs_read(fd, big_pattern, len) != len) {
                ok = 0;
            }
            for (int i = 0; i < len && ok; i++) {
                if (big_pattern[i] != mark) {
                    ok = 0;
                }
            }
            vfs_close(fd);
        }

        TEST_ASSERT("rewrite after truncate reads back what was written", ok);
    }

    // teardown, innermost first
    {
        TEST_ASSERT_EQ("unlink nested file", vfs_unlink(NEST_FILE), 0);
        TEST_ASSERT_EQ("rmdir level 2", vfs_rmdir(NEST_SUB), 0);
        TEST_ASSERT_EQ("rmdir level 1", vfs_rmdir(NEST_DIR), 0);

        struct stat st;
        TEST_ASSERT("nested tree removed", vfs_stat(NEST_DIR, &st) != 0);
    }

    /*
     * The root vnode is handed out from raw slab memory, so every field it does
     * not set explicitly must still read as zero. Dirty a same-sized slab object
     * first: the allocator reuses it, so an unzeroed field shows up as 0xAB.
     */
    {
        struct vfs_vnode *dirt = slab_alloc(sizeof(struct vfs_vnode));
        TEST_ASSERT("slab object for root-node poison", dirt != NULL);
        if (dirt) {
            memset(dirt, 0xAB, sizeof(*dirt));
            slab_free(dirt);
        }

        struct vfs_vnode *root = fat32_get_root_node();
        TEST_ASSERT("root node allocated", root != NULL);
        if (root) {
            TEST_ASSERT_EQ("root node refcount is one", root->refcount.counter, 1);
            TEST_ASSERT("root node has no parent", root->parent == NULL);
            TEST_ASSERT("root node name is empty", root->name[0] == '\0');
            slab_free(root);
        }
    }

    /*
     * A deleted entry keeps its old name with name[0] overwritten to 0xE5,
     * and name_as_83 only folds 'a'-'z' -- 0xE5 passes through untouched, so
     * a live file can be named to collide with a ghost byte-for-byte. unlink
     * must not treat that collision as a match: doing so frees the ghost's
     * stale start cluster, which may by then belong to a live file.
     */
    {
        const char *live = "/zzzzzzzz.txt";
        const char *ghost = "/\345zzzzzzz.txt"; // \345 == 0xE5

        int fd = vfs_open(live, O_RDWR | O_CREAT | O_TRUNC);
        TEST_ASSERT("create collision-name file", fd >= 0);
        TEST_ASSERT_EQ("write collision-name file", vfs_write(fd, "hello", 5), 5);
        vfs_close(fd);
        TEST_ASSERT_EQ("unlink collision-name file", vfs_unlink(live), 0);

        TEST_ASSERT("deleted entry is not unlinkable", vfs_unlink(ghost) != 0);
    }

    /*
     * A name that does not fit 8.3 is stored as LFN entries plus a generated
     * alias. It has to survive the round trip through the directory, and the
     * LFN entries have to go away with it -- left behind, they still spell the
     * old name and a later scan will assemble it.
     */
    {
        const char *lname = "/a_name_of_exactly_forty_characters_here";
        const char *lname2 = "/renamed_to_a_different_long_name.txt";

        TEST_ASSERT_EQ("long name is 40 chars", (int)strlen(lname) - 1, 39);

        int fd = vfs_open(lname, O_RDWR | O_CREAT | O_TRUNC);
        TEST_ASSERT("create long name", fd >= 0);
        TEST_ASSERT_EQ("write long-name file", vfs_write(fd, "long", 4), 4);
        vfs_close(fd);

        // Reopening by the same long name is what proves the LFN entries
        // round-tripped rather than the name being truncated to 8.3.
        fd = vfs_open(lname, O_RDONLY);
        TEST_ASSERT("reopen by long name", fd >= 0);
        char buf[8] = {0};
        TEST_ASSERT_EQ("read long-name file", vfs_read(fd, buf, sizeof(buf)), 4);
        TEST_ASSERT("long-name contents", strncmp(buf, "long", 4) == 0);
        vfs_close(fd);

        struct stat st;
        TEST_ASSERT_EQ("stat long name", vfs_stat(lname, &st), 0);
        TEST_ASSERT_EQ("long-name size", (int)st.st_size, 4);
        TEST_ASSERT("modified time was stamped", st.st_mtime != 0);

        TEST_ASSERT_EQ("rename long to long", vfs_rename(lname, lname2), 0);
        TEST_ASSERT("old long name is gone", vfs_open(lname, O_RDONLY) < 0);
        fd = vfs_open(lname2, O_RDONLY);
        TEST_ASSERT("new long name resolves", fd >= 0);
        vfs_close(fd);

        TEST_ASSERT_EQ("unlink long name", vfs_unlink(lname2), 0);
        TEST_ASSERT("unlinked long name is gone", vfs_open(lname2, O_RDONLY) < 0);

        // Recreating it must work: if the LFN entries had outlived the unlink,
        // the stale fragments would still name a file that no longer exists.
        fd = vfs_open(lname, O_RDWR | O_CREAT | O_TRUNC);
        TEST_ASSERT("recreate after unlink", fd >= 0);
        vfs_close(fd);
        TEST_ASSERT_EQ("cleanup long name", vfs_unlink(lname), 0);
    }

    // A long name is never cut down to 8.3 to find a match: these are two
    // files, and truncating the long one must leave the short one alone.
    {
        const char *short_name = "/VERYLONG.TXT";
        const char *long_name = "/verylongname.txt";
        struct stat st;

        TEST_ASSERT_EQ("create the 8.3 file", write_file(short_name, "S"), 0);
        TEST_ASSERT_EQ("create a long name sharing its prefix", write_file(long_name, "L"), 0);

        TEST_ASSERT_EQ("the 8.3 file keeps its byte", first_byte(short_name), 'S');
        TEST_ASSERT_EQ("the long file has its own", first_byte(long_name), 'L');

        TEST_ASSERT_EQ("unlink the long file", vfs_unlink(long_name), 0);
        TEST_ASSERT_EQ("the 8.3 file survives it", vfs_stat(short_name, &st), 0);
        TEST_ASSERT_EQ("unlink the 8.3 file", vfs_unlink(short_name), 0);
    }

    // Names compare without case, long ones too, so another spelling opens
    // the same file rather than creating a second one
    {
        const char *name = "/Mixed_Case_Name.txt";
        struct stat st;

        TEST_ASSERT_EQ("create a mixed-case long name", write_file(name, "c"), 0);
        TEST_ASSERT_EQ("found in upper case", vfs_stat("/MIXED_CASE_NAME.TXT", &st), 0);

        int fd = vfs_open("/mixed_case_name.txt", O_RDWR | O_CREAT);
        TEST_ASSERT("opened in lower case", fd >= 0);
        if (fd >= 0) {
            vfs_close(fd);
        }
        TEST_ASSERT_EQ(
            "no second entry was made",
            count_listed("/", "Mixed_Case_Name.txt") + count_listed("/", "mixed_case_name.txt"), 1);

        TEST_ASSERT_EQ("unlink in another case", vfs_unlink("/MIXED_CASE_NAME.TXT"), 0);
        TEST_ASSERT("the file is gone", vfs_stat(name, &st) != 0);
    }

    // rename replaces an existing target of the same kind, and refuses the rest
    {
        const char *src = "/tfrsrc.tmp";
        const char *dst = "/tfrdst.tmp";
        const char *dir = "/tfrdir";
        struct stat st;

        TEST_ASSERT_EQ("create the source", write_file(src, "new"), 0);
        TEST_ASSERT_EQ("create the target", write_file(dst, "old!"), 0);

        TEST_ASSERT_EQ("rename onto an existing file", vfs_rename(src, dst), 0);
        TEST_ASSERT_EQ("the target is listed once", count_listed("/", "tfrdst.tmp"), 1);
        TEST_ASSERT_EQ("the target holds the source", first_byte(dst), 'n');
        TEST_ASSERT("the source name is gone", vfs_stat(src, &st) != 0);

        TEST_ASSERT_EQ("mkdir for the kind checks", vfs_mkdir(dir), 0);
        TEST_ASSERT_EQ("a file does not replace a directory", vfs_rename(dst, dir), -EISDIR);
        TEST_ASSERT_EQ("a directory does not replace a file", vfs_rename(dir, dst), -ENOTDIR);
        TEST_ASSERT_EQ("a directory cannot move beneath itself", vfs_rename(dir, "/tfrdir/in"),
                       -EINVAL);

        TEST_ASSERT_EQ("mkdir a second directory", vfs_mkdir("/tfrdir2"), 0);
        TEST_ASSERT_EQ("fill it", write_file("/tfrdir2/f.tmp", "f"), 0);
        TEST_ASSERT_EQ("a non-empty directory is not replaced", vfs_rename(dir, "/tfrdir2"),
                       -ENOTEMPTY);
        TEST_ASSERT_EQ("empty it", vfs_unlink("/tfrdir2/f.tmp"), 0);
        TEST_ASSERT_EQ("an empty directory is replaced", vfs_rename(dir, "/tfrdir2"), 0);
        TEST_ASSERT_EQ("the replaced name is listed once", count_listed("/", "tfrdir2"), 1);
        TEST_ASSERT("the source directory is gone", vfs_stat(dir, &st) != 0);

        TEST_ASSERT_EQ("cleanup the target", vfs_unlink(dst), 0);
        TEST_ASSERT_EQ("cleanup the directory", vfs_rmdir("/tfrdir2"), 0);
    }

    // A directory moved to another parent takes its ".." with it
    {
        int err = 0;
        uint32_t dotdot = 0;

        TEST_ASSERT_EQ("mkdir first parent", vfs_mkdir("/tfmva"), 0);
        TEST_ASSERT_EQ("mkdir second parent", vfs_mkdir("/tfmvb"), 0);
        TEST_ASSERT_EQ("mkdir the one that moves", vfs_mkdir("/tfmva/d"), 0);
        TEST_ASSERT_EQ("move it across", vfs_rename("/tfmva/d", "/tfmvb/d"), 0);

        struct vfs_vnode *moved = vfs_resolve_path("/tfmvb/d", NULL, &err);
        struct vfs_vnode *parent = vfs_resolve_path("/tfmvb", NULL, &err);
        TEST_ASSERT("moved directory resolves", moved != NULL && parent != NULL);

        if (moved && parent) {
            TEST_ASSERT_EQ("read its ..", fat32_test_dotdot(moved, &dotdot), 0);
            TEST_ASSERT_EQ("its .. names the new parent", dotdot,
                           (uint32_t)(uintptr_t)parent->internal_info);
        }
        if (moved) {
            vfs_vnode_put(moved);
        }
        if (parent) {
            vfs_vnode_put(parent);
        }

        TEST_ASSERT_EQ("cleanup moved dir", vfs_rmdir("/tfmvb/d"), 0);
        TEST_ASSERT_EQ("cleanup second parent", vfs_rmdir("/tfmvb"), 0);
        TEST_ASSERT_EQ("cleanup first parent", vfs_rmdir("/tfmva"), 0);
    }

    /*
     * A write refused partway reports what landed, and the offset and size
     * cover exactly that, so a retry from the offset repeats and loses nothing.
     * Every page allocation the write makes is refused in turn.
     */
    {
        const int len = 3 * 4096;
        int ok = 1;
        int partial = 0;

        for (int i = 0; i < len; i++) {
            big_pattern[i] = (uint8_t)(i * 7);
        }

        for (unsigned long nth = 1; nth <= 24 && ok; nth++) {
            int fd = vfs_open(BIG_FILE, O_RDWR | O_CREAT | O_TRUNC);
            if (fd < 0) {
                ok = 0;
                break;
            }

            pmm_test_fail_nth(nth);
            int r = vfs_write(fd, big_pattern, (size_t)len);
            pmm_test_fail_nth(0);

            long off = vfs_lseek(fd, 0, SEEK_CUR);
            struct stat st;
            int st_ok = vfs_fstat(fd, &st) == 0;

            if (r > 0) {
                ok = off == r && st_ok && (long)st.st_size >= r;
                partial |= r < len;
            } else {
                ok = off == 0;
            }
            vfs_close(fd);
        }

        TEST_ASSERT("a refused write moves the offset by what it reports", ok);
        TEST_ASSERT("some refusal landed partway through", partial);
        vfs_unlink(BIG_FILE);
    }

    TEST_SUITE_END("FAT32");
}
