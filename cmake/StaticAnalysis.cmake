# Static analysis with the analyzer that each compiler ships.
#
#   TB_ENABLE_STATIC_ANALYSIS  Analyze every project-owned target as it is
#                              built. A finding fails the build, as a warning
#                              does.
#
# MSVC and GCC analyze during an ordinary compilation, so the option adds a
# flag. Clang's analyzer is a separate action that produces no object file:
# for Clang the option exports the compilation database, and the `tb_analyze`
# target runs the analyzer over every source in it with
# tools/analyze_with_clang.py.
#
# Analysis is for a build of its own. It slows compilation several times over
# and changes no generated code, so nothing is gained by turning it on in a
# build that is also tested or shipped.

function(tb_analyze_target target)
    if(NOT TB_ENABLE_STATIC_ANALYSIS)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C>:/analyze>")
    elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE -fanalyzer)
    endif()
endfunction()

if(TB_ENABLE_STATIC_ANALYSIS AND CMAKE_C_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(tb_analyze
        COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/analyze_with_clang.py"
            "${PROJECT_BINARY_DIR}/compile_commands.json"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Running the Clang static analyzer over every project source"
        VERBATIM)
endif()
