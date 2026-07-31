/*
 * tty.h - Public API for the Teletype (TTY) subsystem.
 *
 * This header defines the TTY structure and functions for character-based
 * input/output, including line editing and buffering.
 */

#ifndef PERSPICUA_CORE_TTY_H
#define PERSPICUA_CORE_TTY_H

#include "types.h"

#include "core/lock.h"
#include "sched/sched.h"

#define TTY_BUFFER_SIZE 256

struct vfs_file;

/*
 * struct tty - Represents a terminal device with its buffers and state.
 *
 * Manages separate RX/TX circular buffers and wait queues for processes
 * performing blocking I/O.
 */
struct tty {
    char rx_buffer[TTY_BUFFER_SIZE];
    size_t rx_head;
    size_t rx_tail;

    char tx_buffer[TTY_BUFFER_SIZE];
    size_t tx_head;
    size_t tx_tail;

    struct task *wait_queue_head;
    struct task *wait_queue_tail;

    struct task *tx_wait_queue_head;
    struct task *tx_wait_queue_tail;

    spinlock_t lock;
    int echo_enabled;
    int canon_enabled;
    uint32_t foreground_pgid;
    uint32_t session_id;
};

/*
 * tty_init - Initializes a TTY structure with default settings.
 */
void tty_init(struct tty *tty);

/*
 * tty_session_exit - Clears terminal session association and sends SIGHUP when session leader exits.
 */
void tty_session_exit(uint32_t sid);

/*
 * tty_handle_rx - Processes a character received from the hardware.
 */
void tty_handle_rx(struct tty *tty, char c);

/*
 * tty_handle_tx - Called from the UART TX interrupt to drain the TX ring buffer.
 */
void tty_handle_tx(struct tty *tty);

/*
 * tty_read - Reads characters from the TTY receive buffer.
 */
int tty_read(struct tty *tty, struct vfs_file *file, char *buf, size_t count);

/*
 * tty_write - Writes characters to the TTY transmit buffer.
 */
int tty_write(struct tty *tty, const char *buf, size_t count);

#endif /* PERSPICUA_CORE_TTY_H */
