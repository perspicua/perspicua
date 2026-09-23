/*
 * test_fat32_corrupt.c - Mounting volumes crafted to be wrong.
 *
 * Where test_fat32.c drives the BPB validator directly, this drives
 * fat32_init: the signature, the partition search, the geometry and the guard
 * against a cluster chain that loops. The device reports 32 MB and stores one
 * sector, synthesising the rest, so a volume of any shape costs 512 bytes.
 */

#include <stddef.h>
#include <stdint.h>

#include "test.h"

#include "string.h"

#include "uapi/errno.h"
#include "uapi/fcntl.h"

#include "driver/block.h"
#include "fs/fat32.h"
#include "fs/vfs.h"

#define RAM_BLOCK_SIZE 512
#define RAM_BLOCKS     65536 // 32 MB, reported but never stored

// A geometry the validator accepts, so only the crafted fault is under test.
#define GOOD_SPC      1
#define GOOD_RESERVED 32
#define GOOD_NUM_FATS 2
#define GOOD_SPF      128

#define FAT_LBA_START GOOD_RESERVED
#define FAT_LBA_END   (GOOD_RESERVED + GOOD_NUM_FATS * GOOD_SPF)

// Where cluster 2 lands, given the geometry above.
#define DATA_LBA_START FAT_LBA_END

// A directory slot whose first byte is 0xE5 is a deleted entry: skipped, but
// it does not end the scan the way a zeroed slot does.
#define DIR_ENTRY_SIZE    32
#define DIR_ENTRY_DELETED 0xE5

// A file the live volume is known to carry, read back after the probes.
#define LIVE_FILE "/README.md"

static uint8_t ram_sector0[RAM_BLOCK_SIZE];
static struct block_device ram_dev;

// While set, every FAT entry reads as cluster 2, so the root chain is a loop.
static int ram_fat_cycles;

static int ram_read_blocks(struct block_device *dev, void *buffer, size_t start_block,
                           size_t num_blocks)
{
    (void)dev;

    uint8_t *out = (uint8_t *)buffer;

    for (size_t i = 0; i < num_blocks; i++) {
        size_t block = start_block + i;
        uint8_t *dst = out + i * RAM_BLOCK_SIZE;

        if (block >= RAM_BLOCKS) {
            return -EIO;
        }

        if (block == 0) {
            memcpy(dst, ram_sector0, RAM_BLOCK_SIZE);
        } else if (ram_fat_cycles && block >= FAT_LBA_START && block < FAT_LBA_END) {
            uint32_t *entries = (uint32_t *)dst;
            for (size_t e = 0; e < RAM_BLOCK_SIZE / sizeof(uint32_t); e++) {
                entries[e] = 2;
            }
        } else if (ram_fat_cycles && block >= DATA_LBA_START) {
            // Every slot taken, so the scan runs off the end of the cluster
            // and follows the chain. A zeroed one would end the directory.
            memset(dst, 0, RAM_BLOCK_SIZE);
            for (size_t e = 0; e < RAM_BLOCK_SIZE; e += DIR_ENTRY_SIZE) {
                dst[e] = DIR_ENTRY_DELETED;
            }
        } else {
            memset(dst, 0, RAM_BLOCK_SIZE);
        }
    }

    return 0;
}

static int ram_write_blocks(struct block_device *dev, const void *buffer, size_t start_block,
                            size_t num_blocks)
{
    (void)dev;
    (void)buffer;
    (void)start_block;
    (void)num_blocks;

    // Nothing here persists, and a caller that believes it did would be wrong.
    return -EIO;
}

// The cache remembers sector 0, so a volume rewritten in place has to be
// dropped from it or the next mount reads the previous one.
static int ram_mount(void)
{
    block_test_invalidate(&ram_dev);
    return fat32_init("ramfat");
}

// Lays a mountable volume into sector 0, which each probe then damages.
static void ram_write_good_bpb(void)
{
    struct fat32_bpb *bpb = (struct fat32_bpb *)ram_sector0;

    memset(ram_sector0, 0, sizeof(ram_sector0));

    bpb->jmp[0] = 0xEB; // read as a boot sector rather than an MBR
    bpb->bytes_per_sector = RAM_BLOCK_SIZE;
    bpb->sectors_per_cluster = GOOD_SPC;
    bpb->reserved_sectors = GOOD_RESERVED;
    bpb->num_fats = GOOD_NUM_FATS;
    bpb->sectors_per_fat_32 = GOOD_SPF;
    bpb->root_cluster = 2;

    ram_sector0[510] = 0x55;
    ram_sector0[511] = 0xAA;
}

