/*
 * mailbox.h - Public API for the VideoCore Mailbox driver.
 */

#ifndef PERSPICUA_DRIVER_MAILBOX_H
#define PERSPICUA_DRIVER_MAILBOX_H

#include <stdint.h>

#define MBOX_TAG_CLOCK_RATE     0x00030002
#define MBOX_TAG_MAX_CLOCK_RATE 0x00030004
#define MBOX_TAG_THROTTLED      0x00030046
#define MBOX_TAG_MEASURED_RATE  0x00030047
#define MBOX_CLOCK_ARM          3

void mbox_call(unsigned int *buffer);

// Sends one property tag carrying two words; 0 with the firmware's two words back, -1 if unanswered.
int mbox_query(uint32_t tag, uint32_t value[2]);

#endif // PERSPICUA_DRIVER_MAILBOX_H
