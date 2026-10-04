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
#include "sched/process.h"
#include "driver/uart.h"
#include "driver/fb_console.h"
#include "io.h"

struct tty console_tty;

static void console_rx_adapter(char c)
{
    tty_handle_rx(&console_tty, c);
}

static void console_tx_adapter(void)
{
    tty_handle_tx(&console_tty);
}

// The producer holds tty->lock, consumers uart_tx_lock; the indices cross via acquire/release.
static size_t tty_tx_send_locked(struct tty *tty, int until_fifo_full)
{
    size_t head = __atomic_load_n(&tty->tx_head, __ATOMIC_ACQUIRE);
    size_t tail = tty->tx_tail;
    size_t sent = 0;

    while (tail != head) {
        if (until_fifo_full && (mmio_read(uart_fr) & UART_FR_TXFF)) {
            break;
        }
        uart_send_raw((unsigned char)tty->tx_buffer[tail]);
        tail = (tail + 1) % TTY_BUFFER_SIZE;
        sent++;
    }

    __atomic_store_n(&tty->tx_tail, tail, __ATOMIC_RELEASE);
    return sent;
}

static void console_flush_adapter(void)
{
    tty_tx_send_locked(&console_tty, 0);
}

static int tty_tx_empty(const struct tty *tty)
{
    return __atomic_load_n(&tty->tx_tail, __ATOMIC_ACQUIRE) == tty->tx_head;
}

static void tty_pump_tx(struct tty *tty)
{
    unsigned long flags = spin_lock_irqsave(&uart_tx_lock);
    size_t sent = tty_tx_send_locked(tty, 1);
    spin_unlock_irqrestore(&uart_tx_lock, flags);

    // A direct kernel write may have emptied the ring without waking anyone.
    if (sent || tty_tx_empty(tty)) {
        wq_wake_all(&tty->tx_wq);
    }

    // Enable TX IRQ only if data remains in buffer
    if (!tty_tx_empty(tty)) {
        mmio_write(uart_imsc, mmio_read(uart_imsc) | UART_IMSC_TXIM);
    } else {
        mmio_write(uart_imsc, mmio_read(uart_imsc) & ~UART_IMSC_TXIM);
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

static void tty_echo_char(struct tty *tty, char c)
{
    if (!tty->echo_enabled) {
        return;
    }

    if (c == '\b' || c == 127) {
        tty_put_tx_char(tty, '\b');
        tty_put_tx_char(tty, ' ');
        tty_put_tx_char(tty, '\b');
        fb_console_putc('\b');
    } else {
        if (c == '\n') {
            tty_put_tx_char(tty, '\r');
        }
        tty_put_tx_char(tty, c);
        fb_console_putc(c);
    }
    tty_pump_tx(tty);
}

static int tty_has_line(struct tty *tty)
{
    size_t i = tty->rx_tail;
    while (i != tty->rx_head) {
        if (tty->rx_buffer[i] == '\n') {
            return 1;
        }
        i = (i + 1) % TTY_BUFFER_SIZE;
    }
    return 0;
}

static int tty_rx_ready(struct tty *tty)
{
    if (tty->rx_head == tty->rx_tail) {
        return 0;
    }
    if (tty->canon_enabled) {
        return tty_has_line(tty);
    }
    return 1;
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
    tty->echo_enabled = 0;
    tty->canon_enabled = 0;
    tty->foreground_pgid = 0;
    tty->session_id = 0;

    uart_reg_rx_callback(console_rx_adapter);
    uart_reg_tx_callback(console_tx_adapter);
    uart_reg_flush_callback(console_flush_adapter);

    pr_info("tty: console tty initialized\n");
}

/*
 * tty_session_exit - Dispatches SIGHUP and detaches terminal when session leader exits.
 *
 * Explicitly out of scope: POSIX orphaned-process-group handling
 * (SIGHUP+SIGCONT to newly-orphaned groups with stopped members).
 */
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

/*
 * tty_handle_tx - Called from the UART TX interrupt to drain the TX ring buffer.
 *
 * Pumps as many bytes as the FIFO will accept, then wakes blocked writers and
 * re-arms (or disarms) the TX interrupt based on buffer fullness.
 */
void tty_handle_tx(struct tty *tty)
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

    // Ctrl-C and Ctrl-Z reach the foreground group as a whole.
    if (c == 3 || c == 26) {
        int sig = (c == 3) ? SIGINT : SIGTSTP;
        uint32_t fg_pgid = tty->foreground_pgid;
        spin_unlock_irqrestore(&tty->lock, flags);
        if (fg_pgid > 0) {
            signal_send_group(fg_pgid, sig);
        }
        return;
    }

    // Backspace handling in canonical mode
    if (tty->canon_enabled && (c == '\b' || c == 127)) {
        if (tty->rx_head != tty->rx_tail) {
            tty->rx_head = (tty->rx_head + TTY_BUFFER_SIZE - 1) % TTY_BUFFER_SIZE;
            tty_echo_char(tty, c);
        }
        spin_unlock_irqrestore(&tty->lock, flags);
        return;
    }

    tty_echo_char(tty, c);

    // Push character to RX buffer
    size_t next_rx_head = (tty->rx_head + 1) % TTY_BUFFER_SIZE;
    if (next_rx_head != tty->rx_tail) {
        tty->rx_buffer[tty->rx_head] = c;
        tty->rx_head = next_rx_head;
    }

    // Wake readers for every char (raw) or every newline (canon)
    if (!tty->canon_enabled || c == '\n') {
        wq_wake_all(&tty->rx_wq);
    }

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
        char c = tty->rx_buffer[tty->rx_tail];
        tty->rx_tail = (tty->rx_tail + 1) % TTY_BUFFER_SIZE;
        buf[n++] = c;
        if (tty->canon_enabled && c == '\n') {
            break;
        }
    }

    spin_unlock_irqrestore(&tty->lock, flags);
    return (int)n;
}

int tty_write(struct tty *tty, const char *buf, size_t count)
{
    unsigned long flags = spin_lock_irqsave(&tty->lock);

    for (size_t i = 0; i < count; i++) {
        char c = buf[i];

        // A newline needs both slots at once, or the CR goes out without its LF.
        size_t need = (c == '\n') ? 2 : 1;

        while (tty_tx_space(tty) < need) {
            // Arms the TX interrupt, which is what wakes us once space frees up.
            tty_pump_tx(tty);
            if (tty_tx_space(tty) >= need) {
                break;
            }

            wq_wait_event_locked(&tty->tx_wq, tty_tx_space(tty) >= need, &tty->lock);
        }

        if (c == '\n') {
            tty_put_tx_char(tty, '\r');
        }
        tty_put_tx_char(tty, c);
    }

    tty_pump_tx(tty);
    spin_unlock_irqrestore(&tty->lock, flags);

    /*
     * Drawn outside tty->lock so a long write does not mask IRQs for
     * thousands of glyphs into uncacheable framebuffer memory. The cost: two
     * concurrent writers' bytes can now interleave on screen instead of one
     * message rendering atomically -- fb_console_lock is per-character, not
     * held across the whole loop, because it is also taken from IRQ context
     * (the RX-echo path) and cannot be held that long either. Purely
     * cosmetic (nothing written to the ring buffer or UART is affected).
     */
    for (size_t i = 0; i < count; i++) {
        char c = buf[i];

        fb_console_putc(c);
    }

    return (int)count;
}
