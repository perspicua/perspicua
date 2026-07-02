/*
 * tty.c - Implementation of the Teletype (TTY) subsystem.
 *
 * Handles buffering, line editing, and process synchronization for
 * character devices.
 */

#include "core/tty.h"

#include "stdio.h"
#include "string.h"

#include "uapi/errors.h"

#include "core/signals.h"
#include "sched/process.h"
#include "driver/uart.h"
#include "driver/fb_console.h"
#include "io.h"

struct tty console_tty;

/*
 * console_rx_adapter - Bridges the UART RX callback to the console TTY.
 */
static void console_rx_adapter(char c)
{
    tty_handle_rx(&console_tty, c);
}

/*
 * console_tx_adapter - Bridges the UART TX callback to the console TTY.
 */
static void console_tx_adapter(void)
{
    tty_handle_tx(&console_tty);
}

/*
 * wait_queue_add - Appends a task to a TTY wait queue.
 */
static void wait_queue_add(struct task **head, struct task **tail, struct task *t)
{
    t->next = NULL;
    if (*tail) {
        (*tail)->next = t;
        *tail = t;
    } else {
        *head = *tail = t;
    }
}

/*
 * wait_queue_remove - Removes and returns the first blocked task in a TTY wait queue.
 */
static struct task *wait_queue_remove(struct task **head, struct task **tail)
{
    while (*head) {
        struct task *t = *head;
        *head = t->next;
        if (*head == NULL) {
            *tail = NULL;
        }
        t->next = NULL;

        /* Skip tasks that were woken up by other signals or timeouts */
        if (__atomic_load_n(&t->state, __ATOMIC_SEQ_CST) == SCHED_TASK_BLOCKED) {
            return t;
        }
    }
    return NULL;
}

/*
 * wait_queue_remove_task - Unlinks a specific task from a TTY wait queue.
 */
