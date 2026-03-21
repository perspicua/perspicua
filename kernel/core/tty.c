/*
 * tty.c - Implementation of the Teletype (TTY) subsystem.
 *
 * This file handles character buffering, line-based input processing,
 * and synchronization between user-space reads/writes and hardware
 * interrupt handlers.
 */

#include "core/tty.h"

#include "stdio.h"
#include "string.h"

#include "driver/uart.h"
#include "driver/fb_console.h"

/* The primary system console TTY instance */
struct tty console_tty;

/*
 * tty_init - Initializes the TTY structure fields and buffers.
 */
void tty_init(struct tty* tty)
{
    memset(tty->rx_buffer, 0, TTY_BUFFER_SIZE);
    tty->rx_head = 0;
    tty->rx_tail = 0;

    memset(tty->tx_buffer, 0, TTY_BUFFER_SIZE);
    tty->tx_head = 0;
    tty->tx_tail = 0;

    tty->wait_queue_head = (void*)0;
    tty->wait_queue_tail = (void*)0;
    tty->tx_wait_queue_head = (void*)0;
    tty->tx_wait_queue_tail = (void*)0;
    tty->lock = (spinlock_t)SPINLOCK_INIT;
    tty->echo_enabled = 0;
    tty->canon_enabled = 0;

    printf("[ TTY  ] Console TTY initialized\n");
}

/*
 * wait_queue_add - Internal helper to append a task to a TTY wait queue.
 */
static void wait_queue_add(struct task** head, struct task** tail, struct task* t)
{
    t->next = (void*)0;
    if (*tail)
    {
        (*tail)->next = t;
        *tail = t;
    }
    else
    {
        *head = *tail = t;
    }
}

/*
 * wait_queue_remove - Internal helper to remove the first task from a TTY wait queue.
 */
static struct task* wait_queue_remove(struct task** head, struct task** tail)
{
    struct task* t = *head;
    if (t)
    {
        *head = t->next;
        if (*head == (void*)0)
        {
            *tail = (void*)0;
        }
        t->next = (void*)0;
    }
    return t;
}

/*
 * tty_pump_tx - Attempts to transfer as much data as possible from the
 * software TX buffer to the hardware UART FIFO.
 */
static void tty_pump_tx(struct tty* tty)
{
    size_t initial_tail = tty->tx_tail;
    while (tty->tx_head != tty->tx_tail)
    {
        if (mmio_read(uart_fr) & UART_FR_TXFF)
        {
            break;
        }

        mmio_write(uart_dr, (unsigned char)tty->tx_buffer[tty->tx_tail]);
        tty->tx_tail = (tty->tx_tail + 1) % TTY_BUFFER_SIZE;
    }

    /* Unblock tasks that were waiting for space in the TX buffer */
    if (tty->tx_tail != initial_tail)
    {
        struct task* t = wait_queue_remove(&tty->tx_wait_queue_head, &tty->tx_wait_queue_tail);
        if (t)
        {
            sched_unblock(t);
        }
    }

    /* Enable or disable the TX interrupt based on whether data remains */
    if (tty->tx_head != tty->tx_tail)
    {
        mmio_write(uart_imsc, mmio_read(uart_imsc) | UART_IMSC_TXIM);
    }
    else
    {
        mmio_write(uart_imsc, mmio_read(uart_imsc) & ~UART_IMSC_TXIM);
    }
}

/*
 * tty_put_tx_char - Inserts a single character into the TX circular buffer.
 */
static void tty_put_tx_char(struct tty* tty, char c)
{
    size_t next_tx_head = (tty->tx_head + 1) % TTY_BUFFER_SIZE;
    if (next_tx_head != tty->tx_tail)
    {
        tty->tx_buffer[tty->tx_head] = c;
        tty->tx_head = next_tx_head;
    }
}

/*
 * tty_echo_char - Renders a character to the framebuffer console and
 * queues it for transmission if echo is enabled.
 */
static void tty_echo_char(struct tty* tty, char c)
{
    if (!tty->echo_enabled)
    {
        return;
    }

    if (c == '\b' || c == 127)
    {
        tty_put_tx_char(tty, '\b');
        tty_put_tx_char(tty, ' ');
        tty_put_tx_char(tty, '\b');
        fb_console_putc('\b');
    }
    else
    {
        if (c == '\n')
        {
            tty_put_tx_char(tty, '\r');
        }
        tty_put_tx_char(tty, c);
        fb_console_putc(c);
    }
    tty_pump_tx(tty);
}

