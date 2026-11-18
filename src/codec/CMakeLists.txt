# Codecs: checksums, and later compression and encodings.
set(TB_CODEC_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/checksum.c"
    CACHE INTERNAL "Codec sources")
target_sources(badger PRIVATE ${TB_CODEC_SOURCES})
target_sources(badger_static PRIVATE ${TB_CODEC_SOURCES})
