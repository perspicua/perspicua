/*
 * tty.h - Public API for the Teletype (TTY) subsystem.
 *
 * This file defines the TTY structure and functions for managing
 * character-based input and output, including line editing and buffering.
 */

#ifndef PERSPICUA_KERNEL_TTY_H
#define PERSPICUA_KERNEL_TTY_H

#include "types.h"
#include "lock.h"
#include "sched.h"

/* Size of the internal circular buffers for receive and transmit */
#define TTY_BUFFER_SIZE 256

/*
 * tty - Represents a terminal device with its associated buffers and state.
 */
struct tty
{
    char rx_buffer[TTY_BUFFER_SIZE];
    size_t rx_head;
    size_t rx_tail;

    char tx_buffer[TTY_BUFFER_SIZE];
    size_t tx_head;
    size_t tx_tail;

    struct task* wait_queue_head;
    struct task* wait_queue_tail;

    struct task* tx_wait_queue_head;
    struct task* tx_wait_queue_tail;

    spinlock_t lock;
    int echo_enabled;
    int canon_enabled; /* Canonical mode: waits for newline before returning data */
};

/*
 * tty_init - Initializes a TTY structure with empty buffers and default settings.
 */
void tty_init(struct tty* tty);

/*
 * tty_handle_rx - Processes a single character received from the hardware.
 * Handles line editing in canonical mode and wakes up waiting readers.
 */
void tty_handle_rx(struct tty* tty, char c);

/*
 * tty_read - Reads up to 'count' characters from the TTY receive buffer.
 * This function may block if the buffer is empty or a full line is required.
 */
int tty_read(struct tty* tty, char* buf, size_t count);

/*
 * tty_write - Writes 'count' characters to the TTY transmit buffer and
 * triggers hardware transmission.
 */
int tty_write(struct tty* tty, const char* buf, size_t count);

#endif /* PERSPICUA_KERNEL_TTY_H */
