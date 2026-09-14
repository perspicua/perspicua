#include <stddef.h>

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("[ STRESS ] Starting System Call Fuzzer...\n");

    printf("[ STRESS ] Testing invalid file descriptors...\n");
    char buf[16];
    read(-1, buf, sizeof(buf));
    read(9999, buf, sizeof(buf));
    write(-1, "test", 4);
    write(9999, "test", 4);
    pread(-1, buf, sizeof(buf), 0);
    pread(9999, buf, sizeof(buf), 0);
    pwrite(-1, "test", 4, 0);
    pwrite(9999, "test", 4, 0);
    close(-1);
    close(9999);
    getdents(-1, buf, sizeof(buf));
    getdents(9999, buf, sizeof(buf));

    printf("[ STRESS ] Testing null/invalid pointers...\n");
    read(0, NULL, 10);
    pread(0, NULL, 10, 0);
    write(1, NULL, 10);
    pwrite(1, NULL, 10, 0);
    write(1, (void *)0xdeadbeef, 10);
    pwrite(1, (void *)0xdeadbeef, 10, 0);
    open(NULL, 0);
    open((void *)0xffffffffffffffff, 0);
    chdir(NULL);
    getcwd(NULL, 100);
    stat(NULL, NULL);

    printf("[ STRESS ] Testing invalid exec arguments...\n");
    execve(NULL, NULL, NULL);
    char *bad_argv[] = {(char *)0xdeadbeef, NULL};
    execve("/bin/ls", bad_argv, NULL);

    printf("[ STRESS ] Testing invalid PIDs...\n");
    int status;
    waitpid(-999, &status, 0);
    waitpid(999999, NULL, 0);

    // 5. Invalid memory operations
    printf("[ STRESS ] Testing invalid mmap arguments...\n");
    mmap(NULL, 0, 0, 0, -1, 0);
    mmap((void *)0x1000, 0xffffffff, 0, 0, -1, 0);

    printf("[ STRESS ] Testing invalid signals...\n");
    signal(-1, NULL);
    signal(999, NULL);
    kill(-1, 0);
    kill(getpid(), -1);
    kill(999999, 9);
    sigaction(-1, NULL, NULL);
    sigprocmask(-1, NULL, NULL);

    // 7. New syscalls fuzzing
    printf("[ STRESS ] Testing extreme offsets and getppid...\n");
    pread(0, buf, 10, -1);
    pwrite(1, "test", 4, -1);
    getppid();

    printf("[ STRESS ] Testing boundary sleep/yield...\n");
    /* An extreme sleep: the kernel must reject or clamp it, not overflow. */
    struct timespec huge = {.tv_sec = 0xffffffffUL / 1000,
                            .tv_nsec = (0xffffffffUL % 1000) * 1000000};
    nanosleep(&huge, NULL);
    sched_yield();

    printf("[ STRESS ] System Call Fuzzer completed successfully.\n");
    printf("[ STRESS ] If the kernel did not panic, validation works!\n");

    return 0;
}
