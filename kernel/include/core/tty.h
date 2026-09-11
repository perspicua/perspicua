/*
 * tty.h - Public API for the Teletype (TTY) subsystem.
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

// The system console, initialised at boot and never freed.
extern struct tty console_tty;

void tty_init(struct tty *tty);

void tty_session_exit(uint32_t sid);

// Results of tty_access_check.
#define TTY_ACCESS_OK      0 // foreground, or not the caller's controlling terminal
#define TTY_ACCESS_BLOCKED 1 // background, but the signal cannot stop the caller
#define TTY_ACCESS_STOPPED 2 // background: signal sent, abandon the operation

/*
 * tty_access_check - Whether a background caller may touch its controlling
 * terminal, signalling its group with sig if not. A caller that blocks or
 * ignores sig cannot be stopped by it, so that case is reported separately
 * rather than looping forever.
 */
int tty_access_check(struct tty *tty, int sig);

void tty_handle_rx(struct tty *tty, char c);

void tty_handle_tx(struct tty *tty);

int tty_read(struct tty *tty, struct vfs_file *file, char *buf, size_t count);

int tty_write(struct tty *tty, const char *buf, size_t count);

#endif // PERSPICUA_CORE_TTY_H
