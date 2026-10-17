# Writes build_info.h from the release tags (0.1.2, 0.1.2-41-g653bf8a-dirty) and the build day,
# touched only on change. SOURCE_DATE_EPOCH, when set, is the build time.

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

string(TIMESTAMP date "%Y-%m-%d" UTC)
# The day, not the second: a rebuild later the same day changes nothing.
string(TIMESTAMP now "%s" UTC)
math(EXPR day_start "${now} / 86400 * 86400")

file(WRITE ${OUT}.tmp
    "#define PERSPICUA_VERSION     \"${version}\"\n"
    "#define PERSPICUA_BUILD_DATE  \"${date}\"\n"
    "#define PERSPICUA_BUILD_EPOCH ${day_start}LL\n")
configure_file(${OUT}.tmp ${OUT} COPYONLY)
