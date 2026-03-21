#include "syscall.h"
#include "stdio.h"
#include "string.h"

// Recursion test to test user stack size
int recursive_function(int depth)
{
    volatile char buffer[256];
    for (int i = 0; i < 256; i++)
    {
        buffer[i] = (char)(depth & 0xFF);
    }
    // Prevent optimization out
    if (buffer[0] != (char)(depth & 0xFF))
    {
        return -1;
    }

    if (depth > 0)
    {
        return recursive_function(depth - 1) + 1;
    }
    return 0;
}

int main(int argc __attribute__((unused)), char** argv __attribute__((unused)))
{
    printf("[STRESS] Starting stress test...\n");

    printf("[STRESS] Testing deep recursion (stack size)...\n");
    int res = recursive_function(300);  // 300 * 256 bytes = ~75KB stack
    printf("[STRESS] Recursion depth reached, returned %d\n", res);

    printf("[STRESS] Testing process forks (table size)...\n");
    int num_processes = 200;  // Stress the process table (1024 limit)
    int pids[200];
    int forked = 0;

    for (int i = 0; i < num_processes; i++)
    {
        int pid = sys_fork();
        if (pid < 0)
        {
            printf("[STRESS] Fork failed at process %d\n", i);
            break;
        }
        if (pid == 0)
        {
            // Child process
            sys_sleep(500);  // Sleep briefly
            sys_exit(0);
        }
        else
        {
            pids[i] = pid;
            forked++;
        }
    }

    printf("[STRESS] Forked %d processes. Waiting for them to complete...\n", forked);
    for (int i = 0; i < forked; i++)
    {
        int status;
        sys_waitpid(pids[i], &status);
    }

    printf("[STRESS] All child processes have exited.\n");
    printf("[STRESS] Stress test complete.\n");

    return 0;
}
