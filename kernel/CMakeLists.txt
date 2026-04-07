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

find_package(Python3 REQUIRED)

# First pass: build kernel without symbol table
add_executable(kernel8_pass1.elf ${KERNEL_SRCS} ${KERNEL_ASMS} ${CMAKE_SOURCE_DIR}/scripts/dummy_symtab.c)

target_include_directories(kernel8_pass1.elf PRIVATE
    include
    tests
    ../libc/include
    ..
)

target_compile_definitions(kernel8_pass1.elf PRIVATE __KERNEL__ ${KERNEL_CONFIG_DEFS})
target_link_libraries(kernel8_pass1.elf PRIVATE libk)

target_link_options(kernel8_pass1.elf PRIVATE
    -nostdlib
    -T ${CMAKE_CURRENT_SOURCE_DIR}/kernel.ld
)

# Generate symbol table from pass 1
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/symbol_table.c
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_symtab.py ${CMAKE_CURRENT_BINARY_DIR}/kernel8_pass1.elf ${CMAKE_CURRENT_BINARY_DIR}/symbol_table.c
    DEPENDS kernel8_pass1.elf ${CMAKE_SOURCE_DIR}/scripts/gen_symtab.py
    COMMENT "Generating kernel symbol table"
)

# Second pass: build kernel with real symbol table
add_executable(kernel8.elf ${KERNEL_SRCS} ${KERNEL_ASMS} ${CMAKE_CURRENT_BINARY_DIR}/symbol_table.c)

target_include_directories(kernel8.elf PRIVATE
    include
    tests
    ../libc/include
    ..
)

target_compile_definitions(kernel8.elf PRIVATE __KERNEL__ ${KERNEL_CONFIG_DEFS})
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
