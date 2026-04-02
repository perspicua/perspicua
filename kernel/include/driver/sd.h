/*
 * sd.h - Public API for the SD card driver.
 *
 * This header defines the initialization and block transfer functions
 * for the SDHCI-compliant EMMC2 controller.
 */

#ifndef PERSPICUA_DRIVER_SD_H
#define PERSPICUA_DRIVER_SD_H

#include "types.h"

#include "driver/block.h"

/*
 * sd_read_blocks - Block-level read implementation for the SD card.
 */
int sd_read_blocks(struct block_device *dev, void *buffer, size_t start_block, size_t num_blocks);

/*
 * sd_write_blocks - Block-level write implementation for the SD card.
 */
int sd_write_blocks(struct block_device *dev, const void *buffer, size_t start_block,
                    size_t num_blocks);

#endif /* PERSPICUA_DRIVER_SD_H */
