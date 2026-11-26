add_library(badger SHARED)
add_library(TenaciousBadger::badger ALIAS badger)
add_library(badger_static STATIC)
add_library(TenaciousBadger::badger_static ALIAS badger_static)
foreach(target badger badger_static)
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")
    tb_configure_target(${target})
    tb_analyze_target(${target})
endforeach()
# The C library's math functions are in a library of their own on Linux, and
# badger/math.h calls them from its inline functions, so its users link it too.
include(CheckLibraryExists)
# Programs built from the engine's sources, in place of linking a library,
# link TB_MATH_LIBRARIES themselves.
check_library_exists(m sqrtf "" TB_HAVE_LIBM)
if(TB_HAVE_LIBM)
    set(TB_MATH_LIBRARIES m CACHE INTERNAL "The C library's math functions, where separate")
else()
    set(TB_MATH_LIBRARIES "" CACHE INTERNAL "The C library's math functions, where separate")
endif()
target_link_libraries(badger PUBLIC ${TB_MATH_LIBRARIES})
target_link_libraries(badger_static PUBLIC ${TB_MATH_LIBRARIES})
target_compile_definitions(badger PRIVATE TB_BUILDING_BADGER)
target_compile_definitions(badger_static PUBLIC TB_STATIC)
set_target_properties(badger PROPERTIES
    C_VISIBILITY_PRESET hidden
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}")
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(jobs)
add_subdirectory(anim)
add_subdirectory(codec)
add_subdirectory(serialize)
add_subdirectory(font)
add_subdirectory(vfs)
add_subdirectory(shader)
add_subdirectory(rhi)
add_subdirectory(render)
add_subdirectory(ui)
add_subdirectory(gui)
add_subdirectory(settings)
add_subdirectory(input)
add_subdirectory(game)
tb_strip_release(badger)

# The engine's modules call one another across source files, and a compiler
# that sees one file at a time can neither inline those calls nor shape a
# function to its callers. Release builds of the shared library are therefore
# optimized as a whole when they are linked, wherever the toolchain can do it.
# Nothing selects this and nothing else changes: a toolchain that cannot builds
# as before. The static library is left alone, because an archive of objects
# meant for link-time optimization can only be linked by the same version of
# the same compiler, and the SDK ships that archive to others.
include(CheckIPOSupported)
check_ipo_supported(RESULT tb_ipo_supported OUTPUT tb_ipo_output LANGUAGES C)
if(tb_ipo_supported)
    set_target_properties(badger PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    message(STATUS "badger: Release is not optimized at link time; the toolchain cannot")
endif()

if(TB_BUILD_BURROW)
    # Burrow's default settings travel in the executable; the file is edited
    # as text, and the build turns it into a C source.
    set(burrow_defaults_source "${CMAKE_CURRENT_BINARY_DIR}/burrow_defaults.c")
    add_custom_command(OUTPUT "${burrow_defaults_source}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/burrow_defaults.cfg"
            "-DOUTPUT=${burrow_defaults_source}" -DNAME=burrow_defaults
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/burrow_defaults.cfg"
            "${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake"
        COMMENT "Embedding Burrow's default settings"
        VERBATIM)
    # The game's logic and data, which its tests build too: the room, the
    # world grid's texture, the player, the HUD, and the menus.
    set(TB_BURROW_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/burrow_grid.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/burrow_hud.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/burrow_menu.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/burrow_menus.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/burrow_player.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/burrow_room.c"
        CACHE INTERNAL "Burrow's sources that its tests build")
    # Burrow's content, resources/burrow/Content: the menus' layout files and
    # their theme, read at run time through a VFS. The build copies it
    # beside the executable, or into a macOS bundle's Resources, where the
    # game and its exports find it, as they find the shader libraries.
    set(TB_BURROW_CONTENT "${PROJECT_SOURCE_DIR}/resources/burrow"
        CACHE INTERNAL "Burrow's content, under Content/")
    file(GLOB_RECURSE burrow_content_files CONFIGURE_DEPENDS
        "${TB_BURROW_CONTENT}/Content/*")
    if(APPLE)
        set(burrow_content_destination "$<TARGET_BUNDLE_CONTENT_DIR:burrow>/Resources/Content")
    else()
        set(burrow_content_destination "$<TARGET_FILE_DIR:burrow>/Content")
    endif()
    set(burrow_content_stamp "${CMAKE_CURRENT_BINARY_DIR}/burrow_content.stamp")
    add_custom_command(OUTPUT "${burrow_content_stamp}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${TB_BURROW_CONTENT}/Content"
            "${burrow_content_destination}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${burrow_content_stamp}"
        DEPENDS ${burrow_content_files}
        COMMENT "Copying Burrow's content"
        VERBATIM)
    add_custom_target(burrow_content ALL DEPENDS "${burrow_content_stamp}")
    add_executable(burrow main.c burrow_game.c burrow_scene.c burrow_settings.c
        ${TB_BURROW_SOURCES} "${burrow_defaults_source}")
    target_link_libraries(burrow PRIVATE TenaciousBadger::badger)
    # A developer build starts in a window and quits on Escape; Release and
    # MinSizeRel are release builds.
    target_compile_definitions(burrow PRIVATE
        "BURROW_DEVELOPER=$<IF:$<CONFIG:Release,MinSizeRel>,0,1>")
    # A developer build reads its content from the source tree before the
    # copy, so that the menus' files load again as an author saves them; a
    # release build knows no source tree.
    target_compile_definitions(burrow PRIVATE
        "$<$<NOT:$<CONFIG:Release,MinSizeRel>>:BURROW_SOURCE_CONTENT=\"${TB_BURROW_CONTENT}\">")
    add_dependencies(burrow burrow_content)
    tb_configure_target(burrow)
    tb_analyze_target(burrow)
    tb_strip_release(burrow)
endif()

include("${PROJECT_SOURCE_DIR}/cmake/DesktopExports.cmake")
