# Writes build_info.h from the release tags (0.1.2, 0.1.2-41-g653bf8a-dirty), touched only on change.

execute_process(
    COMMAND git -C ${SRC} describe --tags --match "v[0-9]*" --always --dirty
    OUTPUT_VARIABLE version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR version STREQUAL "")
    set(version "unknown")
endif()
string(REGEX REPLACE "^v" "" version "${version}")

string(TIMESTAMP date "%Y-%m-%d")

file(WRITE ${OUT}.tmp
    "#define PERSPICUA_VERSION    \"${version}\"\n"
    "#define PERSPICUA_BUILD_DATE \"${date}\"\n")
configure_file(${OUT}.tmp ${OUT} COPYONLY)
