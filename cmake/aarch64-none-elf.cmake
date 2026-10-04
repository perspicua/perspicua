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

# No FP or SIMD anywhere, kernel or userspace. boot.S leaves CPACR_EL1.FPEN
# clear to match, so an FP instruction faults instead of running against
# registers the trap frame does not save. Lifting this means implementing lazy
# FP first -- see the note on struct exception_trap_frame.
set(ARCH_FLAGS "-mcpu=cortex-a72+nosimd -mgeneral-regs-only")
set(COMMON_FLAGS "${ARCH_FLAGS} -Wall -Wextra -Werror -ffreestanding -nostdlib -std=gnu11 -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -mno-outline-atomics")

set(CMAKE_C_FLAGS "${COMMON_FLAGS}" CACHE STRING "Common C flags")
set(CMAKE_ASM_FLAGS "${ARCH_FLAGS}" CACHE STRING "Common ASM flags")

# Build type specific flags.
#
# RelWithDebInfo is the default (see CMakeLists.txt): -O0 hides the bugs that
# only appear once the optimiser reorders things, and its timing says nothing
# about how the kernel behaves on hardware. -g costs nothing in the image --
# the debug sections are not loaded -- and keeps the symbol table and
# backtraces intact. Debug stays available for stepping.
#
# No -DNDEBUG anywhere: ASSERT is how the kernel catches broken invariants, and
# assert() is how the userspace test apps check anything at all -- test_syscall
# alone wraps a hundred syscalls in one. Compiling those out does not make the
# tests faster, it makes them pass without running.
#
# FORCE, because CMake seeds its own defaults for every build type on the first
# configure -- RelWithDebInfo's includes -DNDEBUG -- and a plain
# set(... CACHE ...) will not displace a value already in the cache. Without it
# these flags apply only to build trees created after this line was written.
set(CMAKE_C_FLAGS_DEBUG "-O0 -g" CACHE STRING "Debug C flags" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g" CACHE STRING "Default C flags" FORCE)
set(CMAKE_C_FLAGS_RELEASE "-O2" CACHE STRING "Release C flags" FORCE)

# Linker flags: Generate map file
set(CMAKE_EXE_LINKER_FLAGS "-Wl,-Map,kernel8.map" CACHE STRING "Linker flags")
