#ifndef _PANIC_H_
#define _PANIC_H_

/* Set to 1 by panic(). All cores poll this and halt themselves. */
extern volatile int kernel_panicked;

void panic(const char* msg, const char* file, int line) __attribute__((noreturn));

#define PANIC(msg) panic((msg), __FILE__, __LINE__)
#define ASSERT(cond)                                            \
    do                                                          \
    {                                                           \
        if (!(cond))                                            \
            panic("ASSERT failed: " #cond, __FILE__, __LINE__); \
    } while (0)
#endif // _PANIC_H_
