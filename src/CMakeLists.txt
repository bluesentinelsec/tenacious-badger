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
endforeach()
target_compile_definitions(badger PRIVATE TB_BUILDING_BADGER)
target_compile_definitions(badger_static PUBLIC TB_STATIC)
set_target_properties(badger PROPERTIES
    C_VISIBILITY_PRESET hidden
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}")
add_subdirectory(core)
add_subdirectory(platform)
tb_strip_release(badger)

if(TB_BUILD_BURROW)
    add_executable(burrow main.c)
    target_link_libraries(burrow PRIVATE TenaciousBadger::badger)
    tb_configure_target(burrow)
    tb_strip_release(burrow)
endif()

include("${PROJECT_SOURCE_DIR}/cmake/DesktopExports.cmake")
