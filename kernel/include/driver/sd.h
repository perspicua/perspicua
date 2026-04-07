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

/*
 * sd_get_irq - Returns the IRQ line assigned to the SDHCI controller.
 */
unsigned int sd_get_irq(void);

/*
 * sd_handle_irq - Services an SDHCI interrupt.
 *
 * Reads and clears the hardware interrupt register, updates the pending-bits
 * word, and unblocks any task waiting for those bits.
 * Returns 1 if a blocked task was woken (caller should call schedule()), 0 otherwise.
 */
int sd_handle_irq(void);

#endif /* PERSPICUA_DRIVER_SD_H */
