# Serialization: JSON now, binary formats later.
set(TB_SERIALIZE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/json_document.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/json_reader.c"
    CACHE INTERNAL "Serialization sources")
target_sources(badger PRIVATE ${TB_SERIALIZE_SOURCES})
target_sources(badger_static PRIVATE ${TB_SERIALIZE_SOURCES})
