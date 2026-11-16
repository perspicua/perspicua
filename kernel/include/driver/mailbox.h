/*
 * mailbox.h - Public API for the VideoCore Mailbox driver.
 */

#ifndef PERSPICUA_DRIVER_MAILBOX_H
#define PERSPICUA_DRIVER_MAILBOX_H

#include <stddef.h>
#include <stdint.h>

#define MBOX_TAG_BOARD_REVISION 0x00010002
#define MBOX_TAG_CLOCK_RATE     0x00030002
#define MBOX_TAG_TEMPERATURE    0x00030006
#define MBOX_TAG_MAX_CLOCK_RATE 0x00030004
#define MBOX_TAG_SET_CLOCK_RATE 0x00038002
#define MBOX_TAG_THROTTLED      0x00030046
#define MBOX_TAG_MEASURED_RATE  0x00030047
#define MBOX_CLOCK_ARM          3

// How long a call waits for the firmware, with interrupts off, before it fails.
#define MBOX_TIMEOUT_MS 500

// 0, or -ETIMEDOUT when the firmware did not answer in time.
int mbox_call(unsigned int *buffer);

#ifdef CONFIG_TESTS
// Set to make the next call send nothing, as a firmware that never answers would look.
extern volatile int mbox_test_drop_next;
#endif

// One property tag carrying words values in and out; 0 if answered, -1 if not.
int mbox_property(uint32_t tag, uint32_t *value, size_t words);

// One property tag carrying two words; 0 with the firmware's two words back, -1 if unanswered.
int mbox_query(uint32_t tag, uint32_t value[2]);

// Raises the ARM clock to its maximum; MHz set, or -1. The firmware still throttles on heat.
int mbox_arm_clock_to_max(void);

#endif // PERSPICUA_DRIVER_MAILBOX_H
