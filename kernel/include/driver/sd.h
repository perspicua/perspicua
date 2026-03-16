/*
 * sd.h - Public API for the SD card driver.
 *
 * This file defines the initialization and block transfer functions
 * for the SDHCI-compliant EMMC2 controller.
 */

#ifndef PERSPICUA_DRIVER_SD_H
#define PERSPICUA_DRIVER_SD_H

#include "types.h"

#include "driver/block.h"

/*
 * sd_init - Discovers and initializes the EMMC2 controller.
 */
void sd_init(void);

/*
 * sd_read_blocks - Implementation of the block_device read operation.
 */
int sd_read_blocks(struct block_device* dev, void* buffer, size_t start_block, size_t num_blocks);

/*
 * sd_write_blocks - Implementation of the block_device write operation.
 */
int sd_write_blocks(struct block_device* dev, const void* buffer, size_t start_block, size_t num_blocks);

#endif /* PERSPICUA_DRIVER_SD_H */
