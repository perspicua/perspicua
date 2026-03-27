/*
 * mailbox.h - Public API for the VideoCore Mailbox driver.
 *
 * This header defines the interface for communicating with the Raspberry Pi
 * VideoCore GPU using the mailbox property interface.
 */

#ifndef PERSPICUA_DRIVER_MAILBOX_H
#define PERSPICUA_DRIVER_MAILBOX_H

/* --- Function Prototypes --- */

/*
 * mbox_init - Discovers the mailbox hardware and maps registers.
 */
void mbox_init(void);

/*
 * mbox_call - Performs a synchronous mailbox transaction with cache maintenance.
 */
void mbox_call(unsigned int *buffer);

#endif /* PERSPICUA_DRIVER_MAILBOX_H */
