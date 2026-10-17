file(GLOB_RECURSE KERNEL_SRCS CONFIGURE_DEPENDS
    "mm/*.c"
    "fs/*.c"
    "sched/*.c"
    "core/*.c"
    "arch/*.c"
    "driver/*.c"
    "devicetree/*.c"
    "init/*.c"
)

# Test suites are compiled in only under CONFIG_TESTS, so a shipping kernel
# carries no test code at all rather than merely not calling it.
if(CONFIG_TESTS)
    file(GLOB_RECURSE KERNEL_TEST_SRCS CONFIGURE_DEPENDS "tests/*.c")
    list(APPEND KERNEL_SRCS ${KERNEL_TEST_SRCS})
endif()

file(GLOB_RECURSE KERNEL_ASMS CONFIGURE_DEPENDS
    "core/*.S"
    "arch/*.S"
)
# CMake's ASM dependency scanner cannot see the -include'd config.h.
set_source_files_properties(${KERNEL_ASMS} PROPERTIES OBJECT_DEPENDS ${KERNEL_CONFIG_INCLUDE}/config.h)

# The linker script needs CONFIG_NR_CPUS for the boot stacks, and -T does not
# run it through the preprocessor.
configure_file(kernel.ld.in ${CMAKE_CURRENT_BINARY_DIR}/kernel.ld @ONLY)

find_package(Python3 REQUIRED)

# The kernel is linked twice: once to discover the symbol addresses, then again
# with a table of them compiled in. Only that one generated file differs
# between the passes, and it holds data only, so the sources are compiled once
# here and both links lay out text identically.
add_library(kernel_objs OBJECT ${KERNEL_SRCS} ${KERNEL_ASMS})

target_include_directories(kernel_objs PUBLIC
    include
    tests
    ../libc/include
    ..
    ${KERNEL_CONFIG_INCLUDE}
)

target_compile_definitions(kernel_objs PUBLIC __KERNEL__)

# /proc/version names the commit a kernel was built from; refreshed on every build.
add_custom_target(build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${KERNEL_CONFIG_INCLUDE}/build_info.h
            -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${KERNEL_CONFIG_INCLUDE}/build_info.h
)
add_dependencies(kernel_objs build_info)
target_compile_options(kernel_objs PUBLIC ${KERNEL_CONFIG_FORCE_INCLUDE})
target_link_libraries(kernel_objs PUBLIC libk)

# First pass: a stand-in table, because the real one needs these addresses.
add_executable(kernel8_pass1.elf ${CMAKE_SOURCE_DIR}/scripts/dummy_symtab.c)
target_link_libraries(kernel8_pass1.elf PRIVATE kernel_objs libk)
target_link_options(kernel8_pass1.elf PRIVATE
    -nostdlib
    -T ${CMAKE_CURRENT_BINARY_DIR}/kernel.ld
)

set(GEN_SYMTAB ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_symtab.py)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/symbol_table.c
    COMMAND ${GEN_SYMTAB} table ${CMAKE_NM} ${CMAKE_CURRENT_BINARY_DIR}/kernel8_pass1.elf ${CMAKE_CURRENT_BINARY_DIR}/symbol_table.c
    DEPENDS kernel8_pass1.elf ${CMAKE_SOURCE_DIR}/scripts/gen_symtab.py
    COMMENT "Generating kernel symbol table"
)

# Second pass: the same objects, now with the real table.
add_executable(kernel8.elf ${CMAKE_CURRENT_BINARY_DIR}/symbol_table.c)
target_link_libraries(kernel8.elf PRIVATE kernel_objs libk)
target_link_options(kernel8.elf PRIVATE
    -nostdlib
    -T ${CMAKE_CURRENT_BINARY_DIR}/kernel.ld
    -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/kernel8.map
)

# -T is invisible to CMake's dependency tracking.
set_target_properties(kernel8_pass1.elf kernel8.elf PROPERTIES
    LINK_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/kernel.ld
)

# The table holds pass-1 addresses, so the image ships only if pass 2 matches them.
add_custom_command(TARGET kernel8.elf POST_BUILD
    COMMAND ${GEN_SYMTAB} check ${CMAKE_NM} ${CMAKE_CURRENT_BINARY_DIR}/kernel8_pass1.elf ${CMAKE_CURRENT_BINARY_DIR}/kernel8.elf
    COMMENT "Checking the symbol table against the final link"
)

# Post-build: Create binary image
add_custom_command(TARGET kernel8.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary kernel8.elf kernel8.img
    COMMENT "Generating kernel8.img binary"
)

# pi4-boot is what gets flashed to real hardware: `just build` copies the image
# there, so no other tree, whatever its configuration, can replace it.
