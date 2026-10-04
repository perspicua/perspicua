# Add crt0
add_library(crt0 OBJECT crt0.S)
target_include_directories(crt0 PRIVATE include ../libc/include ..)

# One program per source file in apps/, and one per directory under it for the
# programs too big to stay in a single file.
file(GLOB APP_SRCS CONFIGURE_DEPENDS "apps/*.c")

file(GLOB APP_DIRS CONFIGURE_DEPENDS "apps/*")
foreach(APP_DIR ${APP_DIRS})
    if(IS_DIRECTORY ${APP_DIR})
        file(GLOB DIR_SRCS CONFIGURE_DEPENDS "${APP_DIR}/*.c")
        if(DIR_SRCS)
            list(APPEND APP_SRCS "${APP_DIR}")
        endif()
    endif()
endforeach()

set(USER_ELFS "")

foreach(APP_SRC ${APP_SRCS})
    if(IS_DIRECTORY ${APP_SRC})
        get_filename_component(APP_NAME ${APP_SRC} NAME)
        file(GLOB APP_FILES CONFIGURE_DEPENDS "${APP_SRC}/*.c")
    else()
        get_filename_component(APP_NAME ${APP_SRC} NAME_WE)
        set(APP_FILES ${APP_SRC})
    endif()
    add_executable(${APP_NAME}.elf ${APP_FILES})
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
