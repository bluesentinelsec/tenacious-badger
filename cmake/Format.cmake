cmake_minimum_required(VERSION 3.25)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT TB_CLANG_FORMAT)
    find_program(TB_CLANG_FORMAT NAMES clang-format-22 clang-format REQUIRED)
endif()
if(TB_FORMAT_MODE STREQUAL "write")
    set(arguments -i)
elseif(TB_FORMAT_MODE STREQUAL "check")
    set(arguments --dry-run --Werror)
else()
    message(FATAL_ERROR "Set TB_FORMAT_MODE to write or check")
endif()

# The formatted files are found by searching the project's source directories.
include("${CMAKE_CURRENT_LIST_DIR}/FormatSources.cmake")
if(NOT TB_FORMAT_SOURCES)
    message(FATAL_ERROR "cmake/FormatSources.cmake found no C sources or headers to format")
endif()
list(TRANSFORM TB_FORMAT_SOURCES PREPEND "${root}/" OUTPUT_VARIABLE sources)

# A search of named directories misses C code that is added anywhere else. The
# check therefore asks git which C files the repository tracks and fails for
# any that the search did not find.
find_package(Git QUIET)
if(TB_FORMAT_MODE STREQUAL "check" AND Git_FOUND AND EXISTS "${root}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" ls-files -- ${TB_FORMAT_PATTERNS}
        WORKING_DIRECTORY "${root}" OUTPUT_VARIABLE tracked
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    string(REPLACE "\n" ";" tracked "${tracked}")
    foreach(source IN LISTS tracked)
        # A file deleted from the working tree is still tracked until the
        # deletion is committed.
        if(EXISTS "${root}/${source}" AND NOT source IN_LIST TB_FORMAT_SOURCES)
            message(FATAL_ERROR
                "${source} is tracked but not formatted; add its directory to "
                "TB_FORMAT_DIRECTORIES in cmake/FormatSources.cmake")
        endif()
    endforeach()
endif()

execute_process(COMMAND "${TB_CLANG_FORMAT}" ${arguments} ${sources}
    WORKING_DIRECTORY "${root}" COMMAND_ERROR_IS_FATAL ANY)
