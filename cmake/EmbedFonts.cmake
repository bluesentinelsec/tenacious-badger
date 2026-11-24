# Builds the fonts of resources/fonts/ into the engine library, compressed.
#
# Each .ttf file the build keeps is compressed by badger-fontpack, a small
# program built from the engine's own DEFLATE (tools/fontpack/), into a C
# source of its own; the sources compile in parallel, and a font is compressed
# again only when its file changes. An index names every file of the pack, in
# the order of their names, with its data or none; src/font/font_embedded.c
# describes the same files by hand, and the tests hold the two together.
#
# The data compiles once, into the object library badger_font_data, whose
# objects the shared and static libraries, and the test programs built from
# the engine's sources, all take. TB_FONT_DATA_OBJECTS names them.
#
# TB_EMBED_FONTS chooses the files:
#   defaults  the default: the families the default roles name
#             (TB_FONT_DEFAULT_FAMILIES), every weight and slant of each:
#             about 2.5 MB of compressed data.
#   all       every font of the pack, an opt-in: about 7 MB.
#   none      no font; the catalog is empty until a game adds fonts.
#
# badger-fontpack runs on the machine that builds. Cross-compiling, it runs
# through CMAKE_CROSSCOMPILING_EMULATOR when there is one; otherwise build it
# for the build machine first (a native build of the target badger-fontpack)
# and name it with TB_FONT_PACKER, or configure TB_EMBED_FONTS=none.

set(TB_FONT_DIRECTORY "${PROJECT_SOURCE_DIR}/resources/fonts")

# The files of the families that the default roles name, in the role table of
# src/font/font_catalog.c. Change the two together; the tests check that
# every role finds its own font in a build with TB_EMBED_FONTS=defaults.
set(TB_FONT_DEFAULT_FAMILIES
    "serif/SourceSerif4-*.ttf"
    "sans/SourceSans3-*.ttf"
    "mono/JetBrainsMono-*.ttf"
    "retro/Px437_IBM_VGA_8x16.ttf")

set(TB_FONT_PACKER "" CACHE FILEPATH
    "A badger-fontpack built for the build machine, for cross-compiling; empty to build one")

function(tb_font_packer output)
    if(TB_FONT_PACKER)
        set(${output} "${TB_FONT_PACKER}" PARENT_SCOPE)
        return()
    endif()
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        message(FATAL_ERROR
            "Cross-compiling, the build cannot run badger-fontpack, which compresses the "
            "embedded fonts. Build the target badger-fontpack natively and pass its path "
            "as -DTB_FONT_PACKER=<path>, or configure -DTB_EMBED_FONTS=none.")
    endif()
    add_executable(badger-fontpack
        "${PROJECT_SOURCE_DIR}/tools/fontpack/fontpack.c"
        "${PROJECT_SOURCE_DIR}/tools/fontpack/fontpack_support.c"
        "${PROJECT_SOURCE_DIR}/src/codec/checksum.c"
        "${PROJECT_SOURCE_DIR}/src/codec/deflate.c"
        "${PROJECT_SOURCE_DIR}/src/codec/inflate.c")
    target_include_directories(badger-fontpack PRIVATE
        "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/generated"
        "${PROJECT_SOURCE_DIR}/src/codec")
    target_compile_definitions(badger-fontpack PRIVATE TB_STATIC)
    tb_configure_build_tool(badger-fontpack)
    set(${output} badger-fontpack PARENT_SCOPE)
endfunction()

# Whether a file of the pack, named relative to it, is one this build keeps.
function(tb_font_is_kept file output)
    set(kept FALSE)
    if(TB_EMBED_FONTS STREQUAL "all")
        set(kept TRUE)
    elseif(TB_EMBED_FONTS STREQUAL "defaults")
        foreach(pattern IN LISTS TB_FONT_DEFAULT_FAMILIES)
            string(REPLACE "." "\\." expression "${pattern}")
            string(REPLACE "*" ".*" expression "${expression}")
            if(file MATCHES "^${expression}$")
                set(kept TRUE)
            endif()
        endforeach()
    endif()
    set(${output} ${kept} PARENT_SCOPE)
