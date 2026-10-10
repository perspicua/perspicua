#include <stddef.h>
#include <stdint.h>

#include "test.h"
#include "driver/block.h"
#include "string.h"
#include "uapi/errno.h"

// Offset of the 0x55AA boot signature within a 512-byte sector.
#define BOOT_SIG_OFFSET 510

// The multi-block checks use the card's last sectors, saved first and restored after.
#define SCRATCH_BLOCKS 16

void test_sd(void)
{
    TEST_SUITE_BEGIN("SD Driver");

    // device registration
    struct block_device *dev = block_device_lookup("sd0");
    TEST_ASSERT("sd0 registered", dev != NULL);
    TEST_ASSERT("sd0 present", dev->present != 0);
    TEST_ASSERT("sd0 name", strcmp(dev->name, "sd0") == 0);
    TEST_ASSERT_EQ("block size", dev->block_size, 512);
    TEST_ASSERT("block count nonzero", dev->block_count > 0);
    TEST_ASSERT("read_blocks bound", dev->read_blocks != NULL);
    TEST_ASSERT("write_blocks bound", dev->write_blocks != NULL);

    // lookup of an unregistered name must not invent a device
    TEST_ASSERT("lookup unknown returns NULL", block_device_lookup("no_such_dev") == NULL);
    TEST_ASSERT("lookup empty returns NULL", block_device_lookup("") == NULL);

    /*
     * Block 0 of the QEMU-attached image is a FAT32 boot sector, so its
     * content is known: a correct read must land the 0x55AA signature in the
     * last two bytes. That makes this a verification of the data path rather
     * than just a "did not return an error" check.
     */
    static uint8_t buf[512];
    memset(buf, 0xA5, sizeof(buf));

    int res = dev->read_blocks(dev, buf, 0, 1);
    TEST_ASSERT_EQ("read block 0", res, 0);
    TEST_ASSERT_EQ("boot sig low", buf[BOOT_SIG_OFFSET], 0x55);
    TEST_ASSERT_EQ("boot sig high", buf[BOOT_SIG_OFFSET + 1], 0xAA);

    // a second read of the same block must return identical bytes
    static uint8_t buf2[512];
    memset(buf2, 0x5A, sizeof(buf2));
    TEST_ASSERT_EQ("reread block 0", dev->read_blocks(dev, buf2, 0, 1), 0);
    TEST_ASSERT("reads are consistent", memcmp(buf, buf2, 512) == 0);

    // a zero-block read is a no-op that must leave the buffer alone
    static uint8_t untouched[512];
    memset(untouched, 0x3C, sizeof(untouched));
    TEST_ASSERT_EQ("zero-block read", dev->read_blocks(dev, untouched, 0, 0), 0);
    for (int i = 0; i < 512; i++) {
        TEST_ASSERT("zero-block read wrote nothing", untouched[i] == 0x3C);
    }

    // multi-block read must place block 0 at the front, unchanged
    static uint8_t multi[1024];
    memset(multi, 0, sizeof(multi));
    TEST_ASSERT_EQ("two-block read", dev->read_blocks(dev, multi, 0, 2), 0);
    TEST_ASSERT("multi-block block 0 matches", memcmp(multi, buf, 512) == 0);

    /*
     * Write path, exercised on the last sector so no filesystem structure is
     * at risk. The original content is saved and restored before any assert
     * runs, because a failing TEST_ASSERT returns from the suite immediately
     * and would otherwise leave the scratch pattern on the image.
     */
    size_t scratch = dev->block_count - 1;
    static uint8_t orig[512];
    static uint8_t pattern[512];
    static uint8_t readback[512];

    int rd_orig = dev->read_blocks(dev, orig, scratch, 1);
    int wr = -1, rd_back = -1, restore = -1, restore_ok = 0;

    if (rd_orig == 0) {
        for (int i = 0; i < 512; i++) {
            pattern[i] = (uint8_t)(i ^ 0x5A);
        }

        wr = dev->write_blocks(dev, pattern, scratch, 1);

        // The write went through the cache too; only a dropped cache proves the card has it.
        block_cache_invalidate();
        memset(readback, 0, sizeof(readback));
        rd_back = dev->read_blocks(dev, readback, scratch, 1);

        restore = dev->write_blocks(dev, orig, scratch, 1);
        block_cache_invalidate();

        static uint8_t verify[512];
        memset(verify, 0, sizeof(verify));
        if (restore == 0 && dev->read_blocks(dev, verify, scratch, 1) == 0) {
            restore_ok = (memcmp(verify, orig, 512) == 0);
        }
    }

    TEST_ASSERT_EQ("read scratch block", rd_orig, 0);
    TEST_ASSERT_EQ("write scratch block", wr, 0);
    TEST_ASSERT_EQ("read back scratch block", rd_back, 0);
    TEST_ASSERT("written data reads back", memcmp(readback, pattern, 512) == 0);
    TEST_ASSERT_EQ("restore scratch block", restore, 0);
    TEST_ASSERT("scratch block restored", restore_ok != 0);

    /*
     * Multi-block transfers, in shapes of 1, 13 and 2 blocks across the
     * scratch range. Every block of the pattern differs, so a block landing
     * in the wrong place shows. Everything is put back before any assert.
     */
    size_t base = dev->block_count - SCRATCH_BLOCKS;
    static uint8_t saved[SCRATCH_BLOCKS * 512];
    static uint8_t blocks[SCRATCH_BLOCKS * 512];
    static uint8_t got[SCRATCH_BLOCKS * 512];
    int saved_rc = dev->read_blocks(dev, saved, base, SCRATCH_BLOCKS);
    int w1 = -1, w13 = -1, w2 = -1, whole_rc = -1, whole_ok = 0, singles_ok = 0;
    int back_rc = -1, back_ok = 0;

    if (saved_rc == 0) {
        for (size_t i = 0; i < sizeof(blocks); i++) {
            blocks[i] = (uint8_t)((i / 512) * 31 + (i % 512) * 7 + 1);
        }
        w1 = dev->write_blocks(dev, blocks, base, 1);
        w13 = dev->write_blocks(dev, blocks + 512, base + 1, 13);
        w2 = dev->write_blocks(dev, blocks + 14 * 512, base + 14, 2);

        block_cache_invalidate();
        memset(got, 0, sizeof(got));
        whole_rc = dev->read_blocks(dev, got, base, SCRATCH_BLOCKS);
        whole_ok = memcmp(got, blocks, sizeof(blocks)) == 0;

        block_cache_invalidate();
        singles_ok = 1;
        for (size_t b = 0; b < SCRATCH_BLOCKS; b++) {
            memset(got, 0, 512);
            if (dev->read_blocks(dev, got, base + b, 1) != 0
                || memcmp(got, blocks + b * 512, 512) != 0) {
                singles_ok = 0;
            }
        }

        back_rc = dev->write_blocks(dev, saved, base, SCRATCH_BLOCKS);
        block_cache_invalidate();
        if (back_rc == 0 && dev->read_blocks(dev, got, base, SCRATCH_BLOCKS) == 0) {
            back_ok = memcmp(got, saved, sizeof(saved)) == 0;
        }
    }

    TEST_ASSERT_EQ("read the multi-block scratch range", saved_rc, 0);
    TEST_ASSERT_EQ("one-block write", w1, 0);
    TEST_ASSERT_EQ("13-block write", w13, 0);
    TEST_ASSERT_EQ("2-block write", w2, 0);
    TEST_ASSERT_EQ("16-block read", whole_rc, 0);
    TEST_ASSERT("multi-block writes read back whole", whole_ok);
    TEST_ASSERT("multi-block writes read back block by block", singles_ok);
    TEST_ASSERT_EQ("restore the scratch range", back_rc, 0);
    TEST_ASSERT("scratch range restored", back_ok);

    static uint8_t spill[1024];
    TEST_ASSERT_EQ("a read running off the end is refused",
                   dev->read_blocks(dev, spill, dev->block_count - 1, 2), -EINVAL);

    TEST_SUITE_END("SD Driver");
}
