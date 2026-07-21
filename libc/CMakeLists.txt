# Sources are grouped by what they depend on, not by who happens to call them.
# An object with unresolved dependencies sits harmlessly in a static archive
# until something references it, so a misplaced file here stays invisible until
# the link that finally pulls it out. Keep the tiers honest.

# Freestanding: self-contained, no syscalls and no kernel subsystems.
# arch/ holds the AArch64 assembly members; ASM is enabled project-wide by
# the top-level project(perspicua C ASM), so no per-target language setup.
set(LIBC_SRC_COMMON
    src/string.c
    src/printf.c
    src/stdlib.c
    src/ctype.c
    arch/setjmp.S
)

# Depends on kernel subsystems (heap, UART, panic).
set(LIBC_SRC_KERNEL
    kernel/hooks.c
    kernel/malloc.c
    kernel/panic.c
)

# Depends on syscalls or on userspace process state.
set(LIBC_SRC_USER
    user/malloc.c
    user/file.c
    user/dirent.c
    user/env.c
    user/hooks.c
    user/syscall.c
    user/errno.c
    user/assert.c
)

# libk (for kernel)
add_library(libk STATIC ${LIBC_SRC_COMMON} ${LIBC_SRC_KERNEL})
target_include_directories(libk PUBLIC include ../kernel/include ..)
target_compile_definitions(libk PRIVATE __KERNEL__)

# libc (for user space)
add_library(libc STATIC ${LIBC_SRC_COMMON} ${LIBC_SRC_USER})
target_include_directories(libc PUBLIC include ../kernel/include ..)
