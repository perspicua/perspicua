/*
 * clear - clear the terminal screen.
 */

#include <unistd.h>

int main(void)
{
    static const char seq[] = "\033[2J\033[H";
    write(STDOUT_FILENO, seq, sizeof(seq) - 1);
    return 0;
}
