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
target_compile_definitions(badger PRIVATE TB_BUILDING_BADGER)
target_compile_definitions(badger_static PUBLIC TB_STATIC)
set_target_properties(badger PROPERTIES
    C_VISIBILITY_PRESET hidden
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}")
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(jobs)
add_subdirectory(codec)
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
    add_executable(burrow main.c)
    target_link_libraries(burrow PRIVATE TenaciousBadger::badger)
    tb_configure_target(burrow)
    tb_analyze_target(burrow)
    tb_strip_release(burrow)
endif()

include("${PROJECT_SOURCE_DIR}/cmake/DesktopExports.cmake")
