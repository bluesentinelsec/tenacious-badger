# Finds every C source and header that clang-format maintains and sets
# TB_FORMAT_SOURCES to their paths, sorted and relative to the repository root.
# A new file needs no entry here: it is found by being in one of the
# directories below.
#
# tools/check_allocations.py reads the two settings below to check the same
# files, so keep each one a single set() of plain words.

# Directories that hold project C code, searched with their subdirectories.
# Searching these, and not the whole tree, keeps build directories and other
# generated files out. The format check fails when git tracks a C file that
# they do not cover.
set(TB_FORMAT_DIRECTORIES benchmarks cmake include src tests)

# C sources, headers, and the templates that headers are configured from.
set(TB_FORMAT_PATTERNS *.c *.h *.h.in)

get_filename_component(tb_format_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(tb_format_globs "")
foreach(directory IN LISTS TB_FORMAT_DIRECTORIES)
    foreach(pattern IN LISTS TB_FORMAT_PATTERNS)
        list(APPEND tb_format_globs "${tb_format_root}/${directory}/${pattern}")
    endforeach()
endforeach()
file(GLOB_RECURSE TB_FORMAT_SOURCES LIST_DIRECTORIES false RELATIVE "${tb_format_root}"
    ${tb_format_globs})
list(SORT TB_FORMAT_SOURCES)