void test_fat32_corrupt(void)
{
    TEST_SUITE_BEGIN("FAT32 Corrupt Media");

    struct fat32_fs live;
    fat32_test_save_fs(&live);

    memset(&ram_dev, 0, sizeof(ram_dev));
    strcpy(ram_dev.name, "ramfat");
    ram_dev.block_count = RAM_BLOCKS;
    ram_dev.block_size = RAM_BLOCK_SIZE;
    ram_dev.read_blocks = ram_read_blocks;
    ram_dev.write_blocks = ram_write_blocks;
    ram_dev.present = 1;
    ram_fat_cycles = 0;

    block_device_register(&ram_dev);
    TEST_ASSERT("synthetic device registered", block_device_lookup("ramfat") == &ram_dev);

    // the baseline has to mount, or every rejection below proves nothing
    {
        ram_write_good_bpb();
        TEST_ASSERT_EQ("a well-formed volume mounts", ram_mount(), 0);
    }

    // a sector that is not a boot sector at all
    {
        ram_write_good_bpb();
        ram_sector0[510] = 0x00;
        ram_sector0[511] = 0x00;
        TEST_ASSERT("missing boot signature refused", ram_mount() != 0);
    }

    // without the 0xEB or 0xE9 jump the sector is read as an MBR, and this
    // one's partition table names no FAT32 volume
    {
        ram_write_good_bpb();
        ram_sector0[0] = 0x00;
        TEST_ASSERT("an MBR with no FAT32 partition refused", ram_mount() != 0);
    }

    // sectors_per_cluster is a divisor, and the mount is where zero is caught
    {
        struct fat32_bpb *bpb = (struct fat32_bpb *)ram_sector0;

        ram_write_good_bpb();
        bpb->sectors_per_cluster = 0;
        TEST_ASSERT("zero sectors_per_cluster refused at mount", ram_mount() != 0);

        ram_write_good_bpb();
        bpb->sectors_per_cluster = 3;
        TEST_ASSERT("non-power-of-two cluster refused at mount", ram_mount() != 0);
    }

    // a root cluster the data area cannot address
    {
        struct fat32_bpb *bpb = (struct fat32_bpb *)ram_sector0;

        ram_write_good_bpb();
        bpb->root_cluster = 0;
        TEST_ASSERT("root cluster below 2 refused at mount", ram_mount() != 0);

        ram_write_good_bpb();
        bpb->root_cluster = 0x0FFFFFF0;
        TEST_ASSERT("root cluster past the volume refused at mount", ram_mount() != 0);
    }

    // a FAT that describes more sectors than the device holds
    {
        struct fat32_bpb *bpb = (struct fat32_bpb *)ram_sector0;

        ram_write_good_bpb();
        bpb->sectors_per_fat_32 = 0;
        TEST_ASSERT("zero-length FAT refused at mount", ram_mount() != 0);

        ram_write_good_bpb();
        bpb->sectors_per_fat_32 = 0x80000000U;
        TEST_ASSERT("an overflowing FAT refused at mount", ram_mount() != 0);
    }

    // a device the kernel has never heard of
    TEST_ASSERT("an unknown device refused", fat32_init("no_such_device") != 0);

    // nothing in a cluster cycle is end-of-chain or out of range, so only the
    // guard ends the walk: without it this call never returns
    {
        ram_write_good_bpb();
        ram_fat_cycles = 1;

        TEST_ASSERT_EQ("the looping volume still mounts", ram_mount(), 0);

        struct vfs_vnode *root = fat32_get_root_node();
        TEST_ASSERT("root node built", root != NULL);

        if (root) {
            struct vfs_vnode *found = root->ops->lookup(root, "anything.txt");
            TEST_ASSERT("a cyclic directory chain terminates", found == NULL);
            vfs_vnode_put(root);
        }

        ram_fat_cycles = 0;
    }

    fat32_test_restore_fs(&live);

    // the probes cleared the mounted volume on their way through, and the
    // suites that follow read real files, so prove the restore
    {
        int fd = vfs_open(LIVE_FILE, O_RDONLY);
        TEST_ASSERT("the live volume survived the probes", fd >= 0);
        if (fd >= 0) {
            char buf[16];
            TEST_ASSERT("the live volume still reads", vfs_read(fd, buf, sizeof(buf)) > 0);
            vfs_close(fd);
        }
    }

    TEST_SUITE_END("FAT32 Corrupt Media");
}
