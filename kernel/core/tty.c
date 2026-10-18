/*
 * tty.c - Implementation of the Teletype (TTY) subsystem.
 */

#include "core/tty.h"

#include <stddef.h>
#include <stdint.h>

#include "stdio.h"
#include "string.h"

#include "uapi/errno.h"

#include "core/signals.h"
#include "driver/fb_console.h"
#include "driver/uart.h"
#include "io.h"
#include "sched/process.h"

struct tty console_tty;

static void console_rx_adapter(char c)
{
    tty_handle_rx(&console_tty, c);
}

static void tty_handle_tx(struct tty *tty);

static void console_tx_adapter(void)
{
    tty_handle_tx(&console_tty);
}

// Sends until the FIFO fills. Producers hold tty->lock and consumers uart_tx_lock.
static size_t tty_tx_send_locked(struct tty *tty)
{
    size_t head = __atomic_load_n(&tty->tx_head, __ATOMIC_ACQUIRE);
    size_t tail = tty->tx_tail;
    size_t sent = 0;

    while (tail != head) {
        if (mmio_read(uart_fr) & UART_FR_TXFF) {
            break;
        }
        uart_send_raw((unsigned char)tty->tx_buffer[tail]);
        tail = (tail + 1) % TTY_BUFFER_SIZE;
        sent++;
    }

    __atomic_store_n(&tty->tx_tail, tail, __ATOMIC_RELEASE);
    return sent;
}

static int tty_tx_empty(const struct tty *tty)
{
    return __atomic_load_n(&tty->tx_tail, __ATOMIC_ACQUIRE) == tty->tx_head;
}

#ifdef CONFIG_TESTS
static int tty_test_tx_stalled;

void tty_test_stall_tx(int on)
{
    __atomic_store_n(&tty_test_tx_stalled, on, __ATOMIC_RELEASE);
}
#endif

static int tty_tx_held(void)
{
#ifdef CONFIG_TESTS
    return __atomic_load_n(&tty_test_tx_stalled, __ATOMIC_ACQUIRE);
#else
    return 0;
#endif
}

// Sends what the ring holds until the FIFO fills; nonzero while bytes are left.
static int console_flush_adapter(void)
{
    if (tty_tx_held()) {
        return 0;
    }
    tty_tx_send_locked(&console_tty);
    return !tty_tx_empty(&console_tty);
}

static void tty_pump_tx(struct tty *tty)
{
    unsigned long flags = spin_lock_irqsave(&uart_tx_lock);
    // A kernel message being written owns the UART, and re-arms the TX interrupt when done.
    int halted = uart_tx_paused_locked() || tty_tx_held();
    size_t sent = halted ? 0 : tty_tx_send_locked(tty);
    uart_tx_irq_locked(!halted && !tty_tx_empty(tty));
    spin_unlock_irqrestore(&uart_tx_lock, flags);

    // A direct kernel write may have emptied the ring without waking anyone.
    if (sent || tty_tx_empty(tty)) {
        wq_wake_all(&tty->tx_wq);
    }
}

// One slot is always left empty so a full ring is distinguishable from an empty one.
static size_t tty_tx_space(const struct tty *tty)
{
    size_t tail = __atomic_load_n(&tty->tx_tail, __ATOMIC_ACQUIRE);
    return (tail + TTY_BUFFER_SIZE - tty->tx_head - 1) % TTY_BUFFER_SIZE;
}

static void tty_put_tx_char(struct tty *tty, char c)
{
    size_t next_tx_head = (tty->tx_head + 1) % TTY_BUFFER_SIZE;
    if (next_tx_head != __atomic_load_n(&tty->tx_tail, __ATOMIC_ACQUIRE)) {
        tty->tx_buffer[tty->tx_head] = c;
        __atomic_store_n(&tty->tx_head, next_tx_head, __ATOMIC_RELEASE);
    }
}

// Input is raw: a reader takes bytes as they arrive, and echo is the reader's job.
static int tty_rx_ready(struct tty *tty)
{
    return tty->rx_head != tty->rx_tail;
}

void tty_init(struct tty *tty)
{
    memset(tty->rx_buffer, 0, TTY_BUFFER_SIZE);
    tty->rx_head = 0;
    tty->rx_tail = 0;

    memset(tty->tx_buffer, 0, TTY_BUFFER_SIZE);
    tty->tx_head = 0;
    tty->tx_tail = 0;

    wq_init(&tty->rx_wq);
    wq_init(&tty->tx_wq);
    tty->lock = (spinlock_t)SPINLOCK_INIT;
    tty->foreground_pgid = 0;
    tty->session_id = 0;
    tty->rx_dropped = 0;

    uart_reg_rx_callback(console_rx_adapter);
    uart_reg_tx_callback(console_tx_adapter);
    uart_reg_flush_callback(console_flush_adapter);

    pr_info("tty: console tty initialized\n");
}

// On the leader's exit; stopped groups it orphans get SIGHUP and SIGCONT from process_exit.
void tty_session_exit(uint32_t sid)
{
    unsigned long flags = spin_lock_irqsave(&console_tty.lock);
    if (console_tty.session_id == sid && sid > 0) {
        uint32_t fg_pgid = console_tty.foreground_pgid;
        console_tty.session_id = 0;
        console_tty.foreground_pgid = 0;
        spin_unlock_irqrestore(&console_tty.lock, flags);

        if (fg_pgid > 0) {
            signal_send_group(fg_pgid, SIGHUP);
        }
    } else {
        spin_unlock_irqrestore(&console_tty.lock, flags);
    }
}

