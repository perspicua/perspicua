file(GLOB_RECURSE KERNEL_SRCS CONFIGURE_DEPENDS
    "mm/*.c"
    "fs/*.c"
    "sched/*.c"
    "core/*.c"
    "arch/*.c"
    "driver/*.c"
    "devicetree/*.c"
    "init/*.c"
    "tests/*.c"
)

file(GLOB_RECURSE KERNEL_ASMS CONFIGURE_DEPENDS
    "core/*.S"
    "arch/*.S"
)

add_executable(kernel8.elf ${KERNEL_SRCS} ${KERNEL_ASMS})

target_include_directories(kernel8.elf PRIVATE
    include
    tests
    ../libc/include
    ..
)

target_compile_definitions(kernel8.elf PRIVATE __KERNEL__)
target_link_libraries(kernel8.elf PRIVATE libk)

target_link_options(kernel8.elf PRIVATE
    -nostdlib
    -T ${CMAKE_CURRENT_SOURCE_DIR}/kernel.ld
)

# Post-build: Create binary image
add_custom_command(TARGET kernel8.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary kernel8.elf kernel8.img
    COMMAND ${CMAKE_COMMAND} -E copy kernel8.img ${CMAKE_SOURCE_DIR}/pi4-boot/kernel8.img
    COMMENT "Generating kernel8.img binary and copying to pi4-boot"
)