/*
 * tty_handle_rx - Core logic for processing received characters.
 */
void tty_handle_rx(struct tty* tty, char c)
{
    unsigned long flags = spin_lock_irqsave(&tty->lock);

    /* A zero character triggers a TX pump (used by the UART interrupt handler) */
    if (c == 0)
    {
        tty_pump_tx(tty);
        spin_unlock_irqrestore(&tty->lock, flags);
        return;
    }

    /* Normalize carriage returns to newlines */
    if (c == '\r')
    {
        c = '\n';
    }

    /* Handle backspace in canonical mode */
    if (tty->canon_enabled && (c == '\b' || c == 127))
    {
        if (tty->rx_head != tty->rx_tail)
        {
            tty->rx_head = (tty->rx_head + TTY_BUFFER_SIZE - 1) % TTY_BUFFER_SIZE;
            tty_echo_char(tty, c);
        }
        spin_unlock_irqrestore(&tty->lock, flags);
        return;
    }

    tty_echo_char(tty, c);

    /* Store the character in the receive buffer */
    size_t next_rx_head = (tty->rx_head + 1) % TTY_BUFFER_SIZE;
    if (next_rx_head != tty->rx_tail)
    {
        tty->rx_buffer[tty->rx_head] = c;
        tty->rx_head = next_rx_head;
    }

    /* Signal waiting readers if a full line is ready or if in raw mode */
    if (!tty->canon_enabled || c == '\n')
    {
        struct task* t = wait_queue_remove(&tty->wait_queue_head, &tty->wait_queue_tail);
        if (t)
        {
            sched_unblock(t);
        }
    }

    spin_unlock_irqrestore(&tty->lock, flags);
}

/*
 * tty_has_line - Checks if the receive buffer contains at least one newline.
 */
static int tty_has_line(struct tty* tty)
{
    size_t i = tty->rx_tail;
    while (i != tty->rx_head)
    {
        if (tty->rx_buffer[i] == '\n')
        {
            return 1;
        }
        i = (i + 1) % TTY_BUFFER_SIZE;
    }
    return 0;
}

/*
 * tty_read - Implementation of the TTY read operation.
 */
int tty_read(struct tty* tty, char* buf, size_t count)
{
    size_t n = 0;
    while (n < count)
    {
        unsigned long flags = spin_lock_irqsave(&tty->lock);

        int ready = (tty->rx_head != tty->rx_tail);
        if (tty->canon_enabled && !tty_has_line(tty))
        {
            ready = 0;
        }

        /* Block if no data is available */
        if (!ready)
        {
            struct task* curr = sched_get_current();
            wait_queue_add(&tty->wait_queue_head, &tty->wait_queue_tail, curr);
            spin_unlock_irqrestore(&tty->lock, flags);
            sched_block();
            continue;
        }

        /* Extract characters from the receive buffer */
        while (tty->rx_head != tty->rx_tail && n < count)
        {
            char c = tty->rx_buffer[tty->rx_tail];
            tty->rx_tail = (tty->rx_tail + 1) % TTY_BUFFER_SIZE;
            buf[n++] = c;
            if (tty->canon_enabled && c == '\n')
            {
                spin_unlock_irqrestore(&tty->lock, flags);
                return (int)n;
            }
        }
        spin_unlock_irqrestore(&tty->lock, flags);
        if (n > 0)
        {
            break;
        }
    }
    return (int)n;
}

/*
 * tty_write - Implementation of the TTY write operation.
 */
int tty_write(struct tty* tty, const char* buf, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        unsigned long flags = spin_lock_irqsave(&tty->lock);

        /* Block if the transmit buffer is full */
        while ((tty->tx_head + 1) % TTY_BUFFER_SIZE == tty->tx_tail)
        {
            struct task* curr = sched_get_current();
            wait_queue_add(&tty->tx_wait_queue_head, &tty->tx_wait_queue_tail, curr);
            spin_unlock_irqrestore(&tty->lock, flags);
            sched_block();
            flags = spin_lock_irqsave(&tty->lock);
        }

        char c = buf[i];
        if (c == '\n')
        {
            tty_put_tx_char(tty, '\r');
        }
        tty_put_tx_char(tty, c);

        /* Mirror the character to the framebuffer console */
        fb_console_putc(c);

        tty_pump_tx(tty);
        spin_unlock_irqrestore(&tty->lock, flags);
    }
    return (int)count;
}
