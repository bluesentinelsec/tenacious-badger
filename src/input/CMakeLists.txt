# Input: actions, mapping contexts, modifiers and triggers after Unreal's
# Enhanced Input, evaluated once a tick, and read from and written to JSON.
set(TB_INPUT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/input.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/input_file.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/input_keys.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/input_rules.c"
    CACHE INTERNAL "Input sources")
target_sources(badger PRIVATE ${TB_INPUT_SOURCES})
target_sources(badger_static PRIVATE ${TB_INPUT_SOURCES})
