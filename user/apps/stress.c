#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "uapi/mman.h"
#include "signals.h"
#include "uapi/errors.h"

// 1. Stack Stress
int recursive_function(int depth)
{
    volatile char buffer[1024];
    for (int i = 0; i < 1024; i++) {
        buffer[i] = (char)(depth & 0xFF);
    }
    if (buffer[0] != (char)(depth & 0xFF)) {
        return -1;
    }
    if (depth > 0) {
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
    int *pids = malloc(num_processes * sizeof(int));
    if (!pids) {
        printf("[STRESS] Failed to allocate pids array\n");
        return;
    }
    int forked = 0;

    for (int i = 0; i < num_processes; i++) {
        int pid = sys_fork();
        if (pid < 0) {
            printf("[STRESS] Fork failed at process %d (Expected if limit reached)\n", i);
            break;
        }
        if (pid == 0) {
            // Child: do some minor work and exit
            sys_sleep(70);
            sys_exit(0);
        } else {
            pids[i] = pid;
            forked++;
        }
    }

    printf("[STRESS] Forked %d processes. Waiting...\n", forked);
    for (int i = 0; i < forked; i++) {
        int status;
        sys_waitpid(pids[i], &status, 0);
    }
    free(pids);
    printf("[STRESS] All child processes exited.\n");
}

// 3. Pipe & IPC Stress
void stress_pipe()
{
    printf("[STRESS] Testing heavy pipe IPC...\n");
    int pipefd[2];
    if (sys_pipe(pipefd) < 0) {
        printf("[STRESS] Pipe creation failed.\n");
        return;
    }

    int pid = sys_fork();
    if (pid == 0) {
        // Child: reader
        sys_close(pipefd[1]);
        char buf[256];
        int total = 0;
        while (1) {
            int n = sys_read(pipefd[0], buf, sizeof(buf));
            if (n <= 0)
                break;
            total += n;
        }
        sys_close(pipefd[0]);
        // Return modulo as exit status
        sys_exit(total % 256);
    } else {
        // Parent: writer
        sys_close(pipefd[0]);
        char msg[128];
        memset(msg, 'A', sizeof(msg));
        int write_count = 500;
        for (int i = 0; i < write_count; i++) {
            sys_write(pipefd[1], msg, sizeof(msg));
        }
        sys_close(pipefd[1]); // EOF

        int status;
        sys_waitpid(pid, &status, 0);
        int expected = (write_count * sizeof(msg)) % 256;
        if (status == expected) {
            printf("[STRESS] Pipe IPC done. Data transferred correctly.\n");
        } else {
            printf("[STRESS] Pipe IPC failed. Expected %d, got %d\n", expected, status);
        }
    }
}

// 4. File Descriptor Exhaustion Stress
void stress_fd()
{
    printf("[STRESS] Testing file descriptor limits...\n");
    int num_fds = 1024;
    int *fds = malloc(sizeof(int) * num_fds);
    if (!fds) {
        printf("[STRESS] Failed to allocate fds array\n");
        return;
    }
    int opened = 0;
    for (int i = 0; i < num_fds; i++) {
        int fd = sys_open("/big.txt", VFS_O_RDONLY);
        if (fd < 0) {
            break;
        }
        fds[opened++] = fd;
    }
    printf("[STRESS] Opened %d files successfully.\n", opened);
    for (int i = 0; i < opened; i++) {
        sys_close(fds[i]);
    }
    free(fds);
    printf("[STRESS] Closed all %d files.\n", opened);
}

// 5. Memory Mapping Stress
void stress_mmap()
{
    printf("[STRESS] Testing heavy mmap allocation & demand paging...\n");
    int num_allocs = 50;
    size_t alloc_size = 4096 * 8; // 32KB per alloc -> 1.6MB total
    void **ptrs = malloc(num_allocs * sizeof(void *));
    if (!ptrs) {
        printf("[STRESS] Failed to allocate ptrs array\n");
        return;
    }
    int alloced = 0;

    for (int i = 0; i < num_allocs; i++) {
        void *p =
            sys_mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
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
    for (int i = 0; i < alloced; i++) {
        char *p = (char *)ptrs[i];
        for (size_t j = 0; j < alloc_size; j++) {
            if (p[j] != (char)(i & 0xFF)) {
                ok = 0;
                break;
            }
        }
    }
    if (ok) {
        printf("[STRESS] Mmap data verification PASSED.\n");
    } else {
        printf("[STRESS] Mmap data verification FAILED.\n");
    }
    free(ptrs);
}

// 6. Signal Stress
volatile int sig_count = 0;
volatile int stop_child = 0; // Added stop flag

void sig_handler(int sig)
{
    if (sig == SIGNAL_USR1) {
        printf("got a signal %d, curr count: %d\n", sig, sig_count);
        sig_count++;
    } else if (sig == SIGNAL_USR2) {
        stop_child = 1; // Parent says we are done
    }
}
void stress_signals()
{
    printf("[STRESS] Testing signal delivery...\n");

    sys_sigrestore((uintptr_t)sys_sigreturn);

    sys_signal(SIGNAL_USR1, sig_handler);
    sys_signal(SIGNAL_USR2, sig_handler);

    int pid = sys_fork();
    if (pid == 0) {
        while (!stop_child) {
            sys_yield();
        }
        sys_exit(sig_count);
    } else {
        for (int i = 0; i < 100; i++) {
            sys_kill(pid, SIGNAL_USR1);
            sys_yield();
        }

        sys_kill(pid, SIGNAL_USR2);

        int status;
        sys_waitpid(pid, &status, 0);

        printf("[STRESS] Signal delivery done. Child caught %d signals (coalescing is normal).\n",
               status);
    }
}

// 7. VFS & Path Stress
void stress_vfs()
{
    printf("[STRESS] Testing VFS operations (cwd, chdir, stat, getdents, dup2)...\n");

    char start_cwd[256];
    if (sys_getcwd(start_cwd, sizeof(start_cwd)) < 0) {
        printf("[STRESS] sys_getcwd failed\n");
        return;
    }
    printf("[STRESS] Starting CWD: %s\n", start_cwd);

    if (sys_chdir("/") < 0) {
        printf("[STRESS] sys_chdir(\"/\") failed\n");
        return;
    }

    char root_cwd[256];
    sys_getcwd(root_cwd, sizeof(root_cwd));
    if (strcmp(root_cwd, "/") != 0) {
        printf("[STRESS] CWD mismatch: expected /, got %s\n", root_cwd);
    }

    struct stat st;
    if (sys_stat("/", &st) < 0) {
        printf("[STRESS] sys_stat(\"/\") failed\n");
    } else {
        printf("[STRESS] Stat root: size=%lu, mode=0x%x\n", (unsigned long)st.st_size, st.st_mode);
    }

    int fd = sys_open("/", VFS_O_RDONLY);
    if (fd >= 0) {
        struct vfs_dirent *dent = malloc(sizeof(struct vfs_dirent) * 16);
        if (dent) {
            int n = sys_getdents(fd, dent, sizeof(struct vfs_dirent) * 16);
            if (n > 0) {
                printf("[STRESS] Found entries in root directory\n");
            }
            free(dent);
        }
        sys_close(fd);
    }

    /* Test dup2 */
    int null_fd = sys_open("/dev/null", VFS_O_WRONLY);
    if (null_fd >= 0) {
        int saved_stdout = 20; // Use a more reasonable FD
        if (sys_dup2(1, saved_stdout) >= 0) {
            sys_dup2(null_fd, 1);
            printf("This should go to /dev/null\n");
            sys_dup2(saved_stdout, 1);
            sys_close(saved_stdout);
        }
        sys_close(null_fd);
    }

    sys_chdir(start_cwd);
    printf("[STRESS] VFS operations test complete.\n");
}

// 8. Advanced Signal Stress
volatile int adv_sig_caught = 0;
void adv_sig_handler(int sig)
{
    adv_sig_caught = sig;
}

void stress_signals_advanced()
{
    printf("[STRESS] Testing advanced signals (sigaction, sigprocmask, sigpending)...\n");

    struct sigaction sa;
    sa.sa_handler = adv_sig_handler;
    sa.sa_flags = 0;
    sa.sa_mask = 0;
    sa.sa_restorer = sys_sigreturn;

    sys_sigrestore((uintptr_t)sys_sigreturn);

    if (sys_sigaction(SIGNAL_USR1, &sa, NULL) < 0) {
        printf("[STRESS] sys_sigaction failed\n");
        return;
    }

    /* Block USR1 */
    sigset_t mask = (1 << (SIGNAL_USR1 - 1));
    sys_sigprocmask(SIG_BLOCK, &mask, NULL);

    sys_kill(sys_getpid(), SIGNAL_USR1);

    sigset_t pending;
    sys_sigpending(&pending);
    if (pending & mask) {
        printf("[STRESS] SIGNAL_USR1 is correctly pending\n");
    } else {
        printf("[STRESS] SIGNAL_USR1 is NOT pending!\n");
    }

    if (adv_sig_caught != 0) {
        printf("[STRESS] SIGNAL_USR1 was delivered while blocked!\n");
    }

    /* Unblock USR1 */
    sys_sigprocmask(SIG_UNBLOCK, &mask, NULL);
    sys_yield();

    if (adv_sig_caught == SIGNAL_USR1) {
        printf("[STRESS] SIGNAL_USR1 caught after unblocking\n");
    } else {
        printf("[STRESS] SIGNAL_USR1 was NOT caught!\n");
    }

    printf("[STRESS] Advanced signal test complete.\n");
}

// 9. Exec Stress
void stress_exec()
{
    printf("[STRESS] Testing process exec (of self with argument)...\n");

    int pid = sys_fork();
    if (pid == 0) {
        char *argv[] = {"stress", "--exec-child", NULL};
        sys_exec("/bin/stress.elf", argv);
        /* Try without /bin/ prefix as sh does */
        sys_exec("stress.elf", argv);
        sys_exit(1);
    } else if (pid > 0) {
        int status;
        sys_waitpid(pid, &status, 0);
        if (status == 0) {
            printf("[STRESS] sys_exec succeeded\n");
        } else {
            printf("[STRESS] sys_exec failed (status %d)\n", status);
        }
    }
}

// 10. Malloc / Heap Stress
void stress_malloc()
{
    printf("[STRESS] Testing malloc/free churn...\n");

    const int iter = 50;
    const int allocs_per_iter = 30;
    void **ptrs = malloc(sizeof(void *) * allocs_per_iter);
    if (!ptrs)
        return;

    for (int i = 0; i < iter; i++) {
        for (int j = 0; j < allocs_per_iter; j++) {
            size_t sz = (size_t)((i + j) * 123) % 4096 + 1;
            ptrs[j] = malloc(sz);
            if (ptrs[j]) {
                memset(ptrs[j], 0xCC, sz);
            }
        }
        for (int j = 0; j < allocs_per_iter; j++) {
            free(ptrs[j]);
        }
    }

    free(ptrs);
    printf("[STRESS] Malloc churn test complete.\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--exec-child") == 0) {
        /* We were re-executed by stress_exec() */
        sys_exit(0);
    }

    printf("[STRESS] Starting comprehensive stress test...\n");
    stress_stack();
    stress_fork();
    stress_pipe();
    stress_fd();
    stress_mmap();
    stress_signals();
    stress_signals_advanced();
    stress_vfs();
    stress_exec();
    stress_malloc();
    printf("[STRESS] Comprehensive stress test complete.\n");
    return 0;
}
