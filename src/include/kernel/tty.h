#ifndef _TTY_H_
#define _TTY_H_

#include "lib/types.h"
#include "kernel/lock.h"
#include "kernel/sched.h"

#define TTY_BUF_SIZE 256

struct tty
{
    char rx_buf[TTY_BUF_SIZE];
    size_t rx_head;
    size_t rx_tail;

    char tx_buf[TTY_BUF_SIZE];
    size_t tx_head;
    size_t tx_tail;

    struct task* wait_queue_head;
    struct task* wait_queue_tail;

    struct task* tx_wait_queue_head;
    struct task* tx_wait_queue_tail;

    spinlock_t lock;
    int echo;
    int canon; // canonical mode (wait for \n)
};

void tty_init(struct tty* tty);
void tty_handle_rx(struct tty* tty, char c);
int tty_read(struct tty* tty, char* buf, size_t count);
int tty_write(struct tty* tty, const char* buf, size_t count);

#endif // _TTY_H_
