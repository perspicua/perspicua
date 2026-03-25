#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "uapi/mman.h"
#include "signals.h"

// 1. Stack Stress
int recursive_function(int depth)
{
    volatile char buffer[1024];
    for (int i = 0; i < 1024; i++)
    {
        buffer[i] = (char)(depth & 0xFF);
    }
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

void stress_stack()
{
    printf("[STRESS] Testing deep recursion (stack size)...\n");
    int res = recursive_function(100);
    printf("[STRESS] Recursion depth reached, returned %d\n", res);
}

// 2. Process / Fork Stress
void stress_fork()
{
    printf("[STRESS] Testing process forks (table size & concurrency)...\n");
    int num_processes = 200;
    int pids[200];
    int forked = 0;

    for (int i = 0; i < num_processes; i++)
    {
        int pid = sys_fork();
        if (pid < 0)
        {
            printf("[STRESS] Fork failed at process %d (Expected if limit reached)\n", i);
            break;
        }
        if (pid == 0)
        {
            // Child: do some minor work and exit
            sys_sleep(7000);
            sys_exit(0);
        }
        else
        {
            pids[i] = pid;
            forked++;
        }
    }

    printf("[STRESS] Forked %d processes. Waiting...\n", forked);
    for (int i = 0; i < forked; i++)
    {
        int status;
        sys_waitpid(pids[i], &status);
    }
    printf("[STRESS] All child processes exited.\n");
}

// 3. Pipe & IPC Stress
void stress_pipe()
{
    printf("[STRESS] Testing heavy pipe IPC...\n");
    int pipefd[2];
    if (sys_pipe(pipefd) < 0)
    {
        printf("[STRESS] Pipe creation failed.\n");
        return;
    }

    int pid = sys_fork();
    if (pid == 0)
    {
        // Child: reader
        sys_close(pipefd[1]);
        char buf[256];
        int total = 0;
        while (1)
        {
            int n = sys_read(pipefd[0], buf, sizeof(buf));
            if (n <= 0)
                break;
            total += n;
        }
        sys_close(pipefd[0]);
        // Return modulo as exit status
        sys_exit(total % 256);
    }
    else
    {
        // Parent: writer
        sys_close(pipefd[0]);
        char msg[128];
        memset(msg, 'A', sizeof(msg));
        int write_count = 500;
        for (int i = 0; i < write_count; i++)
        {
            sys_write(pipefd[1], msg, sizeof(msg));
        }
        sys_close(pipefd[1]);  // EOF

        int status;
        sys_waitpid(pid, &status);
        int expected = (write_count * sizeof(msg)) % 256;
        if (status == expected)
        {
            printf("[STRESS] Pipe IPC done. Data transferred correctly.\n");
        }
        else
        {
            printf("[STRESS] Pipe IPC failed. Expected %d, got %d\n", expected, status);
        }
    }
}

// 4. File Descriptor Exhaustion Stress
void stress_fd()
{
    printf("[STRESS] Testing file descriptor limits...\n");
    int fds[1024];
    int opened = 0;
    for (int i = 0; i < 1024; i++)
    {
        int fd = sys_open("/big.txt", VFS_O_RDONLY);
        if (fd < 0)
        {
            break;
        }
        fds[opened++] = fd;
    }
    printf("[STRESS] Opened %d files successfully.\n", opened);
    for (int i = 0; i < opened; i++)
    {
        sys_close(fds[i]);
    }
    printf("[STRESS] Closed all %d files.\n", opened);
}

// 5. Memory Mapping Stress
void stress_mmap()
{
    printf("[STRESS] Testing heavy mmap allocation & demand paging...\n");
    int num_allocs = 50;
    size_t alloc_size = 4096 * 8;  // 32KB per alloc -> 1.6MB total
    void* ptrs[50];
    int alloced = 0;

    for (int i = 0; i < num_allocs; i++)
    {
        void* p = sys_mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
        {
            printf("[STRESS] mmap failed at allocation %d\n", i);
            break;
        }
        ptrs[alloced++] = p;
        // Fault in the pages
        memset(p, (char)(i & 0xFF), alloc_size);
    }

    printf("[STRESS] Mmap'd %d blocks.\n", alloced);
    // Verify
    int ok = 1;
    for (int i = 0; i < alloced; i++)
    {
        char* p = (char*)ptrs[i];
        for (size_t j = 0; j < alloc_size; j++)
        {
            if (p[j] != (char)(i & 0xFF))
            {
                ok = 0;
                break;
            }
        }
    }
    if (ok)
    {
        printf("[STRESS] Mmap data verification PASSED.\n");
    }
    else
    {
        printf("[STRESS] Mmap data verification FAILED.\n");
    }
}

// 6. Signal Stress
volatile int sig_count = 0;
volatile int stop_child = 0;  // Added stop flag

void sig_handler(int sig)
{
    if (sig == SIGNAL_USR1)
    {
        printf("got a signal %d, curr count: %d\n", sig, sig_count);
        sig_count++;
    }
    else if (sig == SIGNAL_USR2)
    {
        stop_child = 1;  // Parent says we are done
    }
}
void stress_signals()
{
    printf("[STRESS] Testing signal delivery...\n");

    sys_sigrestore((uintptr_t)sys_sigreturn);

    sys_signal(SIGNAL_USR1, sig_handler);
    sys_signal(SIGNAL_USR2, sig_handler);

    int pid = sys_fork();
    if (pid == 0)
    {
        while (!stop_child)
        {
            sys_yield();
        }
        sys_exit(sig_count);
    }
    else
    {
        for (int i = 0; i < 100; i++)
        {
            sys_kill(pid, SIGNAL_USR1);
            sys_yield();
        }

        sys_kill(pid, SIGNAL_USR2);

        int status;
        sys_waitpid(pid, &status);

        printf("[STRESS] Signal delivery done. Child caught %d signals (coalescing is normal).\n", status);
    }
}

int main(int argc __attribute__((unused)), char** argv __attribute__((unused)))
{
    printf("[STRESS] Starting comprehensive stress test...\n");
    stress_stack();
    stress_fork();
    stress_pipe();
    stress_fd();
    stress_mmap();
    stress_signals();
    printf("[STRESS] Comprehensive stress test complete.\n");
    return 0;
}
