#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("[ STRESS ] Starting System Call Fuzzer...\n");

    /* 1. Invalid File Descriptors */
    printf("[ STRESS ] Testing invalid file descriptors...\n");
    char buf[16];
    sys_read(-1, buf, sizeof(buf));
    sys_read(9999, buf, sizeof(buf));
    sys_write(-1, "test", 4);
    sys_write(9999, "test", 4);
    sys_close(-1);
    sys_close(9999);
    sys_getdents(-1, buf, sizeof(buf));
    sys_getdents(9999, buf, sizeof(buf));

    /* 2. Invalid Pointers */
    printf("[ STRESS ] Testing null/invalid pointers...\n");
    sys_read(0, NULL, 10);
    sys_write(1, NULL, 10);
    sys_write(1, (void *)0xdeadbeef, 10);
    sys_open(NULL, 0);
    sys_open((void *)0xffffffffffffffff, 0);
    sys_chdir(NULL);
    sys_getcwd(NULL, 100);
    sys_stat(NULL, NULL);

    /* 3. Invalid exec arguments */
    printf("[ STRESS ] Testing invalid exec arguments...\n");
    sys_exec(NULL, NULL, NULL);
    char *bad_argv[] = {(char *)0xdeadbeef, NULL};
    sys_exec("/bin/ls", bad_argv, NULL);

    /* 4. Invalid PIDs and Wait options */
    printf("[ STRESS ] Testing invalid PIDs...\n");
    int status;
    sys_waitpid(-999, &status, 0);
    sys_waitpid(999999, NULL, 0);

    /* 5. Invalid memory operations */
    printf("[ STRESS ] Testing invalid mmap arguments...\n");
    sys_mmap(NULL, 0, 0, 0, -1, 0);
    sys_mmap((void *)0x1000, 0xffffffff, 0, 0, -1, 0);

    /* 6. Invalid signals */
    printf("[ STRESS ] Testing invalid signals...\n");
    sys_signal(-1, NULL);
    sys_signal(999, NULL);
    sys_kill(-1, 0);
    sys_kill(sys_getpid(), -1);
    sys_kill(999999, 9);
    sys_sigaction(-1, NULL, NULL);
    sys_sigprocmask(-1, NULL, NULL);

    /* 7. Invalid Sleep/Yield */
    printf("[ STRESS ] Testing boundary sleep/yield...\n");
    sys_sleep(0xffffffff); /* Extremely long sleep, should ideally not overflow or block forever
                              unexpectedly if checked */
    sys_yield();

    printf("[ STRESS ] System Call Fuzzer completed successfully.\n");
    printf("[ STRESS ] If the kernel did not panic, validation works!\n");

    return 0;
}
