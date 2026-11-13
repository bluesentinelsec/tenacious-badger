add_library(badger SHARED)
add_library(TenaciousBadger::badger ALIAS badger)
target_include_directories(badger PUBLIC
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/generated")
target_compile_definitions(badger PRIVATE TB_BUILDING_BADGER)
set_target_properties(badger PROPERTIES
    C_VISIBILITY_PRESET hidden
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}")
tb_configure_target(badger)
add_subdirectory(core)
add_subdirectory(platform)
tb_strip_release(badger)

add_executable(burrow main.c)
target_link_libraries(burrow PRIVATE TenaciousBadger::badger)
tb_configure_target(burrow)
tb_strip_release(burrow)
