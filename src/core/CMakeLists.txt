set(TB_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/allocator.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/arena.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/array.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/assert.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/atom.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/bit.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/bytes.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fmt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/hash.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/list.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ring.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/size.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/str.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sum.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/table.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/text.c"
    CACHE INTERNAL "Platform-independent core sources")
target_sources(badger PRIVATE ${TB_CORE_SOURCES})
target_sources(badger_static PRIVATE ${TB_CORE_SOURCES})
