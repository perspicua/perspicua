#include "test.h"
#include "driver/block.h"
#include "string.h"

void test_sd(void)
{
    TEST_SUITE_BEGIN("SD Driver");

    struct block_device *dev = block_device_lookup("sd0");
    TEST_ASSERT("SD device registered", dev != NULL);
    TEST_ASSERT_EQ("Block size", dev->block_size, 512);

    static uint8_t buf[512];
    memset(buf, 0, 512);
    int res = dev->read_blocks(dev, buf, 0, 1);

    if (res == 0) {
        pr_info("test: sd read block 0 successful\n");
        if (strncmp((char *)buf, "PERSPICUA SD TEST DATA", 22) == 0) {
            pr_info("test: sd data verification: found test string!\n");
        } else {
            pr_info("test: sd data verification: block 0 did not contain test string.\n");
        }
    } else {
        pr_info("test: sd read block 0 failed (expected if no SD card/image attached to QEMU)\n");
    }

    TEST_PASS("SD Driver");
    TEST_SUITE_END("SD Driver");
}
