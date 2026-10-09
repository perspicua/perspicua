/*
 * sleep - pause for a number of seconds, e.g. `sleep 2` or `sleep 0.5`.
 */

#include <stdio.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: sleep seconds\n");
        return 2;
    }

    long secs = 0;
    long nsec = 0;
    long scale = 100000000;
    const char *p = argv[1];
    int digits = 0;

    for (; *p >= '0' && *p <= '9'; p++, digits++) {
        secs = secs * 10 + (*p - '0');
    }
    if (*p == '.') {
        for (p++; *p >= '0' && *p <= '9'; p++, digits++) {
            nsec += (*p - '0') * scale;
            scale /= 10;
        }
    }
    if (*p != '\0' || digits == 0) {
        fprintf(stderr, "sleep: invalid time '%s'\n", argv[1]);
        return 2;
    }

    struct timespec req = {.tv_sec = secs, .tv_nsec = nsec};
    nanosleep(&req, NULL);
    return 0;
}
