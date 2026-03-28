cmake_minimum_required(VERSION 3.16)

project(perspicua C ASM)

# Toolchain check
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
    message(FATAL_ERROR "Please use the toolchain file: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-none-elf.cmake ..")
endif()

# Global Options
option(USE_COLOR "Enable colored output" ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# SD Card Image target
add_custom_target(sdcard
    COMMAND dd if=/dev/zero of=${SD_IMAGE} bs=1M count=32 status=none
    COMMAND ${MFORMAT} -i "${SD_IMAGE}" -F ::
    COMMAND ${MMD} -i "${SD_IMAGE}" ::/bin
    COMMAND ${MCOPY} -i "${SD_IMAGE}" ${USER_ELFS} ::/bin/
    COMMAND ${MCOPY} -i "${SD_IMAGE}" "${CMAKE_SOURCE_DIR}/README.txt" ::/
    DEPENDS initrd
    VERBATIM
    COMMENT "Generating FAT32 SD card image"
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
add_custom_target(gdb
    COMMAND ${CROSS_COMPILE}gdb -ex "target remote :1234" -ex "layout split" ${KERNEL_ELF}
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
