# Line coverage of the engine's sources by its tests.
#
#   TB_ENABLE_COVERAGE  Compile and link every project-owned target so that a
#                       run of the tests records which lines were run.
#
# GCC and Clang instrument the code with `--coverage`. A process writes its
# counts when it exits, and a process that the engine ends through
# tb_platform_abort() never exits in that sense. So the option also defines
# TB_COVERAGE, which makes the abort of each platform adapter write the counts
# first; without that, no line on the way to a fatal error would ever be
# counted, though the tests run every one of them in a child process.
#
# MSVC has no such flag. On Windows the tests are run under
# Microsoft.CodeCoverage.Console, which needs nothing from the build, and the
# option changes nothing.
#
# tools/check_coverage.py reads what either leaves behind.

function(tb_cover_target target)
    if(NOT TB_ENABLE_COVERAGE OR MSVC)
        return()
    endif()
    target_compile_options(${target} PRIVATE --coverage)
    target_link_options(${target} PRIVATE --coverage)
    target_compile_definitions(${target} PRIVATE TB_COVERAGE)
endfunction()
