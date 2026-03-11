#include "syscall.h"
#include "string.h"

#define PAGE_SIZE 4096
#define NUM_PAGES 2048 // 8MB BSS
static char big_array[NUM_PAGES * PAGE_SIZE];

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
    print("STRESS: Starting Memory Stress Test (8MB BSS Scan)...\n");
    int pid = sys_getpid();

    for (int iteration = 0; iteration < 5000; iteration++)
    {
        if (iteration % 100 == 0)
        {
            print("Iter ");
            print_int(iteration);
            print("...");
        }

        for (int i = 0; i < NUM_PAGES; i += 2)
        {
            big_array[i * PAGE_SIZE] = (char)(pid + iteration + i);
        }

        for (int i = NUM_PAGES - 1; i >= 0; i -= 3)
        {
            big_array[i * PAGE_SIZE + 100] = (char)(big_array[i * PAGE_SIZE] ^ (char)iteration);
        }

        sys_yield();

        if (iteration % 500 == 0)
        {
            print("\n  Scanned 8MB memory block x500.\n");
        }
    }

    print("STRESS: Memory test complete.\n");
    sys_exit();
    return 0;
}
