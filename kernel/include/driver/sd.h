/*
 * sd.h - Public API for the SD card driver.
 */

#ifndef PERSPICUA_DRIVER_SD_H
#define PERSPICUA_DRIVER_SD_H

#include "types.h"

#include "driver/block.h"

int sd_read_blocks(struct block_device *dev, void *buffer, size_t start_block, size_t num_blocks);

int sd_write_blocks(struct block_device *dev, const void *buffer, size_t start_block,
                    size_t num_blocks);

unsigned int sd_get_irq(void);

/*
 * sd_handle_irq - Services an SDHCI interrupt.
 *
 * Reads and clears the hardware interrupt register, updates the pending-bits
 * word, and unblocks any task waiting for those bits.
 * Returns 1 if a blocked task was woken (caller should call schedule()), 0 otherwise.
 */
int sd_handle_irq(void);

#endif // PERSPICUA_DRIVER_SD_H
