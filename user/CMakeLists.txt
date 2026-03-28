# Add crt0
add_library(crt0 OBJECT crt0.S)
target_include_directories(crt0 PRIVATE include ../libc/include ..)

# Get all apps
file(GLOB APP_SRCS CONFIGURE_DEPENDS "apps/*.c")

set(USER_ELFS "")

foreach(APP_SRC ${APP_SRCS})
    get_filename_component(APP_NAME ${APP_SRC} NAME_WE)
    add_executable(${APP_NAME}.elf ${APP_SRC})
    target_link_libraries(${APP_NAME}.elf PRIVATE libc crt0)
    target_include_directories(${APP_NAME}.elf PRIVATE include ../libc/include ..)
    target_link_options(${APP_NAME}.elf PRIVATE
        -nostdlib
        -T ${CMAKE_CURRENT_SOURCE_DIR}/user.ld
    )
    set(APP_ELF_PATH "${CMAKE_CURRENT_BINARY_DIR}/${APP_NAME}.elf")
    list(APPEND USER_ELFS "${APP_ELF_PATH}")
endforeach()

set(USER_ELFS ${USER_ELFS} PARENT_SCOPE)

# Create initrd.cpio
add_custom_target(initrd
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/root/bin"
    COMMAND ${CMAKE_COMMAND} -E copy ${USER_ELFS} "${CMAKE_BINARY_DIR}/root/bin/"
    COMMAND sh -c "cd '${CMAKE_BINARY_DIR}/root' && find . -maxdepth 2 -not -path '.' | sed 's|^\\./||' | ${CPIO} -o -H newc > ../initrd.cpio 2>/dev/null"
    DEPENDS ${USER_ELFS}
    VERBATIM
    COMMENT "Building initrd.cpio"
)
