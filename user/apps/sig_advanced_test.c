#include "stdio.h"
#include "syscall.h"
#include "signals.h"

int volatile sig_received = 0;

void handle_sigint(int sig)
{
    printf("Caught signal %d!\n", sig);
    sig_received = sig;
}

int main()
{
    printf("Advanced Signal test starting...\n");

    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sa.sa_mask = 0;
    sa.sa_flags = SA_RESTORER;
    /* crt0 should have registered a restorer, but we can also set it here if we want.
     * Actually, crt0 calls sys_sigrestore which sets default_sigrestorer.
     * Our signal_handle_pending uses default_sigrestorer if SA_RESTORER is not set in sa_flags.
     * Wait, I set SA_RESTORER in sa_flags but didn't set sa_restorer in the struct in my test code.
     * Better to NOT set SA_RESTORER and let it use the default.
     */
    sa.sa_flags = 0;
    sa.sa_restorer = NULL;

    if (sys_sigaction(SIGNAL_INT, &sa, NULL) < 0)
    {
        printf("Failed to set sigaction\n");
        return 1;
    }

    /* Test signal masking */
    sigset_t set = (1u << (SIGNAL_INT - 1));
    printf("Masking SIGNAL_INT...\n");
    sys_sigprocmask(SIG_BLOCK, &set, NULL);

    printf("Sending SIGNAL_INT to ourselves...\n");
    sys_kill(sys_getpid(), SIGNAL_INT);

    sys_yield();
    if (sig_received == 0)
    {
        printf("Signal correctly masked.\n");
    }
    else
    {
        printf("FAILED: Signal was not masked!\n");
    }

    printf("Unmasking SIGNAL_INT...\n");
    sys_sigprocmask(SIG_UNBLOCK, &set, NULL);

    /* Signal should be delivered now */
    sys_yield();

    if (sig_received == SIGNAL_INT)
    {
        printf("Signal correctly delivered after unmasking.\n");
    }
    else
    {
        printf("FAILED: Signal was not delivered!\n");
    }

    printf("Advanced Signal test finished.\n");
    return 0;
}
