cmake_minimum_required(VERSION 3.16)

project(perspicua C ASM)

# Toolchain check
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
    message(FATAL_ERROR "Please use the toolchain file: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-none-elf.cmake ..")
endif()

# Global Options
option(USE_COLOR "Enable colored output" ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Kernel Configuration (Kconfig-lite)
option(CONFIG_SMP      "Enable symmetric multiprocessing (multi-core)" ON)
option(CONFIG_LOCKDEP  "Enable lock dependency validator (debug)" ON)
option(CONFIG_TESTS    "Build in-kernel test suites and run them at boot" OFF)
set(CONFIG_NR_CPUS 4 CACHE STRING "Number of CPU cores (1-4)")
set(CONFIG_MAX_PROCESSES 256 CACHE STRING "Process table slots (one pointer each; PCBs allocated on demand)")
set(CONFIG_MAX_FDS 64 CACHE STRING "Open file descriptors per process")

# Propagate config to compiler defines
set(KERNEL_CONFIG_DEFS "")
if(CONFIG_SMP)
    list(APPEND KERNEL_CONFIG_DEFS CONFIG_SMP)
endif()
if(CONFIG_LOCKDEP)
    list(APPEND KERNEL_CONFIG_DEFS CONFIG_LOCKDEP)
endif()
if(CONFIG_TESTS)
    list(APPEND KERNEL_CONFIG_DEFS CONFIG_TESTS)
endif()
list(APPEND KERNEL_CONFIG_DEFS "CONFIG_NR_CPUS=${CONFIG_NR_CPUS}")
list(APPEND KERNEL_CONFIG_DEFS "CONFIG_MAX_PROCESSES=${CONFIG_MAX_PROCESSES}")
list(APPEND KERNEL_CONFIG_DEFS "CONFIG_MAX_FDS=${CONFIG_MAX_FDS}")

# Build Type Logic
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build: Debug Release" FORCE)
endif()

# Dependency Checks (Configuration Phase)
find_program(MFORMAT mformat)
find_program(MCOPY mcopy)
find_program(MMD mmd)
find_program(CPIO cpio)
find_program(QEMU qemu-system-aarch64)

if(NOT MFORMAT OR NOT MCOPY OR NOT MMD)
    message(FATAL_ERROR "mtools (mformat, mcopy, mmd) not found. Required for SD image generation.")
endif()

if(NOT CPIO)
    message(FATAL_ERROR "cpio not found. Required for initrd generation.")
endif()

if(NOT QEMU)
    message(WARNING "qemu-system-aarch64 not found. 'run' targets will fail.")
endif()


# Subdirectories
add_subdirectory(libc)
add_subdirectory(kernel)
add_subdirectory(user)

# Helper for QEMU targets
set(DTB "${CMAKE_SOURCE_DIR}/pi4-boot/bcm2711-rpi-4-b.dtb")
set(KERNEL_ELF "${CMAKE_BINARY_DIR}/kernel/kernel8.elf")
set(KERNEL_IMG "${CMAKE_BINARY_DIR}/kernel/kernel8.img")
set(SD_IMAGE "${CMAKE_BINARY_DIR}/sdcard.img")

set(QEMU_FLAGS -M raspi4b -serial stdio -display none -dtb ${DTB} -kernel ${KERNEL_IMG})
set(QEMU_FLAGS_GUI -M raspi4b -serial vc -dtb ${DTB} -kernel ${KERNEL_IMG})
set(QEMU_SD_FLAGS -drive file=${SD_IMAGE},format=raw,if=sd)

# SD Card Image: create once, then incrementally update only changed files
add_custom_command(
    OUTPUT ${SD_IMAGE}
    COMMAND dd if=/dev/zero of=${SD_IMAGE} bs=1M count=32 status=none
    COMMAND ${MFORMAT} -i "${SD_IMAGE}" -F ::
    COMMAND ${MMD} -i "${SD_IMAGE}" ::/bin
    COMMENT "Creating FAT32 SD card image"
    VERBATIM
)

add_custom_target(sdcard
    COMMAND ${MCOPY} -o -i "${SD_IMAGE}" ${USER_ELFS} ::/bin/
    COMMAND ${MCOPY} -o -i "${SD_IMAGE}" "${CMAKE_SOURCE_DIR}/README.txt" ::/
    DEPENDS ${SD_IMAGE} initrd
    VERBATIM
    COMMENT "Updating SD card image (incremental)"
)

# QEMU Run target
add_custom_target(run
    COMMAND ${QEMU} ${QEMU_FLAGS} ${QEMU_SD_FLAGS}
    DEPENDS kernel8.elf sdcard
    USES_TERMINAL
)

add_custom_target(run-gui
    COMMAND ${QEMU} ${QEMU_FLAGS_GUI} ${QEMU_SD_FLAGS}
    DEPENDS kernel8.elf sdcard
    USES_TERMINAL
)

add_custom_target(debug
    COMMAND ${QEMU} ${QEMU_FLAGS} ${QEMU_SD_FLAGS} -s -S
    DEPENDS kernel8.elf sdcard
    USES_TERMINAL
)

# Provide the GDB command directly as a target for convenience (run debug target in another terminal)
set(GDB_HELPERS "${CMAKE_SOURCE_DIR}/scripts/gdb_helpers.py")
add_custom_target(gdb
    COMMAND ${CROSS_COMPILE}gdb
        -ex "source ${GDB_HELPERS}"
        -ex "target remote :1234"
        -ex "layout split"
        ${KERNEL_ELF}
    DEPENDS kernel8.elf
    USES_TERMINAL
)

add_custom_target(disasm
    COMMAND ${CMAKE_OBJDUMP} -d -S ${KERNEL_ELF}
    DEPENDS kernel8.elf
    USES_TERMINAL
)

# Analysis targets
add_custom_target(size
    COMMAND ${CMAKE_SIZE} ${KERNEL_ELF}
    DEPENDS kernel8.elf
)

add_custom_target(symbols
    COMMAND ${CMAKE_NM} -n ${KERNEL_ELF}
    DEPENDS kernel8.elf
)
