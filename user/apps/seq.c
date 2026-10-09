/*
 * seq - print a sequence of integers: seq [first [step]] last.
 */

#include <stdio.h>
#include <stdlib.h>

static int parse(const char *s, long *out)
{
    const char *p = s;
    if (*p == '-' || *p == '+') {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return -1;
    }
    for (; *p; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
    }
    *out = atol(s);
    return 0;
}

int main(int argc, char **argv)
{
    long first = 1, step = 1, last;

    int ok = 0;
    if (argc == 2) {
        ok = parse(argv[1], &last) == 0;
    } else if (argc == 3) {
        ok = parse(argv[1], &first) == 0 && parse(argv[2], &last) == 0;
    } else if (argc == 4) {
        ok =
            parse(argv[1], &first) == 0 && parse(argv[2], &step) == 0 && parse(argv[3], &last) == 0;
    }
    if (!ok) {
        fprintf(stderr, "usage: seq [first [step]] last\n");
        return 2;
    }
    if (step == 0) {
        fprintf(stderr, "seq: step must not be 0\n");
        return 2;
    }

    for (long v = first; step > 0 ? v <= last : v >= last; v += step) {
        printf("%ld\n", v);
    }
    return 0;
}