int tty_access_check(struct tty *tty, int sig)
{
    struct process *p = process_current();
    if (!p) {
        return TTY_ACCESS_OK;
    }

    unsigned long flags = spin_lock_irqsave(&tty->lock);
    uint32_t sid = tty->session_id;
    uint32_t fg_pgid = tty->foreground_pgid;
    spin_unlock_irqrestore(&tty->lock, flags);

    if (sid == 0 || p->sid != sid || p->pgid == fg_pgid) {
        return TTY_ACCESS_OK;
    }

    if (p->signal_handlers[sig - 1].sa_handler == SIG_IGN
        || (p->blocked_signals & (1u << (sig - 1)))) {
        return TTY_ACCESS_BLOCKED;
    }

    signal_send_group(p->pgid, sig);
    return TTY_ACCESS_STOPPED;
}

// From the TX interrupt: fills the FIFO, wakes writers, keeps the interrupt on while data waits.
static void tty_handle_tx(struct tty *tty)
{
    unsigned long flags = spin_lock_irqsave(&tty->lock);
    tty_pump_tx(tty);
    spin_unlock_irqrestore(&tty->lock, flags);
}

void tty_handle_rx(struct tty *tty, char c)
{
    unsigned long flags = spin_lock_irqsave(&tty->lock);

    if (c == '\r') {
        c = '\n';
    }

    // ^C, ^\ and ^Z signal the foreground group as a whole and discard what was typed before them.
    int sig = c == 3 ? SIGINT : c == 28 ? SIGQUIT : c == 26 ? SIGTSTP : 0;
    if (sig) {
        tty->rx_tail = tty->rx_head;
        uint32_t fg_pgid = tty->foreground_pgid;
        spin_unlock_irqrestore(&tty->lock, flags);
        if (fg_pgid > 0) {
            signal_send_group(fg_pgid, sig);
        }
        return;
    }

    size_t next_rx_head = (tty->rx_head + 1) % TTY_BUFFER_SIZE;
    if (next_rx_head != tty->rx_tail) {
        tty->rx_buffer[tty->rx_head] = c;
        tty->rx_head = next_rx_head;
    } else {
        __atomic_fetch_add(&tty->rx_dropped, 1, __ATOMIC_RELAXED);
    }
    wq_wake_all(&tty->rx_wq);

    spin_unlock_irqrestore(&tty->lock, flags);
}

int tty_read(struct tty *tty, struct vfs_file *file, char *buf, size_t count)
{
    switch (tty_access_check(tty, SIGTTIN)) {
        case TTY_ACCESS_BLOCKED:
            return -EIO;
        case TTY_ACCESS_STOPPED:
            return -ERESTARTSYS;
        default:
            break;
    }

    if (count == 0) {
        return 0;
    }

    unsigned long flags = spin_lock_irqsave(&tty->lock);

    if (!tty_rx_ready(tty)) {
        if (file && (file->flags & O_NONBLOCK)) {
            spin_unlock_irqrestore(&tty->lock, flags);
            return -EAGAIN;
        }

        int ret = wq_wait_event_interruptible_locked(&tty->rx_wq, tty_rx_ready(tty), &tty->lock);
        if (ret == -ERESTARTSYS) {
            spin_unlock_irqrestore(&tty->lock, flags);
            return -ERESTARTSYS;
        }
    }

    size_t n = 0;
    while (tty->rx_head != tty->rx_tail && n < count) {
        buf[n++] = tty->rx_buffer[tty->rx_tail];
        tty->rx_tail = (tty->rx_tail + 1) % TTY_BUFFER_SIZE;
    }

    spin_unlock_irqrestore(&tty->lock, flags);
    return (int)n;
}

int tty_write(struct tty *tty, const char *buf, size_t count)
{
    unsigned long flags = spin_lock_irqsave(&tty->lock);

    size_t queued = 0;
    int err = 0;
    while (queued < count && !err) {
        char c = buf[queued];

        // A newline needs both slots at once, or the CR goes out without its LF.
        size_t need = (c == '\n') ? 2 : 1;

        while (tty_tx_space(tty) < need && !err) {
            // Arms the TX interrupt, which is what wakes us once space frees up.
            tty_pump_tx(tty);
            if (tty_tx_space(tty) < need) {
                err = wq_wait_event_interruptible_locked(&tty->tx_wq, tty_tx_space(tty) >= need,
                                                         &tty->lock);
            }
        }
        if (err) {
            break;
        }

        if (c == '\n') {
            tty_put_tx_char(tty, '\r');
        }
        tty_put_tx_char(tty, c);
        queued++;
    }

    tty_pump_tx(tty);
    spin_unlock_irqrestore(&tty->lock, flags);

    // A glyph at a time, after tty->lock is dropped, so no lock is held across the whole write.
    for (size_t i = 0; i < queued; i++) {
        fb_console_putc(buf[i]);
    }

    return queued ? (int)queued : err;
}
