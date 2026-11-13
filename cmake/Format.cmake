get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT TB_CLANG_FORMAT)
    find_program(TB_CLANG_FORMAT NAMES clang-format-22 clang-format REQUIRED)
endif()
# Discovery is for formatting only; build translation units are listed explicitly.
file(GLOB_RECURSE sources LIST_DIRECTORIES FALSE
    "${root}/src/*.c" "${root}/src/*.h.in"
    "${root}/include/*.h" "${root}/tests/*.c" "${root}/cmake/*.h.in")
if(TB_FORMAT_MODE STREQUAL "write")
    set(arguments -i)
elseif(TB_FORMAT_MODE STREQUAL "check")
    set(arguments --dry-run --Werror)
else()
    message(FATAL_ERROR "Set TB_FORMAT_MODE to write or check")
endif()
execute_process(COMMAND "${TB_CLANG_FORMAT}" ${arguments} ${sources}
    WORKING_DIRECTORY "${root}" COMMAND_ERROR_IS_FATAL ANY)
