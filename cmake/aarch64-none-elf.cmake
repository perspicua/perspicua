set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(APPLE)
    set(DEFAULT_CROSS_COMPILE "aarch64-elf-")
else()
    set(DEFAULT_CROSS_COMPILE "aarch64-linux-gnu-")
endif()

set(CROSS_COMPILE ${DEFAULT_CROSS_COMPILE} CACHE STRING "Cross-compiler prefix")

set(CMAKE_C_COMPILER ${CROSS_COMPILE}gcc)
set(CMAKE_ASM_COMPILER ${CROSS_COMPILE}gcc)
set(CMAKE_AR ${CROSS_COMPILE}ar)
set(CMAKE_OBJCOPY ${CROSS_COMPILE}objcopy)
set(CMAKE_OBJDUMP ${CROSS_COMPILE}objdump)
set(CMAKE_NM ${CROSS_COMPILE}nm)
set(CMAKE_SIZE ${CROSS_COMPILE}size)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ARCH_FLAGS "-mcpu=cortex-a72+nosimd -mgeneral-regs-only")
set(COMMON_FLAGS "${ARCH_FLAGS} -Wall -Wextra -Werror -ffreestanding -nostdlib -std=gnu11 -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -mno-outline-atomics")

set(CMAKE_C_FLAGS "${COMMON_FLAGS}" CACHE STRING "Common C flags")
set(CMAKE_ASM_FLAGS "${ARCH_FLAGS}" CACHE STRING "Common ASM flags")

# Build type specific flags
set(CMAKE_C_FLAGS_DEBUG "-O0 -g -DDEBUG" CACHE STRING "Debug C flags")
set(CMAKE_C_FLAGS_RELEASE "-O2" CACHE STRING "Release C flags")

# Linker flags: Generate map file
set(CMAKE_EXE_LINKER_FLAGS "-Wl,-Map,kernel8.map" CACHE STRING "Linker flags")
