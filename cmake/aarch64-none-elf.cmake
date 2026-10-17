set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# The compiler prefix: the first of these on PATH, unless -DCROSS_COMPILE= names one.
# aarch64-none-elf is Arm's bare-metal toolchain, aarch64-elf is Homebrew's, and
# aarch64-linux-gnu is the hosted cross compiler Linux distributions package.
if(NOT CROSS_COMPILE)
    foreach(prefix aarch64-none-elf- aarch64-elf- aarch64-linux-gnu-)
        find_program(PERSPICUA_CC_${prefix} ${prefix}gcc)
        if(PERSPICUA_CC_${prefix})
            set(found_prefix ${prefix})
            break()
        endif()
    endforeach()
    if(NOT found_prefix)
        message(FATAL_ERROR "No AArch64 cross compiler on PATH: install aarch64-none-elf-gcc, "
                            "aarch64-elf-gcc or aarch64-linux-gnu-gcc, or pass -DCROSS_COMPILE=<prefix>")
    endif()
    set(CROSS_COMPILE ${found_prefix} CACHE STRING "Cross-compiler prefix")
endif()

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
# Only the compiler's own headers (stddef.h, stdint.h, stdarg.h and the like):
# a hosted cross gcc would otherwise also search glibc's, and anything the tree
# does not provide would quietly come from there. libc/include has the rest.
execute_process(COMMAND ${CROSS_COMPILE}gcc -print-file-name=include
                OUTPUT_VARIABLE GCC_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(INCLUDE_FLAGS "-nostdinc -isystem ${GCC_INCLUDE}")

# Never PIE: a hosted Linux cross gcc defaults to it, which puts const pointer
# tables such as syscall_table in writable .data and links in relocations
# nothing applies. -Wdate-time: the build date comes from build_info.h, which
# every build refreshes, never from __DATE__ in whichever file last recompiled.
set(COMMON_FLAGS "${ARCH_FLAGS} ${INCLUDE_FLAGS} -Wall -Wextra -Werror -Wdate-time -ffreestanding -nostdlib -std=gnu11 -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -mno-outline-atomics -fno-pie")

# FORCE on every flag variable here, for the reason given at the build-type flags below.
set(CMAKE_C_FLAGS "${COMMON_FLAGS}" CACHE STRING "Common C flags" FORCE)
set(CMAKE_ASM_FLAGS "${ARCH_FLAGS} ${INCLUDE_FLAGS}" CACHE STRING "Common ASM flags" FORCE)

# Build type specific flags.
#
# RelWithDebInfo is the default (see CMakeLists.txt): -O0 hides the bugs that
# only appear once the optimiser reorders things, and its timing says nothing
# about how the kernel behaves on hardware. -g costs nothing in the image --
# the debug sections are not loaded -- and keeps the symbol table and
# backtraces intact. Debug stays available for stepping.
#
# No -DNDEBUG anywhere: ASSERT is how the kernel catches broken invariants.
# Compiling it out does not make the tests faster, it makes them pass without
# running.
#
# FORCE, because CMake seeds its own defaults for every build type on the first
# configure -- RelWithDebInfo's includes -DNDEBUG -- and a plain
# set(... CACHE ...) will not displace a value already in the cache. Without it
# these flags apply only to build trees created after this line was written.
set(CMAKE_C_FLAGS_DEBUG "-O0 -g" CACHE STRING "Debug C flags" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g" CACHE STRING "Default C flags" FORCE)
set(CMAKE_C_FLAGS_RELEASE "-O2" CACHE STRING "Release C flags" FORCE)

# Without --build-id=none a -no-pie link fails on the orphan .note.gnu.build-id.
set(CMAKE_EXE_LINKER_FLAGS "-no-pie -Wl,--build-id=none" CACHE STRING "Linker flags" FORCE)
