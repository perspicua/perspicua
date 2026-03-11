#include "syscall.h"
#include "string.h"

void print(const char* s)
{
    sys_write(1, s, strlen(s));
}

void print_int(int n)
{
    char buf[16];
    int i = 0;
    if (n == 0)
    {
        buf[i++] = '0';
    }
    else
    {
        while (n > 0)
        {
            buf[i++] = (n % 10) + '0';
            n /= 10;
        }
    }
    buf[i] = '\0';
    for (int j = 0; j < i / 2; j++)
    {
        char tmp = buf[j];
        buf[j] = buf[i - j - 1];
        buf[i - j - 1] = tmp;
    }
    print(buf);
}

int main(void)
{
    print("STRESS: CPU load starting (syscalls + switching)...\n");

    volatile unsigned long long count = 0;
    int pid = sys_getpid();
    print("PID: ");
    print_int(pid);
    print("\n");

    for (int i = 0; i < 100000000; i++)
    {
        count += (i * 3) / 2;

        if (i % 1000 == 0)
        {
            sys_getpid(); // Stress syscall entry/exit
        }

        if (i % 5000 == 0)
        {
            sys_yield(); // Stress scheduler
        }

        if (i % 5000000 == 0)
        {
            print(".");
        }
    }

    print("\nSTRESS: CPU load complete.\n");
    sys_exit();
    return 0;
}
