/*
 * mailbox.h - Public API for the VideoCore Mailbox driver.
 *
 * This file defines the interface for communicating with the Raspberry Pi
 * VideoCore GPU using the mailbox property interface.
 */

#ifndef PERSPICUA_DRIVER_MAILBOX_H
#define PERSPICUA_DRIVER_MAILBOX_H

/*
 * mbox_init - Discovers the mailbox hardware base address from the device tree
 * and initializes the register pointers for future communication.
 */
void mbox_init(void);

/*
 * mbox_call - Performs a synchronous mailbox transaction. This function
 * flushes the CPU data cache for the buffer, submits the request to the
 * GPU, waits for the response, and then invalidates the cache so the
 * response data is visible to the CPU.
 */
void mbox_call(unsigned int* buffer);

#endif /* PERSPICUA_DRIVER_MAILBOX_H */
