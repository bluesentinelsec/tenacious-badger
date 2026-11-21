# Settings: the options players and developers choose, with their defaults,
# their files, and the engine's catalog of them.
set(TB_SETTINGS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/engine_settings.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/settings.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/settings_file.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/settings_value.c"
    CACHE INTERNAL "Settings sources")
target_sources(badger PRIVATE ${TB_SETTINGS_SOURCES})
target_sources(badger_static PRIVATE ${TB_SETTINGS_SOURCES})
