#include "kernel/tty.h"
#include "driver/uart.h"
#include "lib/stdio.h"
#include "lib/string.h"

struct tty console_tty;

void tty_init(struct tty* tty)
{
    memset(tty->rx_buf, 0, TTY_BUF_SIZE);
    tty->rx_head = 0;
    tty->rx_tail = 0;

    memset(tty->tx_buf, 0, TTY_BUF_SIZE);
    tty->tx_head = 0;
    tty->tx_tail = 0;

    tty->wait_queue_head = NULL;
    tty->wait_queue_tail = NULL;
    tty->tx_wait_queue_head = NULL;
    tty->tx_wait_queue_tail = NULL;
    tty->lock = (spinlock_t)SPINLOCK_INIT;
    tty->echo = 1;
    tty->canon = 1;
}

static void wait_queue_add(struct task** head, struct task** tail, struct task* t)
{
    t->next = NULL;
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

static struct task* wait_queue_remove(struct task** head, struct task** tail)
{
    struct task* t = *head;
    if (t)
    {
        *head = t->next;
        if (*head == NULL)
            *tail = NULL;
        t->next = NULL;
    }
    return t;
}

static void tty_pump_tx(struct tty* tty)
{
    size_t initial_tail = tty->tx_tail;
    while (tty->tx_head != tty->tx_tail)
    {
        if (mmio_read(UART0_FR) & UART_FR_TXFF)
            break;

        mmio_write(UART0_DR, (unsigned char)tty->tx_buf[tty->tx_tail]);
        tty->tx_tail = (tty->tx_tail + 1) % TTY_BUF_SIZE;
    }

    if (tty->tx_tail != initial_tail)
    {
        struct task* t = wait_queue_remove(&tty->tx_wait_queue_head, &tty->tx_wait_queue_tail);
        if (t)
            sched_unblock(t);
    }

    if (tty->tx_head != tty->tx_tail)
        mmio_write(UART0_IMSC, mmio_read(UART0_IMSC) | UART_IMSC_TXIM);
    else
        mmio_write(UART0_IMSC, mmio_read(UART0_IMSC) & ~UART_IMSC_TXIM);
}

static void tty_put_tx_char(struct tty* tty, char c)
{
    size_t next_tx_head = (tty->tx_head + 1) % TTY_BUF_SIZE;
    if (next_tx_head != tty->tx_tail)
    {
        tty->tx_buf[tty->tx_head] = c;
        tty->tx_head = next_tx_head;
    }
}

void tty_handle_rx(struct tty* tty, char c)
{
    unsigned long flags = spin_lock_irqsave(&tty->lock);

    uint32_t mis = mmio_read(UART0_MIS);

    if (mis & UART_MIS_TXMIS)
    {
        tty_pump_tx(tty);
    }

    if (c != 0)
    {
        if (c == '\r')
            c = '\n';

        if (tty->echo)
        {
            if (c == '\n')
                tty_put_tx_char(tty, '\r');
            tty_put_tx_char(tty, c);
            tty_pump_tx(tty);
        }

        size_t next_rx_head = (tty->rx_head + 1) % TTY_BUF_SIZE;
        if (next_rx_head != tty->rx_tail)
        {
            tty->rx_buf[tty->rx_head] = c;
            tty->rx_head = next_rx_head;
        }

        if (!tty->canon || c == '\n')
        {
            struct task* t = wait_queue_remove(&tty->wait_queue_head, &tty->wait_queue_tail);
            if (t)
                sched_unblock(t);
        }
    }
    else if (mis & (UART_MIS_RXMIS | UART_MIS_RTMIS))
    {
        while (!(mmio_read(UART0_FR) & UART_FR_RXFE))
        {
            char rc = (char)(mmio_read(UART0_DR) & 0xFF);
            if (rc == '\r')
                rc = '\n';

            if (tty->echo)
            {
                if (rc == '\n')
                    tty_put_tx_char(tty, '\r');
                tty_put_tx_char(tty, rc);
                tty_pump_tx(tty);
            }

            size_t next_rx_head = (tty->rx_head + 1) % TTY_BUF_SIZE;
            if (next_rx_head != tty->rx_tail)
            {
                tty->rx_buf[tty->rx_head] = rc;
                tty->rx_head = next_rx_head;
            }

            if (!tty->canon || rc == '\n')
            {
                struct task* t = wait_queue_remove(&tty->wait_queue_head, &tty->wait_queue_tail);
                if (t)
                    sched_unblock(t);
            }
        }
    }

    spin_unlock_irqrestore(&tty->lock, flags);
}

static int tty_has_line(struct tty* tty)
{
    size_t i = tty->rx_tail;
    while (i != tty->rx_head)
    {
        if (tty->rx_buf[i] == '\n')
            return 1;
        i = (i + 1) % TTY_BUF_SIZE;
    }
    return 0;
}

int tty_read(struct tty* tty, char* buf, size_t count)
{
    size_t n = 0;
    while (n < count)
    {
        unsigned long flags = spin_lock_irqsave(&tty->lock);

        int ready = (tty->rx_head != tty->rx_tail);
        if (tty->canon && !tty_has_line(tty))
            ready = 0;

        if (!ready)
        {
            struct task* curr = sched_get_current();
            wait_queue_add(&tty->wait_queue_head, &tty->wait_queue_tail, curr);
            spin_unlock_irqrestore(&tty->lock, flags);
            sched_block();
            continue;
        }

        while (tty->rx_head != tty->rx_tail && n < count)
        {
            char c = tty->rx_buf[tty->rx_tail];
            tty->rx_tail = (tty->rx_tail + 1) % TTY_BUF_SIZE;
            buf[n++] = c;
            if (tty->canon && c == '\n')
            {
                spin_unlock_irqrestore(&tty->lock, flags);
                return (int)n;
            }
        }
        spin_unlock_irqrestore(&tty->lock, flags);
        if (n > 0)
            break;
    }
    return (int)n;
}

int tty_write(struct tty* tty, const char* buf, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        unsigned long flags = spin_lock_irqsave(&tty->lock);

        while ((tty->tx_head + 1) % TTY_BUF_SIZE == tty->tx_tail)
        {
            struct task* curr = sched_get_current();
            wait_queue_add(&tty->tx_wait_queue_head, &tty->tx_wait_queue_tail, curr);
            spin_unlock_irqrestore(&tty->lock, flags);
            sched_block();
            flags = spin_lock_irqsave(&tty->lock);
        }

        if (buf[i] == '\n')
            tty_put_tx_char(tty, '\r');
        tty_put_tx_char(tty, buf[i]);

        tty_pump_tx(tty);
        spin_unlock_irqrestore(&tty->lock, flags);
    }
    return (int)count;
}