static void wait_queue_remove_task(struct task **head, struct task **tail, struct task *target)
{
    struct task *curr = *head;
    struct task *prev = NULL;

    while (curr) {
        if (curr == target) {
            if (prev) {
                prev->next = curr->next;
            } else {
                *head = curr->next;
            }
            if (curr == *tail) {
                *tail = prev;
            }
            curr->next = NULL;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

/*
 * tty_pump_tx - Transfers software TX buffer to hardware UART FIFO.
 */
static void tty_pump_tx(struct tty *tty)
{
    size_t initial_tail = tty->tx_tail;
    unsigned long flags = spin_lock_irqsave(&uart_tx_lock);

    while (tty->tx_head != tty->tx_tail) {
        if (mmio_read(uart_fr) & UART_FR_TXFF) {
            break;
        }

        uart_send_raw((unsigned char)tty->tx_buffer[tty->tx_tail]);
        tty->tx_tail = (tty->tx_tail + 1) % TTY_BUFFER_SIZE;
    }

    spin_unlock_irqrestore(&uart_tx_lock, flags);

    /* Wake writers if space became available */
    if (tty->tx_tail != initial_tail) {
        struct task *t = wait_queue_remove(&tty->tx_wait_queue_head, &tty->tx_wait_queue_tail);
        if (t) {
            sched_unblock(t);
        }
    }

    /* Enable TX IRQ only if data remains in buffer */
    if (tty->tx_head != tty->tx_tail) {
        mmio_write(uart_imsc, mmio_read(uart_imsc) | UART_IMSC_TXIM);
    } else {
        mmio_write(uart_imsc, mmio_read(uart_imsc) & ~UART_IMSC_TXIM);
    }
}

/*
 * tty_put_tx_char - Queues a single char for transmission if space exists.
 */
static void tty_put_tx_char(struct tty *tty, char c)
{
    size_t next_tx_head = (tty->tx_head + 1) % TTY_BUFFER_SIZE;
    if (next_tx_head != tty->tx_tail) {
        tty->tx_buffer[tty->tx_head] = c;
        tty->tx_head = next_tx_head;
    }
}

/*
 * tty_echo_char - Mirrors a char to local console and output stream.
 */
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

/*
 * tty_has_line - Returns non-zero if RX buffer contains a newline.
 */
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

/*
 * tty_init - Wipes buffers and resets TTY state.
 */
void tty_init(struct tty *tty)
{
    memset(tty->rx_buffer, 0, TTY_BUFFER_SIZE);
    tty->rx_head = 0;
    tty->rx_tail = 0;

    memset(tty->tx_buffer, 0, TTY_BUFFER_SIZE);
    tty->tx_head = 0;
    tty->tx_tail = 0;

    tty->wait_queue_head = NULL;
    tty->wait_queue_tail = NULL;
    tty->tx_wait_queue_head = NULL;
    tty->tx_wait_queue_tail = NULL;
    tty->lock = (spinlock_t)SPINLOCK_INIT;
    tty->echo_enabled = 0;
    tty->canon_enabled = 0;
    tty->foreground_pid = 0;

    uart_reg_rx_callback(console_rx_adapter);
    uart_reg_tx_callback(console_tx_adapter);

    pr_info("tty: console tty initialized\n");
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

/*
 * tty_handle_rx - Dispatches received characters to the RX buffer or signals.
 */
void tty_handle_rx(struct tty *tty, char c)
{
    unsigned long flags = spin_lock_irqsave(&tty->lock);

    if (c == '\r') {
        c = '\n';
    }

    /* Ctrl+C handling */
    if (c == 3) {
        if (tty->foreground_pid > 0) {
            signal_send(tty->foreground_pid, SIGNAL_INT);
        }
        spin_unlock_irqrestore(&tty->lock, flags);
        return;
    }

    /* Backspace handling in canonical mode */
    if (tty->canon_enabled && (c == '\b' || c == 127)) {
        if (tty->rx_head != tty->rx_tail) {
            tty->rx_head = (tty->rx_head + TTY_BUFFER_SIZE - 1) % TTY_BUFFER_SIZE;
            tty_echo_char(tty, c);
        }
        spin_unlock_irqrestore(&tty->lock, flags);
        return;
    }

    tty_echo_char(tty, c);

    /* Push character to RX buffer */
    size_t next_rx_head = (tty->rx_head + 1) % TTY_BUFFER_SIZE;
    if (next_rx_head != tty->rx_tail) {
        tty->rx_buffer[tty->rx_head] = c;
        tty->rx_head = next_rx_head;
    }

    /* Wake readers for every char (raw) or every newline (canon) */
    if (!tty->canon_enabled || c == '\n') {
        struct task *t = wait_queue_remove(&tty->wait_queue_head, &tty->wait_queue_tail);
        if (t) {
            sched_unblock(t);
        }
    }

    spin_unlock_irqrestore(&tty->lock, flags);
}

/*
 * tty_read - Reads characters, blocking if no line or data is available.
 */
int tty_read(struct tty *tty, struct vfs_file *file, char *buf, size_t count)
{
    struct task *curr_task = sched_get_current();
    if (curr_task) {
        tty->foreground_pid = curr_task->pid;
    }

    size_t n = 0;
    while (n < count) {
        unsigned long flags = spin_lock_irqsave(&tty->lock);

        int ready = (tty->rx_head != tty->rx_tail);
        if (tty->canon_enabled && !tty_has_line(tty)) {
            ready = 0;
        }

        if (!ready) {
            if (file && (file->flags & VFS_O_NONBLOCK)) {
                spin_unlock_irqrestore(&tty->lock, flags);
                if (n == 0) {
                    return -PERS_ERR_TRY_AGAIN;
                }
                break;
            }
            if (n > 0) {
                spin_unlock_irqrestore(&tty->lock, flags);
                break;
            }

            struct task *curr_task_inner = sched_get_current();
            uint32_t pid = curr_task_inner->pid;

            if (process_table[pid].pending_signals & ~process_table[pid].blocked_signals) {
                spin_unlock_irqrestore(&tty->lock, flags);
                return -PERS_ERR_INTERRUPTED;
            }

            curr_task_inner->state = SCHED_TASK_BLOCKED;
            wait_queue_add(&tty->wait_queue_head, &tty->wait_queue_tail, curr_task_inner);
            spin_unlock_irqrestore(&tty->lock, flags);
            schedule();

            /* Cleanup after wake */
            unsigned long flags_cleanup = spin_lock_irqsave(&tty->lock);
            wait_queue_remove_task(&tty->wait_queue_head, &tty->wait_queue_tail, curr_task_inner);
            spin_unlock_irqrestore(&tty->lock, flags_cleanup);

            continue;
        }

        while (tty->rx_head != tty->rx_tail && n < count) {
            char c = tty->rx_buffer[tty->rx_tail];
            tty->rx_tail = (tty->rx_tail + 1) % TTY_BUFFER_SIZE;
            buf[n++] = c;
            if (tty->canon_enabled && c == '\n') {
                spin_unlock_irqrestore(&tty->lock, flags);
                return (int)n;
            }
        }
        spin_unlock_irqrestore(&tty->lock, flags);
        if (n > 0) {
            break;
        }
    }
    return (int)n;
}

/*
 * tty_write - Writes data to TTY and local framebuffer console.
 */
int tty_write(struct tty *tty, const char *buf, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        unsigned long flags = spin_lock_irqsave(&tty->lock);

        /* Wait for TX space */
        while ((tty->tx_head + 1) % TTY_BUFFER_SIZE == tty->tx_tail) {
            struct task *curr = sched_get_current();
            curr->state = SCHED_TASK_BLOCKED;
            wait_queue_add(&tty->tx_wait_queue_head, &tty->tx_wait_queue_tail, curr);
            spin_unlock_irqrestore(&tty->lock, flags);
            schedule();
            flags = spin_lock_irqsave(&tty->lock);

            /* Cleanup after wake */
            wait_queue_remove_task(&tty->tx_wait_queue_head, &tty->tx_wait_queue_tail, curr);
        }

        char c = buf[i];
        if (c == '\n') {
            tty_put_tx_char(tty, '\r');
        }
        tty_put_tx_char(tty, c);

        fb_console_putc(c);
        tty_pump_tx(tty);
        spin_unlock_irqrestore(&tty->lock, flags);
    }
    return (int)count;
}
