set(TB_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/assert.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sum.c"
    CACHE INTERNAL "Platform-independent core sources")
target_sources(badger PRIVATE ${TB_CORE_SOURCES})
target_sources(badger_static PRIVATE ${TB_CORE_SOURCES})
