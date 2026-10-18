/*
 * tty.h - Public API for the Teletype (TTY) subsystem.
 */

#ifndef PERSPICUA_CORE_TTY_H
#define PERSPICUA_CORE_TTY_H

#include <stddef.h>
#include <stdint.h>

#include "core/lock.h"
#include "sched/sched.h"
#include "sched/wait.h"

#define TTY_BUFFER_SIZE 256

struct vfs_file;

struct tty {
    char rx_buffer[TTY_BUFFER_SIZE];
    size_t rx_head;
    size_t rx_tail;

    char tx_buffer[TTY_BUFFER_SIZE];
    size_t tx_head;
    size_t tx_tail;

    struct wait_queue rx_wq;
    struct wait_queue tx_wq;

    spinlock_t lock;
    uint32_t foreground_pgid;
    uint32_t session_id;

    // Bytes that arrived with the RX ring full and were lost.
    unsigned long rx_dropped;
};

// The system console, initialised at boot and never freed.
extern struct tty console_tty;

void tty_init(struct tty *tty);

void tty_session_exit(uint32_t sid);

// Results of tty_access_check.
#define TTY_ACCESS_OK      0 // foreground, or not the caller's controlling terminal
#define TTY_ACCESS_BLOCKED 1 // background, but the signal cannot stop the caller
#define TTY_ACCESS_STOPPED 2 // background: signal sent, abandon the operation

// OK, or signals the caller's group with sig: STOPPED if that stops it, BLOCKED if it cannot.
int tty_access_check(struct tty *tty, int sig);

void tty_handle_rx(struct tty *tty, char c);

int tty_read(struct tty *tty, struct vfs_file *file, char *buf, size_t count);

// A signal ends the wait for ring space: what was queued is written; none queued is -ERESTARTSYS.
int tty_write(struct tty *tty, const char *buf, size_t count);

#ifdef CONFIG_TESTS
// While on, nothing leaves the TX ring, so a writer can be made to wait for space.
void tty_test_stall_tx(int on);
#endif

#endif // PERSPICUA_CORE_TTY_H