endfunction()

function(tb_embed_fonts)
    set(choices all defaults none)
    if(NOT TB_EMBED_FONTS IN_LIST choices)
        message(FATAL_ERROR "TB_EMBED_FONTS is '${TB_EMBED_FONTS}': choose all, defaults, or none")
    endif()

    file(GLOB fonts RELATIVE "${TB_FONT_DIRECTORY}" CONFIGURE_DEPENDS "${TB_FONT_DIRECTORY}/*/*.ttf")
    list(SORT fonts)
    set(generated_directory "${PROJECT_BINARY_DIR}/generated/fonts")
    file(MAKE_DIRECTORY "${generated_directory}")

    set(packer "")
    set(sources "")
    set(declarations "")
    set(entries "")
    set(kept_count 0)
    set(index 0)
    foreach(font IN LISTS fonts)
        if(font MATCHES "[\"\\\\]")
            message(FATAL_ERROR "EmbedFonts.cmake: ${font}: a name cannot hold \" or \\")
        endif()
        tb_font_is_kept("${font}" kept)
        if(kept)
            if(NOT packer)
                tb_font_packer(packer)
            endif()
            set(symbol "tb_font_embedded_blob_${index}")
            set(source "${generated_directory}/font_${index}.c")
            add_custom_command(OUTPUT "${source}"
                COMMAND ${packer} "${TB_FONT_DIRECTORY}/${font}" "${source}" ${symbol}
                DEPENDS "${TB_FONT_DIRECTORY}/${font}" ${packer}
                COMMENT "Compressing the font ${font}"
                VERBATIM)
            list(APPEND sources "${source}")
            string(APPEND declarations "extern const tb_font_blob ${symbol};\n")
            string(APPEND entries "    {\"${font}\", &${symbol}},\n")
            math(EXPR kept_count "${kept_count} + 1")
        else()
            string(APPEND entries "    {\"${font}\", NULL},\n")
        endif()
        math(EXPR index "${index} + 1")
    endforeach()
    if(index EQUAL 0)
        message(FATAL_ERROR "EmbedFonts.cmake: no .ttf file in ${TB_FONT_DIRECTORY}/*/")
    endif()

    # Written beside the index under a name of its own, then copied over it
    # only if it changed, so the build does not see a change that is not there.
    set(index_source "${generated_directory}/font_index.c")
    string(RANDOM LENGTH 12 suffix)
    set(temporary "${index_source}.${suffix}.tmp")
    file(WRITE "${temporary}"
        "/* Generated by cmake/EmbedFonts.cmake with TB_EMBED_FONTS=${TB_EMBED_FONTS}. Do not edit. */\n"
        "#include \"font_embedded_data.h\"\n\n"
        "${declarations}\n"
        "const tb_font_index_entry tb_font_embedded_index[] = {\n${entries}};\n\n"
        "const size_t tb_font_embedded_index_count = ${index};\n")
    file(COPY_FILE "${temporary}" "${index_source}" ONLY_IF_DIFFERENT)
    file(REMOVE "${temporary}")

    add_library(badger_font_data OBJECT "${index_source}" ${sources})
    target_include_directories(badger_font_data PRIVATE "${PROJECT_SOURCE_DIR}/src/font")
    # Data only: compiled as the libraries' own sources are, and hidden in
    # the shared library, but neither analyzed nor counted for coverage.
    tb_configure_compiler(badger_font_data)
    set_target_properties(badger_font_data PROPERTIES
        POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
    set(TB_FONT_DATA_OBJECTS "$<TARGET_OBJECTS:badger_font_data>"
        CACHE INTERNAL "The objects of the embedded fonts' data")
    message(STATUS "badger: embedding ${kept_count} of ${index} fonts (TB_EMBED_FONTS=${TB_EMBED_FONTS})")
endfunction()
