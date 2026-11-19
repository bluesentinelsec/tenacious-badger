# Checks how tb_add_package_key makes and keeps a key (cmake/BadgerPackageKey.cmake):
# a missing key file gets a new key of 64 hexadecimal digits, which is then
# kept; another file gets another key; a malformed file, both kinds of file,
# or neither, are refused; and the same key always makes the same source.
#
#   TB_MODULE     cmake/BadgerPackageKey.cmake.
#   TB_DIRECTORY  A directory the test may write in.

include("${TB_MODULE}")
file(REMOVE_RECURSE "${TB_DIRECTORY}")
file(MAKE_DIRECTORY "${TB_DIRECTORY}")

_tb_package_key_resolve(first "${TB_DIRECTORY}/first.key" "")
if(NOT first MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "A new key is not hexadecimal: ${first}")
endif()
string(LENGTH "${first}" length)
if(NOT length EQUAL 64)
    message(FATAL_ERROR "A new key has ${length} digits, not 64")
endif()
_tb_package_key_resolve(again "${TB_DIRECTORY}/first.key" "")
if(NOT again STREQUAL first)
    message(FATAL_ERROR "A key file was not kept: ${first}, then ${again}")
endif()
_tb_package_key_resolve(second "${TB_DIRECTORY}/second.key" "")
if(second STREQUAL first)
    message(FATAL_ERROR "Two new keys are the same: ${first}")
endif()

_tb_package_key_write_source("${first}" "tb_generated_key" "${TB_DIRECTORY}/one")
_tb_package_key_write_source("${first}" "tb_generated_key" "${TB_DIRECTORY}/two")
file(READ "${TB_DIRECTORY}/one/tb_generated_key.c" one)
file(READ "${TB_DIRECTORY}/two/tb_generated_key.c" two)
if(NOT one STREQUAL two)
    message(FATAL_ERROR "The same key made different sources")
endif()
string(FIND "${one}" "${first}" found)
if(NOT found EQUAL -1)
    message(FATAL_ERROR "The source holds the key's digits")
endif()

# What is refused: each case runs in a child, since a refusal ends CMake.
file(WRITE "${TB_DIRECTORY}/short.key" "abc\n")
file(WRITE "${TB_DIRECTORY}/password" "hunter2\n")
set(refusals
    "_tb_package_key_resolve(key \"${TB_DIRECTORY}/short.key\" \"\")"
    "_tb_package_key_resolve(key \"${TB_DIRECTORY}/first.key\" \"${TB_DIRECTORY}/password\")"
    "_tb_package_key_resolve(key \"\" \"\")"
    "_tb_package_key_resolve(key \"\" \"${TB_DIRECTORY}/missing\")"
    "_tb_package_key_write_source(\"${first}\" \"not-an-identifier\" \"${TB_DIRECTORY}/three\")")
set(index 0)
foreach(refusal IN LISTS refusals)
    file(WRITE "${TB_DIRECTORY}/refusal${index}.cmake" "include(\"${TB_MODULE}\")\n${refusal}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${TB_DIRECTORY}/refusal${index}.cmake"
        RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE error)
    if(status EQUAL 0 OR NOT error MATCHES "tb_add_package_key")
        message(FATAL_ERROR "Not refused: ${refusal}\n${error}")
    endif()
    math(EXPR index "${index} + 1")
endforeach()
message(STATUS "Keys are made, kept, and refused as they should be.")
