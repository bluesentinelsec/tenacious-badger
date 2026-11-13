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

# The formatted files are listed explicitly, like every build input.
include("${CMAKE_CURRENT_LIST_DIR}/FormatSources.cmake")
set(sources "")
foreach(source IN LISTS TB_FORMAT_SOURCES)
    if(NOT EXISTS "${root}/${source}")
        message(FATAL_ERROR
            "cmake/FormatSources.cmake lists ${source}, which does not exist; remove the entry")
    endif()
    list(APPEND sources "${root}/${source}")
endforeach()

# An explicit list can silently fall behind. The check therefore asks git which
# C files the repository tracks and fails for any that the list leaves out.
find_package(Git QUIET)
if(TB_FORMAT_MODE STREQUAL "check" AND Git_FOUND AND EXISTS "${root}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" ls-files -- "*.c" "*.h" "*.h.in"
        WORKING_DIRECTORY "${root}" OUTPUT_VARIABLE tracked
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    string(REPLACE "\n" ";" tracked "${tracked}")
    foreach(source IN LISTS tracked)
        if(NOT source IN_LIST TB_FORMAT_SOURCES)
            message(FATAL_ERROR
                "${source} is tracked but not formatted; add it to cmake/FormatSources.cmake")
        endif()
    endforeach()
endif()

execute_process(COMMAND "${TB_CLANG_FORMAT}" ${arguments} ${sources}
    WORKING_DIRECTORY "${root}" COMMAND_ERROR_IS_FATAL ANY)
