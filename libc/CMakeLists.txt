set(LIBC_SRC_COMMON
    src/string.c
    src/printf.c
    src/malloc.c
    src/env.c
    src/dirent.c
    src/file.c
)

set(LIBC_SRC_KERNEL
    kernel/hooks.c
    kernel/panic.c
)

set(LIBC_SRC_USER
    user/hooks.c
    user/syscall.c
)

# libk (for kernel)
add_library(libk STATIC ${LIBC_SRC_COMMON} ${LIBC_SRC_KERNEL})
target_include_directories(libk PUBLIC include ../kernel/include ..)
target_compile_definitions(libk PRIVATE __KERNEL__)

# libc (for user space)
add_library(libc STATIC ${LIBC_SRC_COMMON} ${LIBC_SRC_USER})
target_include_directories(libc PUBLIC include ../kernel/include ..)
